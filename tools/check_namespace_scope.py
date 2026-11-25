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
    """Names the enclosing scope declares itself (the forwarders), which are fine."""
    names = set()
    depth = 0
    for i in range(0, start):
        stripped = lines[i].strip()
        if stripped and not stripped.startswith(('//', '*', '/*', '#', '}')):
            m = re.match(r'^(?:static\s+|inline\s+)*(?:[\w:<>]+[\s\*&,]+)+?\**&?(\w+)\s*[\(=;]', stripped)
            if m:
                names.add(m.group(1))
        depth += lines[i].count('{') - lines[i].count('}')
    for i in range(end + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped and not stripped.startswith(('//', '*', '/*', '#', '}')) and depth <= 1:
            m = re.match(r'^(?:static\s+|inline\s+)*(?:[\w:<>]+[\s\*&,]+)+?\**&?(\w+)\s*[\(=;]', stripped)
            if m:
                names.add(m.group(1))
        depth += lines[i].count('{') - lines[i].count('}')
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

    if not problems:
        print('OK: every %s symbol used outside the namespace is qualified or forwarded '
              '(%d names checked)' % (NSPACE, len(names)))
        return 0

    print('FAIL: declared inside namespace %s, used unqualified outside:' % NSPACE)
    for line, name, decl in sorted(problems):
        print('  dllmain.cpp(%d): %s   (declared at line %d)' % (line, name, decl))
    print('\nFix: qualify the call as %s::%s(...), or add a forwarder next to\n'
          'ForgetReplacementIndexImpl/RunReplacementSelfTest/NoteReplacementHit.'
          % (NSPACE, sorted({p[1] for p in problems})[0]))
    return 1


if __name__ == '__main__':
    sys.exit(main())
