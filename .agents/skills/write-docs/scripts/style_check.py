#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check Markdown docs against the BirriaSDR writing rules."""
import argparse
import re
import sys

BAD_START = re.compile(r'^(\[|<?https?:|`|\d|[A-Z][A-Z0-9\/]+\b|[A-Z]+\d)')
EMBED = re.compile(r'^https://github\.com/user-attachments/assets/\S+$')
SENTENCE_SPLIT = re.compile(r'(?<=[.!?])\s+(?=\S)')
FORBIDDEN = (('—', 'em dash'), ('–', 'en dash'), (';', 'semicolon'))


def strip_fences(text):
    """Blank out fenced code blocks while keeping line numbers."""
    return re.sub(r'```.*?```', lambda m: '\n' * m.group(0).count('\n'),
                  text, flags=re.S)


def check(path, strict):
    """Return a list of problems found in one file."""
    problems = []
    text = strip_fences(open(path, encoding='utf-8').read())
    lines = text.split('\n')
    for number, line in enumerate(lines, 1):
        if line.startswith('#'):
            continue
        prose = re.sub(r'`[^`]*`', 'CODE', line)
        for char, name in FORBIDDEN:
            if char in prose:
                problems.append(f'{path}:{number}: {name}: {line.strip()[:100]}')

    number = 0
    while number < len(lines):
        line = lines[number]
        stripped = line.strip()
        if not stripped or stripped.startswith('#') or EMBED.match(stripped):
            number += 1
            continue
        if stripped.startswith('|'):
            if not re.match(r'^\|[\s:|-]+\|?$', stripped):
                cells = [c.strip() for c in stripped.strip('|').split('|')]
                for cell in cells[1:]:
                    sentences = SENTENCE_SPLIT.split(cell)
                    if not strict:
                        sentences = sentences[1:]
                    for sentence in sentences:
                        if BAD_START.match(sentence.lstrip('*')):
                            problems.append(f'{path}:{number + 1}: start: {sentence[:100]}')
            number += 1
            continue
        start = number
        paragraph = []
        while number < len(lines) and lines[number].strip() \
                and not lines[number].strip().startswith(('#', '|')):
            quoted = re.sub(r'^\s*>\s?', '', lines[number])
            quoted = re.sub(r'^\[![A-Z]+\]\s*', '', quoted.strip())
            item = re.sub(r'^\s*(?:[-*]|\d+\.)\s+', '', quoted)
            if item != quoted and paragraph:
                paragraph.append('\n')
            paragraph.append(item.strip())
            number += 1
        for chunk in ' '.join(paragraph).split('\n'):
            for sentence in SENTENCE_SPLIT.split(chunk.strip()):
                sentence = sentence.strip().lstrip('*')
                if sentence and BAD_START.match(sentence):
                    problems.append(f'{path}:{start + 1}: start: {sentence[:100]}')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--strict', action='store_true',
                        help='Also check the leading value of each table cell')
    args = parser.parse_args()
    problems = [p for path in args.files for p in check(path, args.strict)]
    print('\n'.join(problems) if problems else 'No style problems found.')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
