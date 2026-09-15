#!/usr/bin/env python3
import re, glob, sys

bad = []
for path in glob.glob('include/itch/**/*.hpp', recursive=True):
    src = open(path).read()
    for m in re.finditer(
            r'^[ \t]*(?:\[\[nodiscard\]\]\s*)?[\w:<>,&*\s]+?\b(\w+)\([^;{]*\)\s*'
            r'(?:const\s*)?noexcept\s*\{', src, re.M):
        start = m.end() - 1
        depth, i = 0, start
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if 'ITCH_ASSERT' in src[start:i]:
            line = src[:m.start()].count('\n') + 1
            bad.append(f'{path}:{line}: {m.group(1)}() is noexcept and asserts')

if bad:
    print('\n'.join(bad), file=sys.stderr)
    print('error: a function that asserts must not be noexcept '
          '(the test assert handler throws)', file=sys.stderr)
    sys.exit(1)
print('no noexcept function contains an assertion: ok')
