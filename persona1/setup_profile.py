"""Creates or refreshes profiles/persona1 from files kept in the persona1 working folder.

Put these in C:\\PSPRecomp\\PSPRecomp\\persona1\\ :
    persona1_main.cpp, persona1_kernel.cpp, persona1_kernel.hpp,
    persona1_io.cpp, persona1_io.hpp, persona1_ge.cpp, persona1_ge.hpp, persona1_display.cpp, persona1_display.hpp, persona1_mods.cpp, persona1_mods.hpp, persona1_CMakeLists.txt, setup_profile.py
and make sure persona1\\gen6\\split exists (decls.hpp, gen_*.cpp, reg_*.cpp, imports.cpp).

Run from the repo root:   python persona1\\setup_profile.py

Generated sources are only copied when missing or different in content, so a refresh
after editing host files is quick and does not trigger a full recompile. Generated
files that are no longer part of the split output are removed.
"""
import filecmp
import glob
import os
import shutil
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
profile = os.path.join(root, 'profiles', 'persona1')
split = os.path.join(here, 'gen6', 'split')

host_files = {
    'persona1_main.cpp': os.path.join('host', 'main.cpp'),
    'persona1_kernel.cpp': os.path.join('host', 'persona1_kernel.cpp'),
    'persona1_kernel.hpp': os.path.join('host', 'persona1_kernel.hpp'),
    'persona1_io.cpp': os.path.join('host', 'persona1_io.cpp'),
    'persona1_io.hpp': os.path.join('host', 'persona1_io.hpp'),
    'persona1_ge.cpp': os.path.join('host', 'persona1_ge.cpp'),
    'persona1_ge.hpp': os.path.join('host', 'persona1_ge.hpp'),
    'persona1_audio.cpp': os.path.join('host', 'persona1_audio.cpp'),
    'persona1_audio.hpp': os.path.join('host', 'persona1_audio.hpp'),
    'persona1_movie.cpp': os.path.join('host', 'persona1_movie.cpp'),
    'persona1_movie.hpp': os.path.join('host', 'persona1_movie.hpp'),
    'persona1_display.cpp': os.path.join('host', 'persona1_display.cpp'),
    'persona1_display.hpp': os.path.join('host', 'persona1_display.hpp'),
    'persona1_mods.cpp': os.path.join('host', 'persona1_mods.cpp'),
    'persona1_mods.hpp': os.path.join('host', 'persona1_mods.hpp'),
    'persona1_CMakeLists.txt': 'CMakeLists.txt',
}

missing = [n for n in host_files if not os.path.exists(os.path.join(here, n))]
if missing or not os.path.isdir(split):
    for n in missing:
        print('MISSING:', os.path.join(here, n))
    if not os.path.isdir(split):
        print('MISSING:', split)
    sys.exit(1)

cpp = glob.glob(os.path.join(split, '*.cpp'))
if not os.path.exists(os.path.join(split, 'decls.hpp')) or not cpp:
    print('gen6\\split does not look complete (need decls.hpp and *.cpp)')
    sys.exit(1)

os.makedirs(os.path.join(profile, 'host'), exist_ok=True)
gen_dir = os.path.join(profile, 'generated')
os.makedirs(gen_dir, exist_ok=True)

for src_name, dest_rel in host_files.items():
    shutil.copy(os.path.join(here, src_name), os.path.join(profile, dest_rel))
    print('copied', src_name, '->', dest_rel)

wanted = {os.path.basename(f) for f in cpp} | {'decls.hpp'}
for old in glob.glob(os.path.join(gen_dir, '*')):
    if os.path.basename(old) not in wanted:
        os.remove(old)
        print('removed stale generated file', os.path.basename(old))

copied = 0
for f in cpp + [os.path.join(split, 'decls.hpp')]:
    dest = os.path.join(gen_dir, os.path.basename(f))
    if not os.path.exists(dest) or not filecmp.cmp(f, dest, shallow=False):
        shutil.copy(f, dest)
        copied += 1
print('generated files:', len(cpp) + 1, 'total,', copied, 'copied this time')
print('profile folder:', profile)
