#!/usr/bin/env python3
"""Merge xedgen's TSV (stdin) with the statuses already in manifest.tsv. New forms get UNSET,
which the spec-complete test refuses. Usage: xedgen | make_manifest.py manifest.tsv [probe.tsv refused.tsv]"""
import sys, collections, fnmatch
path = sys.argv[1]
prop = dict(l.split(chr(9))[:2] for l in open(sys.argv[2])) if len(sys.argv) > 2 and sys.argv[2] != "-" else {}
known = [l.rstrip(chr(10)).split(chr(9)) for l in open(sys.argv[4]) if l[0] != "#" and l.strip()] if len(sys.argv) > 4 and sys.argv[4] != "-" else []
refused = [l.rstrip(chr(10)).split(chr(9)) for l in open(sys.argv[3]) if l[0] != "#"] if len(sys.argv) > 3 and sys.argv[3] != "-" else []
old = {}
try:
    for l in open(path):
        if l[0] != '#':
            c = l.rstrip('\n').split('\t')
            old[c[0]] = (c[17], c[18])
except FileNotFoundError:
    pass
rows = [l.rstrip('\n').split('\t') for l in sys.stdin if l[0] != '#']
short = {'ymm': 'y', 'xmm': 'x', 'gpr': 'g', 'imm8': 'i'}
names, seen = [], collections.Counter()
for c in rows:
    ops = '_'.join(short.get(o, o) for o in c[13].split(','))
    n = f"{c[1].lower()}.{ops}"
    names.append(n); seen[n] += 1
out = ["#id\ticlass\tisa\tvex\tmap\tpp\topc\text\tmod\tW\tL\timm8\topsz\tops\tmemw\tmemrw\tname\tstatus\treason"]
used = set()
for c, n in zip(rows, names):
    if seen[n] > 1:
        n0 = n
        n = f"{n0}.{'o'+c[12] if c[12] != '0' else 'w'+c[9]}"
        k = 1
        while n in used: k += 1; n = f"{n0}.{k}"
    assert n not in used and len(n) < 32, n
    used.add(n)
    st, why = old.get(c[0], ('UNSET', ''))
    if st == 'UNSET' and n in prop:
        if prop[n] == 'accepted': st = 'tested'
        else:
            for g, r in refused:
                if fnmatch.fnmatch(n, g): st, why = 'refused', r; break
    if st in ('tested', 'failing') and known:
        hit = [k[0] for k in known if k[2] != '-' and fnmatch.fnmatch(n, k[2])]
        st, why = ('failing', ','.join(hit)) if hit else ('tested', '')
    out.append(chr(9).join(c + [n, st, why]))
open(path, 'w', newline='\n').write('\n'.join(out) + '\n')
print(len(rows), 'forms', sum(1 for l in out[1:] if '\tUNSET\t' in l), 'unset')
