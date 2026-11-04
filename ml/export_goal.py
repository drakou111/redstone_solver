"""Exports the circuits in a run's data closest to a goal as spec files with
hint lines (the goal's rows, the run's box and ports), for --check and the
app. Usage: python export_goal.py <run dir> <box spec> <goal> <out dir> [n]
"""

import json
import sys
from pathlib import Path

import numpy as np

import loop
import rsd

run, box, goal_name, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
count = int(sys.argv[5]) if len(sys.argv) > 5 else 3
loop.SPEC = box
out.mkdir(parents=True, exist_ok=True)
data = rsd.read(run / "data.rsd")
h = data.header
names = None
for candidate in [run / "data.rsd.json", *run.parent.glob("*/data.rsd.json")]:
    if candidate.exists():
        names = json.loads(candidate.read_text())["classes"]
        break
if names is None:
    names = json.loads((loop.ROOT / "scratch" / "data" / "fa_1m.rsd.json").read_text())["classes"]
_, bits, cares = next(g for g in loop.goal_tables(h) if g[0] == goal_name)
wrong = ((data.tables > 0)[:, cares] != bits[cares]).sum(axis=1)
best = np.argsort(wrong, kind="stable")[:count]
spec_path = out / f"{goal_name.replace('=', '_')}_rows.txt"
loop.goal_spec = getattr(loop, "goal_spec", None)
lines = [l for l in box.read_text().splitlines()
         if l.split() and l.split()[0] in ("size", "input", "output")]
lines += ["timing none", "horizon 30"]
for v in range(h.vectors):
    ins = ",".join(f"IN{i + 1}(ss{15 if (v >> i) & 1 else 0})" for i in range(h.inputs))
    outs = [f"OUT{o + 1}({'ss1..15' if bits[v * h.outputs + o] else 'ss0'})"
            for o in range(h.outputs) if cares[v * h.outputs + o]]
    if outs:
        lines.append(f"row {ins} -> {','.join(outs)}")
for rank, i in enumerate(best):
    cells = data.cells[i]
    hints = []
    sy, sz, sx = cells.shape
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                name = names[cells[y, z, x]]
                if name not in ("air", "port"):
                    hints.append(f"hint {x} {y} {z} {name}")
    path = out / f"{goal_name.replace('=', '_')}_{rank}.txt"
    path.write_text("\n".join(lines + hints) + "\n")
    print(f"{path.name}: {wrong[i]} of {int(cares.sum())} bits wrong, {len(hints)} blocks")
