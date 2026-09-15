#!/usr/bin/env python3
"""Extract the message layout tables from the Nasdaq TotalView-ITCH 5.0 spec.

Usage:  pdftotext -layout NQTVITCHspecification.pdf spec.txt
        python3 tools/extract_spec.py spec.txt tools/itch50_fields.json

Offsets and lengths come out of the PDF mechanically; they are the part that has
to be right. A handful of field names are truncated by the PDF's column width in
ways the wrap recovery below does not catch, and those are completed by the
override table in gen_wire.py, from the same spec text.
"""
import re, sys, json

def main(src, dst):
    txt = open(src, encoding='utf-8').read()
    for a, b in [('–','-'), ('—','-'), ('’',"'"), ('“','"'),
                 ('”','"'), ('-•-','-'), ('®','')]:
        txt = txt.replace(a, b)
    lines = txt.split('\n')

    named = re.compile(r"^(\s{0,14})([A-Za-z][A-Za-z0-9 /()'\-\.]*?)\s+(\d{1,3})\s+(\d{1,2})\s+(Alpha|Integer|Price)\b.*$")
    bare  = re.compile(r'^\s{2,}(\d{1,3})\s+(\d{1,2})\s+(Alpha|Integer|Price)\b.*$')
    typem = re.compile(r'^\s{0,14}Message Type\s{2,}0\s+1\s+"(.)"\s*(.*)$')
    cont  = re.compile(r"^(\s{0,14})([A-Za-z][A-Za-z0-9 /'\-]{0,32})\s*$")

    msgs, cur = [], None
    for i, line in enumerate(lines):
        m = typem.match(line)
        if m:
            cur = {'type': m.group(1), 'desc': m.group(2).strip(),
                   'fields': [['Message Type', 0, 1, 'Alpha']]}
            msgs.append(cur)
            continue
        if cur is None:
            continue
        exp = cur['fields'][-1][1] + cur['fields'][-1][2]

        hit, indent = None, 2
        m = named.match(line)
        if m and int(m.group(3)) == exp and m.group(2).strip().lower() != 'message type':
            hit = [m.group(2).strip(), int(m.group(3)), int(m.group(4)), m.group(5)]
            indent = len(m.group(1))
        else:
            m = bare.match(line)
            if m and int(m.group(1)) == exp:
                hit = ['', int(m.group(1)), int(m.group(2)), m.group(3)]
        if hit is None:
            continue

        for j in (i + 1, i + 2):
            if j >= len(lines):
                break
            c = cont.match(lines[j])
            if c and len(c.group(1)) <= indent + 2 and c.group(2).strip():
                hit[0] = (hit[0] + ' ' + c.group(2).strip()).strip()
            else:
                break
        cur['fields'].append(hit)

    for m in msgs:
        m['size'] = m['fields'][-1][1] + m['fields'][-1][2]
    json.dump(msgs, open(dst, 'w'), indent=1)
    print(f'{len(msgs)} message tables -> {dst}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
