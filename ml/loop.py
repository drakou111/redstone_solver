"""Expert iteration for the circuit sketcher.

Each round: train on all circuits found so far (sampled evenly over the
functions they compute), ask the model for circuits computing goal tables
and tables next to known ones, simulate every sketch in C++, and keep each
working one under the function it actually computes (hindsight labels).
Resumes from the last finished round in OUT_DIR.
"""

import json
import random
import subprocess
import time
from collections import defaultdict
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

import rsd
from model import Sketcher, mask_ratio, sample, table_features

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "examples" / "full_adder.txt"
SEED_DATA = [ROOT / "output" / "ml" / "fa_run1" / "data.rsd",
             ROOT / "scratch" / "data" / "fa_3d_1m.rsd"]
DATAGEN = ROOT / "build" / "msvc" / "Release" / "redstone_datagen.exe"
OUT_DIR = ROOT / "output" / "ml" / "fa_run2"

ROUNDS = 40
TOWARD_GOAL_SHARE = 0.5  # of the nearby requests: one step closer to a goal
FIRST_STEPS = 15000
STEPS_PER_ROUND = 3000
BATCH = 256
LEARNING_RATE = 3e-4
SKETCHES_PER_ROUND = 32768
SAMPLE_BATCH = 2048
SAMPLE_STEPS = 10
GOAL_SHARE = 0.5          # rest: tables one or two bits from known ones
SEED = 1
DEVICE = "cuda"


# ------------------------------------------------------------------ spec

def parse_ports(spec: Path):
    inputs, outputs = {}, {}
    for line in spec.read_text().splitlines():
        w = line.split()
        if len(w) >= 6 and w[0] in ("input", "output"):
            index = int(w[1][2:] if w[0] == "input" else w[1][3:]) - 1
            pos = (int(w[3]), int(w[4]), int(w[5]))
            (inputs if w[0] == "input" else outputs)[index] = pos
    return [inputs[i] for i in sorted(inputs)], [outputs[i] for i in sorted(outputs)]


def static_channels(header: rsd.Header, inputs, outputs) -> torch.Tensor:
    sx, sy, sz = header.size
    chans = []
    for pos in inputs + outputs:
        c = torch.zeros(sy, sz, sx)
        c[pos[1], pos[2], pos[0]] = 1
        chans.append(c)
    gy, gz, gx = torch.meshgrid(torch.linspace(0, 1, sy), torch.linspace(0, 1, sz),
                                torch.linspace(0, 1, sx), indexing="ij")
    chans += [gx, gy, gz]
    return torch.stack(chans)


def goal_tables(header: rsd.Header):
    """(name, bits, cares) goal tables over on/off vectors."""
    n_in, n_out, n_vec = header.inputs, header.outputs, header.vectors

    def bits_of(fn):
        return [bool(fn(*[(v >> i) & 1 for i in range(n_in)])) for v in range(n_vec)]

    funcs = {
        "xor3": lambda a, b, c: a ^ b ^ c,
        "maj3": lambda a, b, c: a + b + c >= 2,
        "xor_ab": lambda a, b, c: a ^ b,
        "xor_bc": lambda a, b, c: b ^ c,
        "xor_ac": lambda a, b, c: a ^ c,
        "and_ab_not_c": lambda a, b, c: a and b and not c,
    }
    goals = []
    for name, fn in funcs.items():
        for o in range(n_out):
            bits = np.zeros(n_vec * n_out, bool)
            cares = np.zeros(n_vec * n_out, bool)
            for v, on in enumerate(bits_of(fn)):
                bits[v * n_out + o] = on
                cares[v * n_out + o] = True
            goals.append((f"OUT{o + 1}={name}", bits, cares))
    full = np.zeros(n_vec * n_out, bool)
    for v, (s, c) in enumerate(zip(bits_of(funcs["xor3"]), bits_of(funcs["maj3"]))):
        full[v * n_out] = s
        full[v * n_out + 1] = c
    goals.append(("full_adder", full, np.ones_like(full)))
    half = np.zeros(n_vec * n_out, bool)
    for v in range(n_vec):
        a, b = v & 1, (v >> 1) & 1
        half[v * n_out] = a ^ b
        half[v * n_out + 1] = a and b
    goals.append(("half_adder_ab", half, np.ones_like(half)))
    return goals


# ------------------------------------------------------------------ data

class Store:
    """All working circuits, indexed by the function they compute."""

    def __init__(self, header: rsd.Header):
        self.header = header
        self.tables: list[np.ndarray] = []
        self.cells: list[np.ndarray] = []
        self.by_function: dict[bytes, list[int]] = defaultdict(list)
        self.seen: set[bytes] = set()

    def add(self, tables: np.ndarray, cells: np.ndarray) -> int:
        added = 0
        for t, c in zip(tables, cells):
            key = c.tobytes()
            if key in self.seen:
                continue
            self.seen.add(key)
            on = t > 0
            self.by_function[np.packbits(on).tobytes()].append(len(self.cells))
            self.tables.append(on)
            self.cells.append(c)
            added += 1
        return added

    def batch(self, n: int, rng: random.Random):
        # evenly over functions, so rare ones are not drowned out
        functions = list(self.by_function.values())
        idx = [rng.choice(rng.choice(functions)) for _ in range(n)]
        return (np.stack([self.tables[i] for i in idx]),
                np.stack([self.cells[i] for i in idx]))

    def save(self, path: Path):
        rsd.write(path, rsd.Samples(self.header,
                                    np.stack(self.tables).astype(np.uint8) * 15,
                                    np.stack(self.cells)))


def simulate(header: rsd.Header, cells: np.ndarray, work: Path) -> np.ndarray:
    """Truth tables of layouts via redstone_datagen --tables (255 = broken)."""
    n = cells.shape[0]
    src, dst = work / "sketches.rsd", work / "sketches_tables.rsd"
    rsd.write(src, rsd.Samples(header, np.zeros((n, header.table_bytes), np.uint8), cells))
    subprocess.run([str(DATAGEN), "--spec", str(SPEC), "--tables", str(src),
                    "--out", str(dst)], check=True, capture_output=True)
    return rsd.read(dst).tables


# ------------------------------------------------------------------ main

def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    torch.manual_seed(SEED)
    params = {k: v for k, v in globals().items() if k.isupper() and
              isinstance(v, (int, float, str))}
    params.update(SPEC=str(SPEC), SEED_DATA=[str(p) for p in SEED_DATA])
    (OUT_DIR / "params.json").write_text(json.dumps(params, indent=2))
    log = (OUT_DIR / "log.txt").open("a")

    def say(text: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {text}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    seeds = [rsd.read(p) for p in SEED_DATA]
    seed = seeds[0]
    header = seed.header
    inputs, outputs = parse_ports(SPEC)
    static = static_channels(header, inputs, outputs).to(DEVICE)
    port_class = header.classes - 1
    fixed = torch.from_numpy(seed.cells[0] == port_class).to(DEVICE)
    fixed_cells = torch.from_numpy(seed.cells[0].astype(np.int64)).to(DEVICE)
    bits = header.table_bytes
    goals = goal_tables(header)

    model = Sketcher(header.classes, static.shape[0], bits).to(DEVICE)
    opt = torch.optim.AdamW(model.parameters(), lr=LEARNING_RATE)
    store = Store(header)
    start_round = 0
    ckpt = OUT_DIR / "checkpoint.pt"
    if ckpt.exists():
        state = torch.load(ckpt, weights_only=False)
        model.load_state_dict(state["model"])
        opt.load_state_dict(state["opt"])
        start_round = state["round"] + 1
        data = rsd.read(OUT_DIR / "data.rsd")
        store.add(data.tables, data.cells)
        say(f"resumed after round {state['round']}: {len(store.cells)} circuits")
    else:
        for extra in seeds:
            store.add(extra.tables, extra.cells)
        say(f"seed data: {len(store.cells)} circuits, {len(store.by_function)} functions")

    def train(steps: int) -> float:
        model.train()
        total = 0.0
        for step in range(steps):
            t, c = store.batch(BATCH, rng)
            cells = torch.from_numpy(c.astype(np.int64)).to(DEVICE)
            on = torch.from_numpy(t).to(DEVICE)
            table = table_features(on, torch.ones_like(on))
            ratio = mask_ratio(torch.rand(BATCH, device=DEVICE))
            hidden = (torch.rand(cells.shape, device=DEVICE) < ratio[:, None, None, None])
            hidden &= ~fixed
            with torch.autocast("cuda", dtype=torch.bfloat16):
                logits = model(cells, hidden, static, table)
            loss = F.cross_entropy(logits.float().permute(0, 2, 3, 4, 1)[hidden],
                                   cells[hidden])
            opt.zero_grad(set_to_none=True)
            loss.backward()
            opt.step()
            total += loss.item()
        return total / max(steps, 1)

    for rnd in range(start_round, ROUNDS):
        t0 = time.time()
        loss = train(FIRST_STEPS if rnd == 0 else STEPS_PER_ROUND)
        say(f"round {rnd}: trained, loss {loss:.3f} ({time.time() - t0:.0f}s)")

        # requests: goals, and known tables with one or two bits flipped
        known = [np.unpackbits(np.frombuffer(k, np.uint8))[:bits].astype(bool)
                 for k in store.by_function]
        known_arr = np.stack(known)
        req_bits, req_cares, req_goal = [], [], []
        n_goal = int(SKETCHES_PER_ROUND * GOAL_SHARE)
        for i in range(SKETCHES_PER_ROUND):
            if i < n_goal:
                g = i % len(goals)
                req_bits.append(goals[g][1]); req_cares.append(goals[g][2]); req_goal.append(g)
            elif rng.random() < TOWARD_GOAL_SHARE:
                # a known function closest to a goal, one wrong bit fixed
                _, gbits, gcares = goals[rng.randrange(len(goals))]
                wrong = (known_arr[:, gcares] != gbits[gcares]).sum(axis=1)
                nearest = np.flatnonzero(wrong == wrong.min())
                b = known_arr[rng.choice(nearest)].copy()
                off = np.flatnonzero(gcares & (b != gbits))
                if off.size:
                    k = int(rng.choice(list(off)))
                    b[k] = gbits[k]
                req_bits.append(b); req_cares.append(np.ones(bits, bool)); req_goal.append(-1)
            else:
                b = rng.choice(known).copy()
                for _ in range(rng.choice((1, 2))):
                    b[rng.randrange(bits)] ^= True
                req_bits.append(b); req_cares.append(np.ones(bits, bool)); req_goal.append(-1)
        model.eval()
        sketches = []
        for s in range(0, SKETCHES_PER_ROUND, SAMPLE_BATCH):
            tb = torch.from_numpy(np.stack(req_bits[s:s + SAMPLE_BATCH])).to(DEVICE)
            tc = torch.from_numpy(np.stack(req_cares[s:s + SAMPLE_BATCH])).to(DEVICE)
            with torch.autocast("cuda", dtype=torch.bfloat16):
                out = sample(model, static, table_features(tb, tc), fixed,
                             fixed_cells, steps=SAMPLE_STEPS)
            sketches.append(out.cpu().numpy().astype(np.uint8))
        sketches = np.concatenate(sketches)
        tables = simulate(header, sketches, OUT_DIR)
        working = (tables != 255).all(axis=1)
        on = tables > 0
        varies = np.zeros(len(on), bool)
        for o in range(header.outputs):
            column = on[:, o::header.outputs]
            varies |= column.any(axis=1) & ~column.all(axis=1)
        keep = working & varies
        functions_before = len(store.by_function)
        added = store.add(tables[keep], sketches[keep])

        hits = defaultdict(int)
        for i in range(n_goal):
            if not working[i]:
                continue
            g = req_goal[i]
            cares = goals[g][2]
            if (on[i][cares] == goals[g][1][cares]).all():
                hits[goals[g][0]] += 1
        near = sum(1 for i in range(n_goal, SKETCHES_PER_ROUND)
                   if working[i] and (on[i] == req_bits[i]).all())
        say(f"round {rnd}: {working.mean():.1%} sketches work, {added} new circuits, "
            f"{len(store.by_function) - functions_before} new functions "
            f"(total {len(store.by_function)}); exact for nearby tables "
            f"{near}/{SKETCHES_PER_ROUND - n_goal}")
        say("   goals hit: " + ", ".join(f"{g[0]} {hits.get(g[0], 0)}" for g in goals))
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "round": rnd}, ckpt)
        store.save(OUT_DIR / "data.rsd")


if __name__ == "__main__":
    main()
