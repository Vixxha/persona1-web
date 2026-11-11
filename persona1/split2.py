import os, sys
SRC, OUT = sys.argv[1], sys.argv[2]
LIMIT, REGCHUNK = 3000000, 20000
os.makedirs(OUT, exist_ok=True)
text = open(SRC, errors='replace').read()
lines = text.split('\n')
ns = next(i for i, l in enumerate(lines) if l.startswith('namespace psprecomp {'))
starts = [i for i, l in enumerate(lines) if l.startswith('static const std::uint16_t kEntryIds_')]
tail = next(i for i, l in enumerate(lines) if l.startswith('static void import_'))
end = max(i for i, l in enumerate(lines) if l.startswith('} // namespace psprecomp'))
rg = next(i for i, l in enumerate(lines) if l.startswith('void register_generated_functions('))
rg_end = max(i for i in range(rg, end) if lines[i].startswith('}'))
header = lines[:ns]
blocks = [lines[a:b] for a, b in zip(starts, starts[1:] + [tail])]
wrappers = lines[tail:rg]
reg = lines[rg + 1:rg_end]
imp = [l for l in reg if '&import_' in l]
fn = [l for l in reg if '&import_' not in l]
odd = [l for l in fn if 'runtime.register_function(' not in l]
names = [l.split('(')[0].split()[-1] for b in blocks for l in b if l.startswith('void sub_') and not '_entry(' in l]
with open(os.path.join(OUT, 'decls.hpp'), 'w') as f:
    f.write('#pragma once\n#include <cstdint>\n#include "psprecomp/runtime.hpp"\nnamespace psprecomp {\n')
    for n in names:
        f.write('void %s(Runtime &rt, AllegrexContext &ctx);\n' % n)
        f.write('void %s_entry(Runtime &rt, AllegrexContext &ctx, std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem);\n' % n)
    f.write('}\n')
def emit(name, body):
    p = os.path.join(OUT, name)
    with open(p, 'w', newline='\n') as f:
        f.write('\n'.join(header + ['#include "decls.hpp"', 'namespace psprecomp {'] + body + ['} // namespace psprecomp', '']))
    return os.path.getsize(p) // 1024
sizes, cur, size, idx = [], [], 0, 0
for b in blocks:
    cur += b
    size += sum(len(l) + 1 for l in b)
    if size >= LIMIT:
        sizes.append(emit('gen_%03d.cpp' % idx, cur)); idx += 1; cur, size = [], 0
if cur:
    sizes.append(emit('gen_%03d.cpp' % idx, cur))
parts = 0
for i in range(0, len(fn), REGCHUNK):
    body = ['void register_part_%03d(Runtime &runtime) {' % parts] + fn[i:i + REGCHUNK] + ['}']
    emit('reg_%03d.cpp' % parts, body); parts += 1
master = ['void register_part_%03d(Runtime &runtime);' % k for k in range(parts)]
master += wrappers
master += ['void register_generated_functions(Runtime &runtime) {']
master += ['    register_part_%03d(runtime);' % k for k in range(parts)]
master += imp + ['}']
emit('imports.cpp', master)
print('functions:', len(blocks), 'declared:', len(names), '| gen files:', len(sizes), 'KB range', min(sizes), max(sizes))
print('registration lines:', len(fn), 'in', parts, 'files | import registrations:', len(imp), '| odd lines in registration:', len(odd))
