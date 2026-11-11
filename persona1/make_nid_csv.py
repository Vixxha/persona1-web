"""Names the 193 Persona 1 imports.

Needs, in C:\\PSPRecomp\\PSPRecomp\\persona1\\ :
    report_imports.csv   (made by psp_analyze)
    nid_resolve.py       (the NID map from the SiD3W4y gist, saved from its Raw link)

Run from the repo root:   python persona1\\make_nid_csv.py
Writes persona1\\nids_persona1.csv and prints every import with its name.
"""
import csv
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
gist = os.path.join(here, 'nid_resolve.py')
imports_csv = os.path.join(here, 'report_imports.csv')
out_csv = os.path.join(here, 'nids_persona1.csv')

for p in (gist, imports_csv):
    if not os.path.exists(p):
        print('MISSING:', p)
        sys.exit(1)

names = {}
pattern = re.compile(r'^\s*0x([0-9a-fA-F]+)\s*:\s*"(.*?)"\s*,?\s*$')
for line in open(gist, encoding='utf-8', errors='replace'):
    m = pattern.match(line)
    if m:
        # The gist's web page shows underscores escaped as \_ ; the raw file does not.
        names.setdefault(int(m.group(1), 16), m.group(2).replace('\\_', '_'))
print('names read from map:', len(names))
if not names:
    print('The map file had no entries. Make sure you saved the Raw version of the gist.')
    sys.exit(1)

rows = list(csv.DictReader(open(imports_csv, newline='')))
missing = []
with open(out_csv, 'w', newline='') as f:
    for r in rows:
        nid = int(r['nid'], 16)
        name = names.get(nid, '')
        f.write('%s,0x%08X,%s\n' % (r['library'], nid, name))
        print('%-22s 0x%08X  %s' % (r['library'], nid, name or '??? (not in map)'))
        if not name:
            missing.append((r['library'], nid))

print()
print('imports:', len(rows), '| named:', len(rows) - len(missing), '| unnamed:', len(missing))
print('wrote', out_csv)
