"""Prove a diff touches only comments: every changed C++ file has the same code tokens before and after.

Usage: python scripts/comment_only_check.py <base-ref> [<head-ref>]   (head defaults to the work tree)

Strips // and /* */ comments (string and character literals respected), collapses whitespace, and
compares the remaining text per file. Exit 0 when every changed .cpp/.hpp/.h file is equal; lists the
files that differ otherwise.
"""
import re
import subprocess
import sys


def strip_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        elif c == 'R' and src.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m:
                end = src.find(')' + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append(src[i:end]); i = end
            else:
                out.append(c); i += 1
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1]); i = j + 1
        else:
            out.append(c); i += 1
    return re.sub(r'\s+', ' ', ''.join(out)).strip()


def blob(ref, path):
    r = subprocess.run(['git', 'show', f'{ref}:{path}'], capture_output=True)
    return r.stdout.decode('utf-8', 'surrogateescape') if r.returncode == 0 else None


def main():
    base = sys.argv[1]
    head = sys.argv[2] if len(sys.argv) > 2 else None
    args = ['git', 'diff', '--name-only', base] + ([head] if head else [])
    files = [f for f in subprocess.run(args, capture_output=True, text=True).stdout.split()
             if f.endswith(('.cpp', '.hpp', '.h'))]
    bad = []
    for f in files:
        old = blob(base, f)
        new = blob(head, f) if head else open(f, encoding='utf-8', errors='surrogateescape').read()
        if old is None or new is None or strip_comments(old) != strip_comments(new):
            bad.append(f)
    print(f'comment_only_check: {len(files)} files, {len(bad)} with a code change')
    for f in bad:
        print('  CODE CHANGED:', f)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
