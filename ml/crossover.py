"""Crossover search: overlay circuits that are closest to a goal with circuits
computing what they miss, and keep children that get closer.

Child of (a, b): every cell from a, except cells where a is empty and b is
not (b's blocks fill a's gaps), optionally limited to a random box region of
b. Usage: python crossover.py <run dir> <spec> <goal> <minutes>
"""

import json
import sys
import time
from pathlib import Path

import numpy as np

import loop
import rsd

BATCH = 50000
ELITE = 512
DONORS = 20000


def main() -> None:
    run, spec, goal_name, minutes = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], float(sys.argv[4])
    loop.SPEC = spec
    data = rsd.read(run / "data.rsd")
    h = data.header
    _, bits, cares = next(g for g in loop.goal_tables(h) if g[0] == goal_name)
    rng = np.random.default_rng(2)
    on = data.tables > 0

    def wrong_of(tables):
        w = ((tables > 0)[:, cares] != bits[cares]).sum(axis=1)
        w[(tables == 255).any(axis=1)] = 99
        return w

    known = wrong_of(data.tables)
    elite_idx = np.argsort(known, kind="stable")[:ELITE]
    elite, elite_wrong = data.cells[elite_idx].copy(), known[elite_idx].copy()
    # donors: circuits that are right on bits the elite gets wrong
    miss = np.zeros(on.shape[1], bool)
    for i in elite_idx:
        miss |= (on[i] != bits) & cares
    helpful = ((on[:, miss] == bits[miss]).sum(axis=1))
    donors = data.cells[np.argsort(-helpful, kind="stable")[:DONORS]]
    print(f"{goal_name}: elite best {elite_wrong.min()} wrong; donors right on "
          f"missed bits: {helpful.max()}/{int(miss.sum())}", flush=True)

    sy, sz, sx = elite.shape[1:]
    start, it = time.time(), 0
    while time.time() - start < minutes * 60:
        it += 1
        a = elite[rng.integers(0, len(elite), BATCH)]
        b = donors[rng.integers(0, len(donors), BATCH)]
        # b's blocks only inside a random region (sometimes everywhere)
        region = np.ones_like(a, dtype=bool)
        boxed = rng.random(BATCH) < 0.7
        for i in np.flatnonzero(boxed):
            dx, dz, dy = rng.integers(2, sx + 1), rng.integers(2, sz + 1), rng.integers(1, sy + 1)
            x0, z0, y0 = rng.integers(0, sx - dx + 1), rng.integers(0, sz - dz + 1), rng.integers(0, sy - dy + 1)
            region[i] = False
            region[i, y0:y0 + dy, z0:z0 + dz, x0:x0 + dx] = True
        child = np.where((a == 0) & (b != 0) & region, b, a)
        tables = loop.simulate(h, child, run)
        w = wrong_of(tables)
        pool = np.concatenate([elite, child])
        pool_w = np.concatenate([elite_wrong, w])
        keep = np.argsort(pool_w, kind="stable")[:ELITE]
        elite, elite_wrong = pool[keep], pool_w[keep]
        print(f"  it {it} ({time.time() - start:.0f}s): best {elite_wrong.min()} wrong, "
              f"children at best {int((w == elite_wrong.min()).sum())}", flush=True)
        if elite_wrong.min() == 0:
            break
    out = run / f"crossover_{goal_name}.rsd"
    rsd.write(out, rsd.Samples(h, np.zeros((len(elite), h.table_bytes), np.uint8), elite))
    (run / f"crossover_{goal_name}.json").write_text(json.dumps(
        {"goal": goal_name, "best_wrong": int(elite_wrong.min()), "iterations": it,
         "seconds": round(time.time() - start)}))
    print(f"done: best {elite_wrong.min()} wrong -> {out}")


if __name__ == "__main__":
    main()
