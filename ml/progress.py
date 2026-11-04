"""How close the circuits found so far come to each goal: the fewest truth
table bits a known function gets wrong on the goal's bits."""

import sys
from pathlib import Path

import numpy as np

import rsd
from loop import goal_tables

DATA = Path(sys.argv[1])

data = rsd.read(DATA)
on = data.tables > 0
functions = np.unique(on, axis=0)
print(f"{len(data)} circuits, {len(functions)} functions")
for name, bits, cares in goal_tables(data.header):
    wrong = (functions[:, cares] != bits[cares]).sum(axis=1)
    best = int(wrong.min())
    print(f"  {name:22} closest: {best} of {int(cares.sum())} bits wrong "
          f"({int((wrong == best).sum())} functions)")
