"""Which per-output functions of the three inputs occur in a dataset, grouped
by name for the ones that matter (and, or, xor, majority...)."""

import sys
from collections import Counter
from itertools import product
from pathlib import Path

import numpy as np

import rsd

data = rsd.read(Path(sys.argv[1]))
h = data.header
on = data.tables > 0


def table(fn):
    return tuple(bool(fn(*[(v >> i) & 1 for i in range(h.inputs)])) for v in range(h.vectors))


named = {}
for i, j in product(range(3), repeat=2):
    if i < j:
        named[table(lambda *x, i=i, j=j: x[i] and x[j])] = f"AND({'ABC'[i]},{'ABC'[j]})"
        named[table(lambda *x, i=i, j=j: x[i] ^ x[j])] = f"XOR({'ABC'[i]},{'ABC'[j]})"
        named[table(lambda *x, i=i, j=j: not (x[i] or x[j]))] = f"NOR({'ABC'[i]},{'ABC'[j]})"
        named[table(lambda *x, i=i, j=j: not (x[i] and x[j]))] = f"NAND({'ABC'[i]},{'ABC'[j]})"
    if i != j:
        named[table(lambda *x, i=i, j=j: x[i] and not x[j])] = f"{'ABC'[i]}&!{'ABC'[j]}"
named[table(lambda a, b, c: a and b and c)] = "AND3"
named[table(lambda a, b, c: a + b + c >= 2)] = "MAJ3"
named[table(lambda a, b, c: a ^ b ^ c)] = "XOR3"
for i in range(3):
    named[table(lambda *x, i=i: not x[i])] = f"NOT {'ABC'[i]}"

for o in range(h.outputs):
    counts = Counter(tuple(r) for r in on[:, o::h.outputs])
    print(f"OUT{o + 1}: " + ", ".join(f"{name} {counts.get(t, 0)}" for t, name in named.items()))
