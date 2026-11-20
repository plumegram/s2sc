#!/usr/bin/env python3
"""Locate the skinchanger layout in an Il2CppDumper dump of any Standoff 2 build.

Class names are obfuscated and change every build, and so do the numeric ids.
Enum *member names* are stable though, so this script identifies the types by
the names they contain, not by their values or their own names:

  1. skin enum      - the enum with the largest overlap with the names in SkinsID.txt
  2. weapon enum    - the enum that contains USP / Deagle / AKR / AKR12 ...
  3. controllers    - classes holding a field of the skin enum type

Usage:
    python find_layout.py dump.cs "SkinsID.txt"

Everything printed is read from the files you pass in. Nothing is guessed.
"""

import re
import sys
from collections import Counter

CLASS_RE = re.compile(r'^(?:public |private |internal |protected |static |sealed |abstract )*'
                      r'(class|struct|enum)\s+([A-Za-z_][\w.]*)\s*.*TypeDefIndex:\s*(\d+)')
FIELD_RE = re.compile(r'^\s*(?:public|private|internal|protected)\s+(.*?)\s+([^\s;]+);\s*//\s*0x([0-9A-Fa-f]+)')
CONST_RE = re.compile(r'^\s*public const\s+(\S+)\s+([^\s=]+)\s*=\s*(-?\d+);')

WEAPON_NAMES = {'USP', 'Deagle', 'AKR', 'AKR12', 'AWM', 'M4A1', 'FiveSeven', 'Mallard'}


def parse(path):
    types, order = {}, []
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = CLASS_RE.match(line)
            if m:
                rest = line[m.end(2):]
                bases = []
                if ':' in rest.split('//')[0]:
                    bases = [b.strip().split('<')[0] for b in
                             rest.split('//')[0].split(':', 1)[1].split(',') if b.strip()]
                cur = {'kind': m.group(1), 'name': m.group(2), 'idx': int(m.group(3)),
                       'fields': [], 'consts': [], 'bases': bases}
                types[cur['name']] = cur
                order.append(cur['name'])
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                cur = None
                continue
            if cur['kind'] == 'enum':
                c = CONST_RE.match(line)
                if c:
                    cur['consts'].append((c.group(2), int(c.group(3))))
                continue
            fm = FIELD_RE.match(line)
            if fm:
                cur['fields'].append((fm.group(1).strip(), fm.group(2), int(fm.group(3), 16)))
    return types, order


def read_enum_names(path):
    names = set()
    pat = re.compile(r'const\s+\S+\s+([^\s=]+)\s*=\s*-?\d+\s*;')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = pat.search(line)
            if m:
                names.add(m.group(1))
    return names


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    dump_path, enum_path = sys.argv[1], sys.argv[2]

    known = read_enum_names(enum_path)
    if not known:
        print('enum file parsed empty')
        return 2
    print(f'known skin names: {len(known)}')

    types, order = parse(dump_path)
    print(f'types in dump    : {len(types)}')

    best, best_hits = None, 0
    for name in order:
        t = types[name]
        if t['kind'] != 'enum':
            continue
        hits = sum(1 for cn, _ in t['consts'] if cn in known)
        if hits > best_hits:
            best, best_hits = name, hits
    if not best or best_hits < 3:
        print('skin enum not found: no enum shares names with the enum file')
        return 3

    skin_enum = best
    print(f'skin enum        : {skin_enum} (TypeDefIndex {types[skin_enum]["idx"]}, '
          f'{len(types[skin_enum]["consts"])} values, {best_hits} name matches)')

    weapon_enum, weapon_hits = None, 0
    for name in order:
        t = types[name]
        if t['kind'] != 'enum':
            continue
        hits = sum(1 for cn, _ in t['consts'] if cn in WEAPON_NAMES)
        if hits > weapon_hits:
            weapon_enum, weapon_hits = name, hits
    print(f'weapon enum      : {weapon_enum} ({weapon_hits} name matches)')
    print()

    holders = []
    for name in order:
        t = types[name]
        if t['kind'] != 'class':
            continue
        skin_fields = [f for f in t['fields'] if f[0] == skin_enum]
        if not skin_fields:
            continue
        has_weapon = any(f[0] == weapon_enum for f in t['fields'])
        is_mono = any(b == 'MonoBehaviour' or b == 'ScriptableObject' for b in t['bases'])
        holders.append((len(skin_fields), has_weapon, is_mono, name, skin_fields))

    # runtime components first, then plain data holders
    holders.sort(key=lambda h: (not h[2], not h[1], -h[0], h[3]))
    if not holders:
        print('no class holds a field of the skin enum type')
        return 4

    print('classes holding a skin id (a weapon controller should hold both ids):')
    for n, has_weapon, is_mono, name, _ in holders[:15]:
        tags = []
        if is_mono:
            tags.append('component')
        tags.append('weapon+skin' if has_weapon else 'skin only')
        print(f'  {name:<44} fields={n:<2} {", ".join(tags)}')
    print()

    for _, _, _, name, _ in holders[:3]:
        t = types[name]
        print(f'layout of {name} (TypeDefIndex {t["idx"]}):')
        for ftype, fname, off in t['fields']:
            mark = ''
            if ftype == skin_enum:
                mark = '   <- skin id'
            elif ftype == weapon_enum:
                mark = '   <- weapon id'
            print(f'  0x{off:03X}  {ftype:<30} {fname}{mark}')
        print()

    # who points at the strongest candidate
    top = holders[0][3]
    print(f'classes referencing {top}:')
    refs = 0
    for name in order:
        o = types[name]
        if o['kind'] != 'class' or name == top:
            continue
        for ftype, fname, off in o['fields']:
            if ftype == top:
                print(f'  {name:<44} 0x{off:03X}  {fname}')
                refs += 1
    if not refs:
        print('  none (reachable only through a list or a base class)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
