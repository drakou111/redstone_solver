"""Samples sketches for one goal from the trained model, simulates them, and
writes the closest ones (plus the closest known circuits) as search seeds."""

import sys
from pathlib import Path

import numpy as np
import torch

import loop
import rsd
from model import Sketcher, sample, table_features

RUN = loop.OUT_DIR
GOAL = sys.argv[1] if len(sys.argv) > 1 else "full_adder"
SKETCHES = 65536
TEMPERATURES = (0.7, 1.0, 1.3)
KEEP = 200
OUT = RUN / f"seeds_{GOAL}.rsd"


def main() -> None:
    data = rsd.read(RUN / "data.rsd")
    header = data.header
    inputs, outputs = loop.parse_ports(loop.SPEC)
    static = loop.static_channels(header, inputs, outputs).to(loop.DEVICE)
    port_class = header.classes - 1
    fixed = torch.from_numpy(data.cells[0] == port_class).to(loop.DEVICE)
    fixed_cells = torch.from_numpy(data.cells[0].astype(np.int64)).to(loop.DEVICE)
    goal = next(g for g in loop.goal_tables(header) if g[0] == GOAL)
    _, bits, cares = goal

    model = Sketcher(header.classes, static.shape[0], header.table_bytes).to(loop.DEVICE)
    model.load_state_dict(torch.load(RUN / "checkpoint.pt", weights_only=False)["model"])
    model.eval()
    sketches = []
    per_temp = SKETCHES // len(TEMPERATURES)
    for temp in TEMPERATURES:
        for s in range(0, per_temp, loop.SAMPLE_BATCH):
            n = min(loop.SAMPLE_BATCH, per_temp - s)
            tb = torch.from_numpy(np.tile(bits, (n, 1))).to(loop.DEVICE)
            tc = torch.from_numpy(np.tile(cares, (n, 1))).to(loop.DEVICE)
            with torch.autocast("cuda", dtype=torch.bfloat16):
                out = sample(model, static, table_features(tb, tc), fixed, fixed_cells,
                             steps=loop.SAMPLE_STEPS, temperature=temp)
            sketches.append(out.cpu().numpy().astype(np.uint8))
    sketches = np.concatenate(sketches)
    tables = loop.simulate(header, sketches, RUN)
    working = (tables != 255).all(axis=1)
    wrong = ((tables > 0)[:, cares] != bits[cares]).sum(axis=1)
    wrong[~working] = 99
    print(f"{GOAL}: {working.mean():.1%} of {len(sketches)} sketches work; "
          f"bits wrong histogram: {np.bincount(wrong[working], minlength=9)[:9]}")

    known_wrong = ((data.tables > 0)[:, cares] != bits[cares]).sum(axis=1)
    pool_cells = np.concatenate([sketches, data.cells])
    pool_wrong = np.concatenate([wrong, known_wrong])
    best = np.argsort(pool_wrong, kind="stable")[:KEEP]
    print(f"seeds: {KEEP}, bits wrong {pool_wrong[best].min()}..{pool_wrong[best].max()}")
    rsd.write(OUT, rsd.Samples(header, np.zeros((KEEP, header.table_bytes), np.uint8),
                               pool_cells[best]))
    print("->", OUT)


if __name__ == "__main__":
    main()
