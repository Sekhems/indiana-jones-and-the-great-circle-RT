#!/usr/bin/env python3
"""Regenerates spv_check.cpp from dllmain.cpp.

spv_check.exe is a tiny standalone tool that reads the spv_replace folder next to
it, rebuilds every module the way another GPU's driver would (new ids, shuffled
instruction order, stripped debug names, different size) and reports whether the
proxy's matching code still recognises it. It exists so that the portability of
the folder can be verified on the machine that is available, without building the
proxy DLL and without starting the game.

The matching code is copied verbatim from dllmain.cpp - do not edit spv_check.cpp
by hand, edit dllmain.cpp and re-run this script:

    python tools/make_spv_check.py
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'dllmain.cpp'
TARGET = ROOT / 'spv_check.cpp'

# Markers in dllmain.cpp. Every block is copied unchanged so the tool cannot
# drift away from the real implementation.
REQUIRED_MARKERS = [
    'static const SpvOpDesc kSpvOps[] = {',
    '    inline const char* KindsFor(uint32_t op) {',
    '    struct Inst { uint32_t op, wc; size_t off; };',
    '    inline bool IsRayTracingCapability(uint32_t cap) {',
    '    // Universal replacement lookup.',
    '    void BuildReplacementIndexLocked() {',
    '    inline void RenumberAndShuffle(',
    '    ReplacementIndex* EnsureReplacementIndex() {',
]

HEADER = r'''// ---------------------------------------------------------------------------
// spv_check.exe - standalone portability check for the spv_replace folder.
//
// GENERATED FILE - do not edit. Change dllmain.cpp and run:
//     python tools/make_spv_check.py
//
// It uses the SAME matching code as the proxy, so what it reports is exactly
// what the game will see on a machine it has never run on.
//
// Build:  build_spv_check.bat   (needs MSVC only, no Vulkan SDK)
// Usage:  spv_check.exe ["D:\path\to\spv_replace"]
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

// Nothing is stubbed here on purpose: sprintf_s, _snprintf_s, SRWLOCK,
// SRWLOCK_INIT and the Interlocked* helpers all come from the Windows headers,
// exactly as they do in the proxy itself. Redefining them breaks the build
// (C2556/C2371/C2491/C2169).

// Diagnostics: same role as in the DLL, but written to the console.
static void LogLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

struct SpvOpDesc { uint16_t op; const char* kinds; };

namespace spvrw {
'''

MAIN = r'''}

using namespace spvrw;

// ---- file helpers ---------------------------------------------------------
static bool ReadSpvFile(const std::wstring& path, std::vector<uint32_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 20 && sz.QuadPart < (64ll << 20) && (sz.QuadPart % 4) == 0) {
        out.resize(static_cast<size_t>(sz.QuadPart / 4));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart && out[0] == 0x07230203u;
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

static const char* StageName(uint32_t model) {
    switch (model) {
    case 0: return "vertex";
    case 1: return "tess control";
    case 2: return "tess eval";
    case 3: return "geometry";
    case 4: return "fragment";
    case 5: return "compute";
    case 5267: return "task";
    case 5268: return "mesh";
    default: return "unknown";
    }
}

static size_t CountNames(const std::string& names) {
    size_t n = 0;
    for (char c : names) if (c == '|') ++n;
    return n;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(65001);
    wchar_t dir[MAX_PATH * 2]{};
    if (argc > 1) {
        wcscpy_s(dir, argv[1]);
    } else {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(dir, L"spv_replace");
    }
    printf("=== spv_replace portability check ===\n");
    printf("folder: %ls\n\n", dir);

    std::vector<ReplacementEntry> entries;
    size_t files = 0, unreadable = 0;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = std::wstring(dir) + L"\\spv_*.spv";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("ERROR: no spv_*.spv files in that folder.\n");
        printf("       Check the path and that files are named spv_<16 hex digits>.spv\n");
        printf("\nPress Enter to close...");
        (void)getchar();
        return 2;
    }
    do {
        ++files;
        const std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
        std::vector<uint32_t> words;
        if (!ReadSpvFile(path, words)) {
            ++unreadable;
            printf("  [bad ] %-30ls not readable as SPIR-V (magic 0x07230203, size % 4 == 0)\n", fd.cFileName);
            continue;
        }
        ReplacementFingerprint fp;
        if (!spvrw::ExtractFingerprint(words.data(), words.size(), fp)) {
            ++unreadable;
            printf("  [bad ] %-30ls unparsable module\n", fd.cFileName);
            continue;
        }
        printf("  [ok  ] %-30ls %-14s %6zu words, %2zu descriptors, %2zu names\n",
            fd.cFileName, StageName(fp.stage), fp.words, fp.interfaceCount, CountNames(fp.names));
        ReplacementEntry e;
        e.path = path;
        e.stage = fp.stage;
        e.fp = std::move(fp);
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    printf("\nread %zu file(s): %zu unusable, %zu usable for matching\n\n",
        files, unreadable, entries.size());
    if (entries.empty()) {
        printf("=== RESULT ===\nFAIL: nothing in this folder can be used.\n");
        printf("\nPress Enter to close...");
        (void)getchar();
        return 3;
    }

    // The real test: rebuild every module the way another driver would and require
    // the matcher to find the original file again.
    size_t pass = 0, fail = 0, namedPass = 0;
    printf("rebuilding each file the way another driver would\n");
    printf("(new ids, shuffled instruction order, stripped names, different size)\n\n");
    for (size_t k = 0; k < entries.size(); ++k) {
        std::vector<uint32_t> words;
        if (!ReadSpvFile(entries[k].path, words)) continue;

        std::vector<uint32_t> stripped, named;
        RenumberAndShuffle(words.data(), words.size(), static_cast<uint32_t>(0x1234u + k), true, stripped);
        RenumberAndShuffle(words.data(), words.size(), static_cast<uint32_t>(0x777u + k), false, named);

        ReplacementFingerprint fa, fb;
        const char* la = nullptr;
        const char* lb = nullptr;
        bool ok1 = false, ok2 = false;
        if (spvrw::ExtractFingerprint(stripped.data(), stripped.size(), fa)) {
            const std::wstring* p = BestSimilarityMatch(entries, fa, &la);
            ok1 = p && *p == entries[k].path;
        }
        if (spvrw::ExtractFingerprint(named.data(), named.size(), fb)) {
            const std::wstring* p = BestSimilarityMatch(entries, fb, &lb);
            ok2 = p && *p == entries[k].path;
        }
        if (ok1) ++pass; else ++fail;
        if (ok2) ++namedPass;

        const wchar_t* name = wcsrchr(entries[k].path.c_str(), L'\\');
        printf("  %-30ls stripped+shuffled: %-6s (%-10s)  named+shuffled: %-6s (%s)\n",
            name ? name + 1 : entries[k].path.c_str(),
            ok1 ? "FOUND" : "MISSED", la ? la : "-",
            ok2 ? "FOUND" : "MISSED", lb ? lb : "-");
    }

    printf("\n=== RESULT ===\n");
    if (fail == 0) {
        printf("PASS: %zu/%zu replacement(s) are recognised after being rebuilt the way\n", pass, pass + fail);
        printf("      another GPU's driver would rebuild them.\n");
        printf("      => this folder is portable and will be picked up on NVIDIA/Intel too.\n");
    } else {
        printf("FAIL: %zu of %zu replacement(s) were NOT recognised (%zu/%zu with names).\n",
            fail, pass + fail, namedPass, pass + fail);
        printf("      Send this text (or a screenshot of this window).\n");
    }
    printf("\nPress Enter to close...");
    (void)getchar();
    return fail == 0 ? 0 : 1;
}
'''


def block(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def strip_trailing_namespace(text):
    """Drops the namespace-closing brace `}` that sits at the very end of a block.

    Only a line that consists of a single '}' at column 0 is removed, so the closing
    brace of a function indented by four spaces is never touched.
    """
    stripped = text.rstrip()
    lines = stripped.split('\n')
    if lines and lines[-1] == '}':
        lines.pop()
    return '\n'.join(lines) + '\n'


def main():
    with open(SOURCE, 'r', newline='') as fh:
        text = fh.read().replace('\r\n', '\n')
    missing = [m for m in REQUIRED_MARKERS if m not in text]
    if missing:
        print('ERROR: dllmain.cpp no longer contains these markers:', file=sys.stderr)
        for m in missing:
            print('  ' + m, file=sys.stderr)
        print('Update tools/make_spv_check.py to match the new code layout.', file=sys.stderr)
        return 1

    table = block(text, 'static const SpvOpDesc kSpvOps[] = {', '\n};') + '\n};\n'
    helpers = block(text, '    inline const char* KindsFor(uint32_t op) {',
                    '    struct Inst { uint32_t op, wc; size_t off; };')
    # IsRayTracingCapability .. StructuralSignature (inclusive) and the ray-ops helper.
    inline_helpers = block(text, '    inline bool IsRayTracingCapability(uint32_t cap) {',
                           '    inline std::string StructuralSignature(')
    structural = block(text, '    inline std::string StructuralSignature(',
                       '\n    }\n\n    // -----------------------------------------------------------------------\n    // Universal replacement lookup.') + '\n    }\n'
    analysis = inline_helpers + structural
    analysis = strip_trailing_namespace(analysis)
    lookup = block(text, '    // Universal replacement lookup.',
                   '    void BuildReplacementIndexLocked() {')
    selftest = block(text, '    inline void RenumberAndShuffle(',
                     '    ReplacementIndex* EnsureReplacementIndex() {')
    # Drop the self-test driver itself: the tool has its own reporting. RenumberAndShuffle
    # is the only piece it needs. Anchoring on the next definition keeps this exact.
    cutoff = selftest.index('    void RunReplacementSelfTest(int mode) {')
    # keep everything up to and including the closing brace of RenumberAndShuffle
    tail = selftest[:cutoff].rstrip()
    assert tail.endswith('}'), 'RenumberAndShuffle block does not end with a brace'
    selftest = tail + '\n'

    body = table + '\n' + helpers + '\n' + analysis + '\n' + lookup + '\n' + selftest
    # MAIN opens with '}' that closes this namespace.
    body = strip_trailing_namespace(body)
    out = HEADER + body + MAIN
    with open(TARGET, 'w', newline='') as fh:
        fh.write(out.replace('\n', '\r\n'))
    print('wrote %s (%d bytes)' % (TARGET.name, len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
