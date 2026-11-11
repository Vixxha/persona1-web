"""Writes persona1\\functions_fixed3.csv for psp_recomp's explicit mode.

Format per line: name,address,size   (size in hex bytes)

Each function covers its analyzed instructions, then is extended up to the start of the
next function. Code reached only through jump tables or function pointers was missing
from the analyzer's instruction counts, and a 256-byte limit on that extension left
part of one large function uncovered (the game jumped to 0x08964514 and stopped).
The extension is now unlimited by default; pass a number to cap it, for example
    python persona1\\make_functions_csv.py 65536

Run from the repo root.
"""
import csv
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
LIMIT = 0x0898147C   # functions at or above this address are data (import stub table and up)
cap = int(sys.argv[1], 0) if len(sys.argv) > 1 else None

rows = list(csv.DictReader(open(os.path.join(here, 'report_functions_auto.csv'))))
keep = sorted([x for x in rows if int(x['address'], 16) < LIMIT], key=lambda x: int(x['address'], 16))
addrs = [int(x['address'], 16) for x in keep]

extended = 0
extra_total = 0
big = []
with open(os.path.join(here, 'functions_fixed3.csv'), 'w', newline='') as f:
    for i, x in enumerate(keep):
        size = int(x['instructions']) * 4
        nxt = addrs[i + 1] if i + 1 < len(addrs) else LIMIT
        gap = nxt - addrs[i]
        use = size
        if gap > size:
            extra = gap - size
            if cap is not None:
                extra = min(extra, cap)
            use = size + extra
            extended += 1
            extra_total += extra
            big.append((extra, x['name'], x['address']))
        f.write('%s,%s,0x%X\n' % (x['name'], x['address'], use))

big.sort(reverse=True)
print(len(keep), 'functions;', extended, 'extended;', extra_total, 'extra bytes in total',
      '(cap: %s)' % ('none' if cap is None else cap))
print('largest extensions:')
for extra, name, addr in big[:6]:
    print('  %s at %s: +%d bytes' % (name, addr, extra))
