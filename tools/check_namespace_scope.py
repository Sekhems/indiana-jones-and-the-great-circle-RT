#!/usr/bin/env python3
"""Catches "declared inside namespace spvrw, used unqualified outside" mistakes.

MSVC reports these one at a time (C2065 / C3861 / C2039), which costs a full
build round-trip each time. This script finds all of them at once, without MSVC:

    python tools/check_namespace_scope.py

Exit code 0 = clean, 1 = at least one symbol needs a `spvrw::` qualifier or a
forwarder (see ForgetReplacementIndexImpl / RunReplacementSelfTest /
NoteReplacementHit for the pattern used in dllmain.cpp).
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'dllmain.cpp'
NSPACE = 'spvrw'

# Names that legitimately appear in both scopes (CRT macros, own forwarders, ...
# still reported, but the list keeps the obvious noise out of the report).
NOISE = {
    'SRWLOCK_INIT', 'string', 'switch', 'sizeof', 'return',
}


def strip_noise(text):
    """Drops comments and string literals so brace counting stays honest."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c in '"\'':
            q = c
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def namespace_range(lines):
    start = next(i for i, l in enumerate(lines)
                 if l.startswith('namespace %s {' % NSPACE))
    depth = 0
    for j in range(start, len(lines)):
        depth += lines[j].count('{') - lines[j].count('}')
        if depth == 0:
            return start, j
    raise SystemExit('ERROR: namespace %s never closes' % NSPACE)


def declared_names(lines, start, end):
    """Top-level declarations of the namespace: functions, types and variables."""
    names = {}
    depth = 0
    for i in range(start, end + 1):
        line = lines[i]
        stripped = line.strip()
        if depth == 1 and stripped and not stripped.startswith(('//', '*', '/*', '#', '}')):
            for m in re.finditer(r'\b(g_\w+|k[A-Z]\w*)\b', stripped):
                names.setdefault(m.group(1), i + 1)
            m = re.match(r'^(?:static\s+|inline\s+|constexpr\s+|const\s+|volatile\s+)*'
                         r'(?:[\w:<>]+[\s\*&,]+)+?\**&?(\w+)\s*\(', stripped)
            if m:
                names.setdefault(m.group(1), i + 1)
            m = re.match(r'^(?:struct|class|enum(?:\s+class)?)\s+(\w+)', stripped)
            if m:
                names.setdefault(m.group(1), i + 1)
        depth += line.count('{') - line.count('}')
    return names


def outer_declared(lines, start, end):
    """Names the enclosing scope declares itself (the forwarders), which are fine.

    Only lines at brace depth 0 or 1 belong to the enclosing scopes; anything deeper is
    a local variable inside some function and must not count as a declaration.
    """
    names = set()
    decl_re = re.compile(r'^(?:static\s+|inline\s+)*(?:[\w:<>]+[\s\*&,]+)+?\**&?(\w+)\s*[\(=;]')

    def scan(chunk, depth):
        for line in chunk:
            stripped = line.strip()
            if 1 <= depth <= 2 and stripped and not stripped.startswith(('//', '*', '/*', '#', '}')):
                m = decl_re.match(stripped)
                if m:
                    names.add(m.group(1))
            depth += line.count('{') - line.count('}')

    # Before the namespace: start at depth 0.
    scan(lines[:start], 0)
    # After the namespace: it closes at depth 1 (the file-wide anonymous namespace).
    scan(lines[end + 1:], 1)
    return names


# ---------------------------------------------------------------------------
# Nested anonymous namespaces
# ---------------------------------------------------------------------------
# dllmain.cpp has a file-wide `namespace {` and, inside it, a second `namespace {`
# for the SPIR-V/SoftwareRT block. A symbol defined in the inner one is a DIFFERENT
# entity from a symbol declared in the outer one, so:
#   * `extern T x;` outside + `T x = 0;` inside  ->  MSVC C7631 (declared, not defined)
#   * a use inside that expects the outer x      ->  it silently sees nothing
# Both cost a build round-trip each. This check reports every global that exists in
# more than one anonymous namespace, or whose declaration and definition disagree.
ANON_NS = re.compile(r'^namespace\s*\{')


def anon_namespace_depths(lines):
    """Returns [(start_index, end_index, depth_factor)] for every anonymous namespace."""
    ranges = []
    depth = 0
    stack = []
    for i, line in enumerate(lines):
        code = strip_noise(line).strip()
        if ANON_NS.match(code):
            stack.append((i, depth))
        depth += line.count('{') - line.count('}')
        while stack and depth <= stack[-1][1]:
            start, d = stack.pop()
            ranges.append((start, i, d))
    return ranges


def find_split_globals(lines):
    """Reports globals that will not link or will silently be two objects.

    Only one case is a hard error: `extern T x;` inside an anonymous namespace whose
    definition lives in a DIFFERENT anonymous namespace (MSVC C7631 - the extern is
    never satisfied). Two same-named globals in two anonymous namespaces are legal C++
    and only reported as a note, because that is all they are.
    """
    ranges = anon_namespace_depths(lines)
    if not ranges:
        return [], []

    def owner(idx):
        best = None
        for start, end, _ in ranges:
            if start <= idx <= end:
                # innermost wins: the smallest enclosing range
                if best is None or (end - start) < (best[1] - best[0]):
                    best = (start, end)
        return best[0] if best else None

    decl_re = re.compile(r'^\s*(?:static\s+)?(?:extern\s+)?(?:volatile\s+|const\s+|constexpr\s+)*'
                         r'[\w:<>]+[\s\*]+(g_\w+)\s*(?:=|;)')
    externs = []     # (name, line, owner)
    definitions = {}  # name -> set(owner)
    lines_of = {}     # name -> first definition line
    for i, line in enumerate(lines):
        code = strip_noise(line)
        m = decl_re.match(code)
        if not m:
            continue
        name = m.group(1)
        if 'extern' in code:
            externs.append((name, i + 1, owner(i)))
        elif '=' in code or code.rstrip().endswith(';'):
            definitions.setdefault(name, set()).add(owner(i))
            lines_of.setdefault(name, i + 1)

    errors = []
    for name, line, own in externs:
        if name not in definitions:
            errors.append(('extern-without-definition', line, name, None))
        elif own not in definitions[name]:
            errors.append(('extern-in-other-namespace', line, name, lines_of.get(name)))

    notes = []
    for name, owners in definitions.items():
        if len(owners) > 1:
            notes.append(('two objects', name, sorted(o + 1 for o in owners if o is not None)))
    return errors, notes


def main():
    raw = SOURCE.read_bytes()
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    lines = text.split('\n')
    start, end = namespace_range(lines)
    names = declared_names(lines, start, end)
    if not names:
        print('ERROR: no declarations found inside namespace %s' % NSPACE, file=sys.stderr)
        return 1

    split_errors, split_notes = find_split_globals(lines)

    forwarded = outer_declared(lines, start, end)
    problems = []
    for scope_lines, offset in ((lines[:start], 0), (lines[end + 1:], end + 1)):
        for i, line in enumerate(scope_lines):
            code = strip_noise(line)
            for name in names:
                if len(name) < 4 or name in NOISE or name in forwarded:
                    continue
                if name not in code:
                    continue
                if re.search(r'\b%s::%s\b' % (NSPACE, re.escape(name)), code):
                    continue
                if re.search(r'(?<![\w.:>])%s\b' % re.escape(name), code):
                    problems.append((i + offset + 1, name, names[name]))

    # The opposite mistake, and the one that cost two MSVC errors in one build:
    # writing spvrw::Something() for a function that is NOT in spvrw (C2039). The check
    # above cannot see it, because it only looks at names that ARE declared inside.
    bogus = []
    for i, line in enumerate(lines):
        code = strip_noise(line)
        for m in re.finditer(r'\b%s::(\w+)\b' % NSPACE, code):
            if m.group(1) not in names:
                bogus.append((i + 1, m.group(1)))

    for kind, line, name, other in split_errors:
        if kind == 'extern-without-definition':
            print('  dllmain.cpp(%d): %s declared extern but never defined (MSVC C7631)' % (line, name))
        else:
            print('  dllmain.cpp(%d): %s is extern here but defined at line %s in another '
                  'anonymous namespace (MSVC C7631)' % (line, name, other))
    if split_errors:
        print('FAIL: globals split across anonymous namespaces:')
        print('\nFix: define the global in the same anonymous namespace as the extern, or\n'
              'put a single definition next to the code that sets it.')
    for kind, name, owners in split_notes:
        print('note: %s is defined twice, in anonymous namespaces starting at lines %s '
              '(legal, but they are two different objects)' % (name, ', '.join(map(str, owners))))

    if not problems and not bogus and not split_errors:
        print('OK: every %s symbol used outside the namespace is qualified or forwarded '
              '(%s names checked), and no global is split across anonymous namespaces'
              % (NSPACE, len(names)))
        return 0

    if bogus:
        print('FAIL: qualified as %s::X but X does not exist inside namespace %s:' % (NSPACE, NSPACE))
        for line, name in bogus:
            print('  dllmain.cpp(%d): %s::%s' % (line, NSPACE, name))
        print('\nFix: the function lives in the outer anonymous namespace - drop the\n'
              'qualifier, or move/forward it. (MSVC reports this as C2039.)')
    if problems:
        print('FAIL: declared inside namespace %s, used unqualified outside:' % NSPACE)
        for line, name, decl in sorted(problems):
            print('  dllmain.cpp(%d): %s   (declared at line %d)' % (line, name, decl))
        print('\nFix: qualify the call as %s::%s(...), or add a forwarder next to\n'
              'ForgetReplacementIndexImpl/RunReplacementSelfTest/NoteReplacementHit.'
              % (NSPACE, sorted({p[1] for p in problems})[0]))
    return 1



if __name__ == '__main__':
    sys.exit(main())
