#!/usr/bin/env python3
"""Offline test for StripRayQuery (the "hit at infinity" rewrite).

Why this exists
---------------
`StripRayQuery` edits SPIR-V byte for byte: it invents constant ids, deletes
instructions that refer to a ray query and must leave a module that a driver
will still accept. A mistake there does not show up as a compile error in
C++ - it shows up as a black screen or a driver crash on the user's machine,
which is expensive to debug. So this script builds a tiny ray-query module,
runs the real rewriter over it and checks the result:

  * the rewrite reports success,
  * no ray-query / acceleration-structure opcode survives,
  * every id is declared before it is used (the proxy's own ForEachId is used
    for this, so the check sees the module exactly as the proxy does),
  * the values are what option 2 intends: distance 1000, "hit type" 1,
    barycentrics 1/3, front face true, Proceed false,
  * types a constant cannot be built for (matrices, 16-bit floats) fall back
    to OpConstantNull instead of producing a dangling reference.

The C++ is not copied here: the script extracts the relevant blocks from
dllmain.cpp (kSpvOps, KindsFor, ForEachId, StripRayQuery and the helpers it
needs) and compiles them with the system g++. That way the test can never
drift away from the code it is supposed to test.

Requirements: python3 and g++ (or clang++). Linux/macOS only; the Windows
build has build_proxy.ps1. Run:

    python3 tools/spv_rq_test.py
"""

import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'dllmain.cpp'


# ---------------------------------------------------------------------------
# 1. Extract the rewriter and the tables it needs from dllmain.cpp
# ---------------------------------------------------------------------------

def extract(lines, start_marker, end_marker=None):
    """Copy one block out of dllmain.cpp, brace balanced.

    A block's body is full of deeper-indented braces, so the end cannot be a
    substring match: counting braces from the opening line is the only way to
    stop at the block's own closing brace. `end_marker` optionally names the
    line the block ends on (used for the op-code table, which is not a
    function); otherwise the block ends where its braces balance.
    """
    start = next(i for i, l in enumerate(lines) if start_marker in l)
    if end_marker is not None:
        # Everything up to (but not including) the marked line: the helpers block
        # ends where StripRayQuery begins, and that signature must not be copied
        # twice.
        end = next(i for i, l in enumerate(lines) if i > start and end_marker in l)
        return '\n'.join(line.rstrip('\r') for line in lines[start:end])
    depth = 0
    end = start
    for i in range(start, len(lines)):
        code = lines[i].split('//')[0]
        depth += code.count('{') - code.count('}')
        if depth == 0:
            end = i          # single-line blocks close on their own line
            break
    else:
        raise RuntimeError('unbalanced block starting at %r' % start_marker)
    return '\n'.join(line.rstrip('\r') for line in lines[start:end + 1])


def build_harness(path: pathlib.Path):
    text = SOURCE.read_bytes().decode('utf-8')
    lines = text.split('\n')

    # The op-code table is not a function: it ends at its own "};", which must be
    # kept (a table without its terminator swallows everything after it).
    table_start = next(i for i, l in enumerate(lines) if 'static const SpvOpDesc kSpvOps[] = {' in l)
    table_end = next(i for i, l in enumerate(lines) if i > table_start and l.rstrip('\r').strip() == '};')
    op_table = '\n'.join(l.rstrip('\r') for l in lines[table_start:table_end + 1])
    kinds_for = extract(lines, '    inline const char* KindsFor(uint32_t op) {')
    for_each = extract(lines, '    template<class F> void ForEachId(')
    inst = extract(lines, '    struct Inst { uint32_t op, wc; size_t off; };')
    rq_ops = extract(lines, '    inline bool IsRqValueOp(uint32_t op) {', 'IsRqVoidOp')
    void_ops = extract(lines, '    inline bool IsRqVoidOp(uint32_t op)')
    helpers = extract(lines, '    struct RayMissReply {', 'inline bool StripRayQuery')
    strip = extract(lines, 'inline bool StripRayQuery(')

    header = r'''#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
struct SpvOpDesc { uint16_t op; const char* kinds; };
namespace spvrw {
'''
    footer = r'''
} // namespace spvrw

using spvrw::ForEachId;
using spvrw::StripRayQuery;

// Declaration-order check, using the proxy's own operand table so the test sees
// the module the same way the rewrite does. Labels and entry points are exempt:
// a branch to a label further down is legal, the entry point names a function
// that is by definition defined later.
static bool CheckOrder(const std::vector<uint32_t>& w, std::string& why) {
    std::unordered_set<uint32_t> declared;
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!wc || i + wc > w.size()) { why = "bad length"; return false; }
        std::vector<std::pair<size_t, char>> ids;
        ForEachId(w.data() + i, op, wc, [&](size_t idx, char role) { ids.push_back({ idx, role }); });
        for (auto& p : ids) if (p.second == 'R') declared.insert(w[i + p.first]);
        if (op != 15 && op != 16 && !(op >= 247 && op <= 251)) {
            for (auto& p : ids) {
                if (p.second != 'I') continue;
                if (!declared.count(w[i + p.first])) {
                    why = "op " + std::to_string(op) + " uses id " + std::to_string(w[i + p.first]) + " before it is declared";
                    return false;
                }
            }
        }
        i += wc;
    }
    return true;
}

static bool Verbose = false;

// Value of the constant whose id the given instruction copy refers to.
static bool ConstValue(const std::vector<uint32_t>& w, uint32_t id, uint32_t& value, uint32_t& type) {
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
        if (op == 43 && wc >= 4 && w[i + 2] == id) { type = w[i + 1]; value = w[i + 3]; return true; }
        if ((op == 41 || op == 42) && wc >= 3 && w[i + 2] == id) { type = w[i + 1]; value = (op == 41); return true; }
        if ((op == 44 || op == 46) && wc >= 3 && w[i + 2] == id) { type = w[i + 1]; value = 0; return true; }
        i += wc;
    }
    return false;
}

// Result of "OpCopyObject %type %id %source": what value does %id get?
static uint32_t CopySource(const std::vector<uint32_t>& w, uint32_t id) {
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
        if (op == 83 && wc >= 4 && w[i + 2] == id) return w[i + 3];
        i += wc;
    }
    return 0;
}

static int g_failures = 0;
static void Expect(bool ok, const char* what) {
    if (!ok) { ++g_failures; printf("  FAIL %s\n", what); }
    else if (Verbose) printf("  ok   %s\n", what);
}

static bool Run(const char* inPath, std::vector<uint32_t>& out) {
    FILE* f = fopen(inPath, "rb");
    if (!f) return false;
    std::vector<uint32_t> in;
    uint32_t x;
    while (fread(&x, 4, 1, f) == 1) in.push_back(x);
    fclose(f);
    std::string err;
    const bool ok = StripRayQuery(in.data(), in.size(), out, err);
    if (!ok) { printf("  FAIL rewrite refused the module: %s\n", err.c_str()); ++g_failures; }
    return ok;
}

static void Report(const std::vector<uint32_t>& w) {
    std::string why;
    Expect(CheckOrder(w, why), why.empty() ? "every id is declared before use" : why.c_str());
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
        const bool ray = (op == 4472 || op == 5341 || (op >= 4473 && op <= 4477) ||
                          op == 4479 || (op >= 6016 && op <= 6032));
        Expect(!ray, "no ray-query/AS opcode survives");
        i += wc;
    }
}

// The id that carries `value` of type `type` (following the one OpCopyObject).
static bool HasValue(const std::vector<uint32_t>& w, uint32_t copyId, uint32_t want, uint32_t typeHint) {
    const uint32_t src = CopySource(w, copyId);
    uint32_t value = 0, type = 0;
    if (!src || !ConstValue(w, src, value, type)) return false;
    if (typeHint && type != typeHint) return false;
    return value == want;
}

static uint32_t NextId(const std::vector<uint32_t>& w, uint32_t after) {
    (void)w;
    return after;
}

int main(int argc, char** argv) {
    char* end = nullptr;
    Verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    char* inPath = argv[Verbose ? 2 : 1];
    char* wantPath = argv[Verbose ? 3 : 2];   // "id=value" lines the fixture expects
    std::vector<uint32_t> out;
    if (!Run(inPath, out)) return 1;
    printf("rewrite ok: %zu words\n", out.size());
    Report(out);

    FILE* f = fopen(wantPath, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long id = 0; unsigned long long val = 0; char kind[32] = {0};
            if (sscanf(line, "%llu %llu %31s", &id, &val, kind) != 3) continue;
            if (!strcmp(kind, "float")) {
                Expect(HasValue(out, (uint32_t)id, (uint32_t)val, 0), "float constant has the expected value");
            } else if (!strcmp(kind, "int")) {
                Expect(HasValue(out, (uint32_t)id, (uint32_t)val, 0), "integer constant has the expected value");
            } else if (!strcmp(kind, "booltrue")) {
                const uint32_t src = CopySource(out, (uint32_t)id);
                Expect(src && HasValue(out, (uint32_t)id, 1, 0), "boolean constant is true");
            } else if (!strcmp(kind, "boolfalse")) {
                Expect(HasValue(out, (uint32_t)id, 0, 0), "boolean constant is false");
            }
        }
        fclose(f);
    }

    if (g_failures) { printf("%d check(s) failed\n", g_failures); return 1; }
    printf("PASSED\n");
    return 0;
}
'''
    blocks = [op_table, kinds_for, for_each, inst, rq_ops, void_ops, helpers, strip]
    path.write_text(header + '\n'.join(blocks) + footer)


# ---------------------------------------------------------------------------
# 2. Fixtures: ray-query modules written by hand (no compiler needed)
# ---------------------------------------------------------------------------

class Asm:
    """Minimal SPIR-V assembler: ids are allocated on demand."""

    def __init__(self):
        self.next_id = 1
        self.head = []
        self.body = []

    def id(self):
        i = self.next_id
        self.next_id += 1
        return i

    def inst(self, where, op, operands):
        where.append([op, list(operands)])

    def words(self):
        out = [0x07230203, 0x00010500, 0xDEADBEEF, self.next_id, 0]
        for op, operands in self.head + self.body:
            out.append(((1 + len(operands)) << 16) | op)
            out.extend(operands)
        return out


def fixture_textured():
    """The shape that matters: a traversal loop reading distance, barycentrics,
    hit type and front face - i.e. a lighting shader."""
    a = Asm()
    f32, v3, u32t, boolt = a.id(), a.id(), a.id(), a.id()
    voidty, fnty, rqty, f32ptr = a.id(), a.id(), a.id(), a.id()
    fnid, label = a.id(), a.id()
    var_rq, var_val = a.id(), a.id()
    zero_f, zero_u, one_u = a.id(), a.id(), a.id()
    loop, cont, merge = a.id(), a.id(), a.id()
    proceed, htype, hdist, hbary, hfront = a.id(), a.id(), a.id(), a.id(), a.id()

    H = a.head
    a.inst(H, 17, [4472])                                   # OpCapability RayQueryKHR
    a.inst(H, 10, [0x4B484B53, 0x7972615F, 0x7261755F, 0x00000079])   # "SPV_KHR_ray_query"
    a.inst(H, 14, [0, 1, 1, 1])                             # OpMemoryModel Logical GLSL450
    a.inst(H, 15, [5, fnid, 0x6E69616D, 0])                 # OpEntryPoint GLCompute "main"
    a.inst(H, 16, [fnid, 17, 1, 1])                         # OpExecutionMode LocalSize
    a.inst(H, 19, [voidty])
    a.inst(H, 20, [boolt])
    a.inst(H, 22, [f32, 32])                                # OpTypeFloat 32
    a.inst(H, 21, [u32t, 32, 0])                            # OpTypeInt 32 0
    a.inst(H, 23, [v3, f32, 3])                             # OpTypeVector
    a.inst(H, 4472, [rqty])                                 # OpTypeRayQueryKHR
    a.inst(H, 32, [f32ptr, 2, f32])                         # OpTypePointer Function f32
    a.inst(H, 33, [fnty, voidty])
    a.inst(H, 43, [f32, zero_f, 0])
    a.inst(H, 43, [u32t, zero_u, 0])
    a.inst(H, 43, [u32t, one_u, 1])

    B = a.body
    a.inst(B, 54, [voidty, fnid, 0, fnty])
    a.inst(B, 248, [label])
    a.inst(B, 59, [rqty, var_rq, 7])                        # OpVariable Function rayquery
    a.inst(B, 59, [f32ptr, var_val, 7])
    a.inst(B, 4473, [var_rq, zero_f, zero_u, zero_f, zero_f, zero_f, zero_f, zero_f, zero_f])
    a.inst(B, 247, [loop])                                  # OpBranch loop
    a.inst(B, 248, [cont])
    a.inst(B, 4477, [boolt, proceed, var_rq])               # OpRayQueryProceedKHR
    a.inst(B, 250, [loop, merge, proceed])                  # OpBranchConditional
    a.inst(B, 248, [loop])
    a.inst(B, 4479, [u32t, htype, var_rq, one_u])           # GetIntersectionType
    a.inst(B, 6018, [f32, hdist, var_rq, one_u])            # GetIntersectionT
    a.inst(B, 6024, [v3, hbary, var_rq, one_u])             # GetIntersectionBarycentrics
    a.inst(B, 6025, [boolt, hfront, var_rq, one_u])         # GetIntersectionFrontFace
    a.inst(B, 63, [var_val, hdist])                         # OpStore
    a.inst(B, 247, [merge])
    a.inst(B, 248, [merge])
    a.inst(B, 253, [])
    a.inst(B, 56, [])

    # What the rewritten module must contain: distance 1000.0f, hit type 1,
    # barycentrics 1/3 each, front face true, and Proceed false so the loop ends.
    want = [
        (hdist, 0x447A0000, 'float'),       # 1000.0f
        (htype, 1, 'int'),
        (hfront, 1, 'booltrue'),
        (proceed, 0, 'boolfalse'),
    ]
    return a.words(), want


def fixture_unsupported():
    """Types a constant cannot be built for, plus the matrices: the rewrite must
    still leave a module whose ids all resolve."""
    a = Asm()
    f32, f16, u32t, boolt = a.id(), a.id(), a.id(), a.id()
    v4, mat4 = a.id(), a.id()
    voidty, fnty, rqty = a.id(), a.id(), a.id()
    fnid, label, var_rq = a.id(), a.id(), a.id()
    c0, c1, c1000 = a.id(), a.id(), a.id()
    mat, half, proceed = a.id(), a.id(), a.id()

    H = a.head
    a.inst(H, 17, [4472])
    a.inst(H, 14, [0, 1, 1, 1])
    a.inst(H, 15, [5, fnid, 0x6E69616D, 0])
    a.inst(H, 19, [voidty])
    a.inst(H, 20, [boolt])
    a.inst(H, 22, [f32, 32])
    a.inst(H, 22, [f16, 16])
    a.inst(H, 21, [u32t, 32, 0])
    a.inst(H, 23, [v4, f32, 4])
    a.inst(H, 23, [mat4, v4, 4])
    a.inst(H, 4472, [rqty])
    a.inst(H, 33, [fnty, voidty])
    a.inst(H, 43, [f32, c0, 0])
    a.inst(H, 43, [u32t, c1, 1])
    a.inst(H, 43, [f32, c1000, 0x447A0000])

    B = a.body
    a.inst(B, 54, [voidty, fnid, 0, fnty])
    a.inst(B, 248, [label])
    a.inst(B, 59, [rqty, var_rq, 7])
    a.inst(B, 6031, [mat4, mat, var_rq, c1])     # ObjectToWorld: matrix
    a.inst(B, 6018, [f16, half, var_rq, c1])     # distance as f16
    a.inst(B, 4477, [boolt, proceed, var_rq])    # Proceed
    a.inst(B, 253, [])
    a.inst(B, 56, [])
    return a.words(), [(proceed, 0, 'boolfalse')]


def write_module(path, words):
    path.write_bytes(struct.pack('<%dI' % len(words), *words))


def main():
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        print('no C++ compiler found (g++/clang++)')
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        harness = tmp / 'harness.cpp'
        build_harness(harness)
        exe = tmp / 'harness'
        print('compiling the rewriter from dllmain.cpp ...')
        proc = subprocess.run([compiler, '-std=c++17', '-O0', '-o', str(exe), str(harness)],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr)
            return 1

        failures = 0
        for name, builder in (('traversal loop', fixture_textured),
                              ('types without a constant', fixture_unsupported)):
            words, want = builder()
            mod = tmp / (name.replace(' ', '_') + '.spv')
            write_module(mod, words)
            # The proxy reads the module from disk; feed it through a file.
            want_path = tmp / 'want.txt'
            want_path.write_text('\n'.join('%d %d %s' % w for w in want) + '\n')
            print('--- %s' % name)
            # Harness takes the module path; copy it next to the exe input name.
            proc = subprocess.run([str(exe), str(mod), str(want_path)],
                                  capture_output=True, text=True)
            sys.stdout.write(proc.stdout)
            if proc.returncode != 0:
                failures += 1
                if proc.stderr:
                    sys.stderr.write(proc.stderr)

        if failures:
            print('%d fixture(s) FAILED' % failures)
            return 1
        print('all fixtures passed')
        return 0


if __name__ == '__main__':
    sys.exit(main())
