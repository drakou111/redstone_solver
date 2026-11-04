"""Model-guided local search: regenerate random regions of the circuits
closest to a goal with the trained model (conditioned on the goal), keep
the variants that get closer, repeat.

Usage: python inpaint.py <run dir> <spec> <goal name> <minutes>
"""

import json
import sys
import time
from pathlib import Path

import numpy as np
import torch

import loop
import rsd
from model import Sketcher, mask_ratio, table_features

POPULATION = 256
VARIANTS = 8
STEPS = 8
DEVICE = "cuda"


@torch.no_grad()
def fill(model, static, table, fixed, cells, steps):
    """Like model.sample, with a fixed mask and cells per sample."""
    cells = cells.clone()
    hidden = ~fixed
    free = hidden.flatten(1).sum(1)
    shape = cells.shape
    for step in range(steps):
        with torch.autocast("cuda", dtype=torch.bfloat16):
            logits = model(cells, hidden, static, table).float()
        probs = logits.softmax(dim=1).permute(0, 2, 3, 4, 1)
        picked = torch.distributions.Categorical(probs=probs).sample()
        conf = probs.gather(-1, picked[..., None])[..., 0]
        conf = torch.where(hidden, conf, torch.full_like(conf, -1.0))
        keep_hidden = (free.float() * float(mask_ratio(torch.tensor((step + 1) / steps)))).long()
        reveal = hidden.flatten(1).sum(1) - keep_hidden
        order = conf.flatten(1).argsort(dim=1, descending=True)
        rank = torch.empty_like(order)
        rank.scatter_(1, order, torch.arange(order.shape[1], device=order.device).expand_as(order))
        newly = (rank < reveal[:, None]).view(shape) & hidden
        cells = torch.where(newly, picked, cells)
        hidden = hidden & ~newly
    return cells


def main() -> None:
    run, spec, goal_name, minutes = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], float(sys.argv[4])
    loop.SPEC = spec
    data = rsd.read(run / "data.rsd")
    h = data.header
    inputs, outputs = loop.parse_ports(spec)
    static = loop.static_channels(h, inputs, outputs).to(DEVICE)
    port = h.classes - 1
    ports = torch.from_numpy(data.cells[0] == port).to(DEVICE)
    _, bits, cares = next(g for g in loop.goal_tables(h) if g[0] == goal_name)
    model = Sketcher(h.classes, static.shape[0], h.table_bytes).to(DEVICE)
    model.load_state_dict(torch.load(run / "checkpoint.pt", weights_only=False)["model"])
    model.eval()
    rng = np.random.default_rng(1)

    def wrong_of(tables):
        w = ((tables > 0)[:, cares] != bits[cares]).sum(axis=1)
        w[(tables == 255).any(axis=1)] = 99
        return w

    known = wrong_of(data.tables)
    order = np.argsort(known, kind="stable")[:POPULATION]
    pop = data.cells[order].copy()
    pop_wrong = known[order].copy()
    print(f"{goal_name}: start best {pop_wrong.min()} wrong, population {len(pop)}", flush=True)
    sy, sz, sx = pop.shape[1:]
    goal_table = table_features(torch.from_numpy(np.tile(bits, (len(pop) * VARIANTS, 1))).to(DEVICE),
                                torch.from_numpy(np.tile(cares, (len(pop) * VARIANTS, 1))).to(DEVICE))
    start = time.time()
    it = 0
    best_seen = int(pop_wrong.min())
    while time.time() - start < minutes * 60:
        it += 1
        parents = np.repeat(pop, VARIANTS, axis=0)
        # hide a random box (x and z extent 2..5, all layers) in each variant
        fixed = np.ones_like(parents, dtype=bool)
        for i in range(len(parents)):
            dx, dz = rng.integers(2, 6), rng.integers(2, 6)
            x0, z0 = rng.integers(0, max(1, sx - dx + 1)), rng.integers(0, max(1, sz - dz + 1))
            fixed[i, :, z0:z0 + dz, x0:x0 + dx] = False
        fixed_t = torch.from_numpy(fixed).to(DEVICE) | ports[None]
        cells_t = torch.from_numpy(parents.astype(np.int64)).to(DEVICE)
        children = fill(model, static, goal_table, fixed_t, cells_t, STEPS).cpu().numpy().astype(np.uint8)
        tables = loop.simulate(h, children, run)
        w = wrong_of(tables).reshape(len(pop), VARIANTS)
        best_child = w.argmin(axis=1)
        child_wrong = w[np.arange(len(pop)), best_child]
        take = child_wrong <= pop_wrong  # ties drift
        idx = np.arange(len(pop)) * VARIANTS + best_child
        pop[take] = children[idx[take]]
        pop_wrong[take] = child_wrong[take]
        if pop_wrong.min() < best_seen or it % 20 == 0:
            best_seen = min(best_seen, int(pop_wrong.min()))
            print(f"  it {it} ({time.time() - start:.0f}s): best {pop_wrong.min()} wrong, "
                  f"population mean {pop_wrong.mean():.2f}", flush=True)
        if pop_wrong.min() == 0:
            break
    best = int(pop_wrong.argmin())
    out = run / f"inpaint_{goal_name}.rsd"
    rsd.write(out, rsd.Samples(h, np.zeros((len(pop), h.table_bytes), np.uint8), pop[np.argsort(pop_wrong)]))
    (run / f"inpaint_{goal_name}.json").write_text(json.dumps(
        {"goal": goal_name, "best_wrong": int(pop_wrong[best]), "iterations": it,
         "seconds": round(time.time() - start), "population": POPULATION, "variants": VARIANTS}))
    print(f"done: best {pop_wrong[best]} wrong after {it} iterations -> {out}")


if __name__ == "__main__":
    main()
