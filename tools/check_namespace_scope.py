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


def main():
    raw = SOURCE.read_bytes()
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    lines = text.split('\n')
    start, end = namespace_range(lines)
    names = declared_names(lines, start, end)
    if not names:
        print('ERROR: no declarations found inside namespace %s' % NSPACE, file=sys.stderr)
        return 1

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

    if not problems and not bogus:
        print('OK: every %s symbol used outside the namespace is qualified or forwarded '
              '(%d names checked)' % (NSPACE, len(names)))
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
