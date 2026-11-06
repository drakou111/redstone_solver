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
SPEC = ROOT / "ml" / "fa_big5.txt"
SEED_DATA = [ROOT / "output" / "ml" / "box9x5x9_run6" / "data.rsd"]
# Block class names; only the datagen output has them (training runs save cells only).
CLASS_NAMES = ROOT / "output" / "ml" / "box9x5x9" / "data.rsd.json"
# Weights to start from (fully convolutional: they fit any box size).
INIT_FROM = ROOT / "output" / "ml" / "box9x5x9_run6" / "checkpoint.pt"
DATAGEN = ROOT / "build" / "msvc" / "Release" / "redstone_datagen.exe"
OUT_DIR = ROOT / "output" / "ml" / "box9x5x9_random1"

ROUNDS = 30
# Search assist: the model's closest sketches seed a hill-climbing search
# for these goals, run while the next round trains.
SEARCH_GOALS = ("full_adder", "OUT1=xor3", "chain_xor_ab_xor3")
SEARCH_SECONDS = 90
SEARCH_THREADS = 6
SEARCH_SEEDS = 64
CARE_DROP = 0.5  # share of training tables with some bits made don't-care
TOWARD_GOAL_SHARE = 0.5  # of the nearby requests: one step closer to a goal
FIRST_STEPS = 3000
STEPS_PER_ROUND = 3000
BATCH = 256
LEARNING_RATE = 3e-4
SKETCHES_PER_ROUND = 32768
SAMPLE_BATCH = 2048
SAMPLE_STEPS = 20
GOAL_SHARE = 0.4  # rest: random tables, then tables one or two bits from known ones
# Random truth tables teach the model functions no goal leads to. Half care
# about every output, half about one output only.
RANDOM_SHARE = 0.25
# A fixed set of random tables, asked every round, measures how well the
# model handles tables it was never pointed at.
RANDOM_EVAL = 512
RANDOM_EVAL_SEED = 12345
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
    gy, gz, gx = torch.meshgrid(
        torch.linspace(0, 1, sy),
        torch.linspace(0, 1, sz),
        torch.linspace(0, 1, sx),
        indexing="ij",
    )
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
        # halves of xor3 = (xor_ab and not c) or (not xor_ab and c)
        "xab_and_not_c": lambda a, b, c: (a ^ b) and not c,
        "xnor_ab_and_c": lambda a, b, c: not (a ^ b) and c,
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
    # Chains: one output shows the intermediate 2-input XOR, the other xor3.
    for pair in ("xor_ab", "xor_bc", "xor_ac"):
        for first, second, tag in (
            (pair, "xor3", f"chain_{pair}_xor3"),
            ("xor3", pair, f"chain_xor3_{pair}"),
        ):
            chain = np.zeros(n_vec * n_out, bool)
            for v, (x, y) in enumerate(
                zip(bits_of(funcs[first]), bits_of(funcs[second]))
            ):
                chain[v * n_out] = x
                chain[v * n_out + 1] = y
            goals.append((tag, chain, np.ones_like(chain)))
    # Both halves of xor3 side by side: joining the two outputs (dust OR)
    # would give xor3.
    for first, second, tag in (
        ("xab_and_not_c", "xnor_ab_and_c", "halves_xor3"),
        ("xnor_ab_and_c", "xab_and_not_c", "halves_xor3_swapped"),
    ):
        pair = np.zeros(n_vec * n_out, bool)
        for v, (x, y) in enumerate(zip(bits_of(funcs[first]), bits_of(funcs[second]))):
            pair[v * n_out] = x
            pair[v * n_out + 1] = y
        goals.append((tag, pair, np.ones_like(pair)))
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
        return (
            np.stack([self.tables[i] for i in idx]),
            np.stack([self.cells[i] for i in idx]),
        )

    def save(self, path: Path):
        rsd.write(
            path,
            rsd.Samples(
                self.header,
                np.stack(self.tables).astype(np.uint8) * 15,
                np.stack(self.cells),
            ),
        )


def simulate(header: rsd.Header, cells: np.ndarray, work: Path) -> np.ndarray:
    """Truth tables of layouts via redstone_datagen --tables (255 = broken)."""
    n = cells.shape[0]
    src, dst = work / "sketches.rsd", work / "sketches_tables.rsd"
    rsd.write(
        src, rsd.Samples(header, np.zeros((n, header.table_bytes), np.uint8), cells)
    )
    subprocess.run(
        [str(DATAGEN), "--spec", str(SPEC), "--tables", str(src), "--out", str(dst)],
        check=True,
        capture_output=True,
    )
    return rsd.read(dst).tables


def goal_spec(
    bits: np.ndarray, cares: np.ndarray, header: rsd.Header, path: Path
) -> None:
    """The box and ports of SPEC with the goal's rows and no deadline."""
    lines = [
        l
        for l in SPEC.read_text().splitlines()
        if l.split() and l.split()[0] in ("size", "input", "output")
    ]
    lines += ["timing none", "horizon 30"]
    for v in range(header.vectors):
        ins = ",".join(
            f"IN{i + 1}(ss{15 if (v >> i) & 1 else 0})" for i in range(header.inputs)
        )
        outs = [
            f"OUT{o + 1}({'ss1..15' if bits[k] else 'ss0'})"
            for o in range(header.outputs)
            for k in [v * header.outputs + o]
            if cares[k]
        ]
        if outs:
            lines.append(f"row {ins} -> {','.join(outs)}")
    path.write_text("\n".join(lines) + "\n")


def layout_from_hints(path: Path, base: np.ndarray, names: list[str]) -> np.ndarray:
    cells = np.where(base == len(names) - 1, base, 0).astype(np.uint8)
    for line in path.read_text().splitlines():
        w = line.split()
        if w and w[0] == "hint":
            x, y, z = int(w[1]), int(w[2]), int(w[3])
            cells[y, z, x] = names.index(" ".join(w[4:]))
    return cells


# ------------------------------------------------------------------ main


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    torch.manual_seed(SEED)
    params = {
        k: v
        for k, v in globals().items()
        if k.isupper() and isinstance(v, (int, float, str))
    }
    params.update(
        SPEC=str(SPEC),
        SEED_DATA=[str(p) for p in SEED_DATA],
        INIT_FROM=str(INIT_FROM),
        CLASS_NAMES=str(CLASS_NAMES),
    )
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
    names = json.loads(CLASS_NAMES.read_text())["classes"]
    searches = []  # (goal name, process, result path)

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
        if INIT_FROM.exists():
            model.load_state_dict(torch.load(INIT_FROM, weights_only=False)["model"])
            say(f"weights from {INIT_FROM}")
        for extra in seeds:
            store.add(extra.tables, extra.cells)
        say(
            f"seed data: {len(store.cells)} circuits, {len(store.by_function)} functions"
        )

    def train(steps: int) -> float:
        model.train()
        total = 0.0
        for step in range(steps):
            t, c = store.batch(BATCH, rng)
            cells = torch.from_numpy(c.astype(np.int64)).to(DEVICE)
            on = torch.from_numpy(t).to(DEVICE)
            # Some bits become don't-care, as in goals that fix one output
            # only: half the time a whole output, else scattered bits.
            cares = torch.ones_like(on)
            drop = torch.rand(BATCH, device=DEVICE) < CARE_DROP
            whole = torch.rand(BATCH, device=DEVICE) < 0.5
            which = torch.randint(0, header.outputs, (BATCH,), device=DEVICE)
            column = torch.arange(bits, device=DEVICE) % header.outputs
            out_mask = column[None, :] == which[:, None]
            bit_mask = torch.rand(BATCH, bits, device=DEVICE) < 0.3
            hide = torch.where(whole[:, None], out_mask, bit_mask) & drop[:, None]
            cares &= ~hide
            table = table_features(on, cares)
            ratio = mask_ratio(torch.rand(BATCH, device=DEVICE))
            hidden = torch.rand(cells.shape, device=DEVICE) < ratio[:, None, None, None]
            hidden &= ~fixed
            with torch.autocast("cuda", dtype=torch.bfloat16):
                logits = model(cells, hidden, static, table)
            loss = F.cross_entropy(
                logits.float().permute(0, 2, 3, 4, 1)[hidden], cells[hidden]
            )
            opt.zero_grad(set_to_none=True)
            loss.backward()
            opt.step()
            total += loss.item()
        return total / max(steps, 1)

    def random_table(r: random.Random) -> tuple[np.ndarray, np.ndarray]:
        table = np.array([r.random() < 0.5 for _ in range(bits)], bool)
        if r.random() < 0.5:
            return table, np.ones(bits, bool)
        column = np.arange(bits) % header.outputs
        return table, column == r.randrange(header.outputs)

    eval_rng = random.Random(RANDOM_EVAL_SEED)
    random_eval = [random_table(eval_rng) for _ in range(RANDOM_EVAL)]

    for rnd in range(start_round, ROUNDS):
        t0 = time.time()
        loss = train(FIRST_STEPS if rnd == 0 else STEPS_PER_ROUND)
        say(f"round {rnd}: trained, loss {loss:.3f} ({time.time() - t0:.0f}s)")

        # requests: goals, and known tables with one or two bits flipped
        known = [
            np.unpackbits(np.frombuffer(k, np.uint8))[:bits].astype(bool)
            for k in store.by_function
        ]
        known_arr = np.stack(known)
        req_bits, req_cares, req_goal = [], [], []
        n_goal = int(SKETCHES_PER_ROUND * GOAL_SHARE)
        n_random = int(SKETCHES_PER_ROUND * RANDOM_SHARE)
        for i in range(SKETCHES_PER_ROUND):
            if n_goal <= i < n_goal + n_random:
                j = i - n_goal
                table, cares = random_eval[j] if j < RANDOM_EVAL else random_table(rng)
                req_bits.append(table)
                req_cares.append(cares)
                req_goal.append(-2)
            elif i < n_goal:
                g = i % len(goals)
                req_bits.append(goals[g][1])
                req_cares.append(goals[g][2])
                req_goal.append(g)
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
                req_bits.append(b)
                req_cares.append(np.ones(bits, bool))
                req_goal.append(-1)
            else:
                b = rng.choice(known).copy()
                for _ in range(rng.choice((1, 2))):
                    b[rng.randrange(bits)] ^= True
                req_bits.append(b)
                req_cares.append(np.ones(bits, bool))
                req_goal.append(-1)
        model.eval()
        sketches = []
        for s in range(0, SKETCHES_PER_ROUND, SAMPLE_BATCH):
            tb = torch.from_numpy(np.stack(req_bits[s : s + SAMPLE_BATCH])).to(DEVICE)
            tc = torch.from_numpy(np.stack(req_cares[s : s + SAMPLE_BATCH])).to(DEVICE)
            with torch.autocast("cuda", dtype=torch.bfloat16):
                out = sample(
                    model,
                    static,
                    table_features(tb, tc),
                    fixed,
                    fixed_cells,
                    steps=SAMPLE_STEPS,
                )
            sketches.append(out.cpu().numpy().astype(np.uint8))
        sketches = np.concatenate(sketches)
        # circuits the search found while this round trained
        found = []
        for name, proc, result in searches:
            proc.wait()
            info = json.loads(Path(str(result) + ".json").read_text())
            say(
                f"   search {name}: {info['rows_right']}/{info['rows']} rows, "
                f"{info['blocks']} blocks"
            )
            found.append(layout_from_hints(result, seed.cells[0], names))
        searches = []
        if found:
            sketches = np.concatenate([sketches, np.stack(found)])
        tables = simulate(header, sketches, OUT_DIR)
        working = (tables != 255).all(axis=1)
        on = tables > 0
        varies = np.zeros(len(on), bool)
        for o in range(header.outputs):
            column = on[:, o :: header.outputs]
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
        n_req = len(req_bits)
        matched = working[:n_req] & (
            (on[:n_req] == np.stack(req_bits)) | ~np.stack(req_cares)
        ).all(axis=1)
        eval_hits = int(matched[n_goal : n_goal + RANDOM_EVAL].sum())
        random_hits = int(matched[n_goal : n_goal + n_random].sum())
        say(
            f"   random tables: {eval_hits}/{RANDOM_EVAL} of the fixed set, "
            f"{random_hits}/{n_random} of all random requests matched"
        )
        near = sum(
            1
            for i in range(n_goal + n_random, SKETCHES_PER_ROUND)
            if working[i] and (on[i] == req_bits[i]).all()
        )
        say(
            f"round {rnd}: {working.mean():.1%} sketches work, {added} new circuits, "
            f"{len(store.by_function) - functions_before} new functions "
            f"(total {len(store.by_function)}); exact for nearby tables "
            f"{near}/{SKETCHES_PER_ROUND - n_goal}"
        )
        say("   goals hit: " + ", ".join(f"{g[0]} {hits.get(g[0], 0)}" for g in goals))
        # seed searches with the closest working sketches for the hard goals
        for g_name in SEARCH_GOALS:
            _, g_bits, g_cares = next(g for g in goals if g[0] == g_name)
            wrong = (on[:, g_cares] != g_bits[g_cares]).sum(axis=1)
            wrong[~working] = 99
            best = np.argsort(wrong, kind="stable")[:SEARCH_SEEDS]
            tag = g_name.replace("=", "_")
            seeds_path = OUT_DIR / f"search_{tag}_seeds.rsd"
            rsd.write(
                seeds_path,
                rsd.Samples(
                    header, np.zeros((len(best), bits), np.uint8), sketches[best]
                ),
            )
            spec_path = OUT_DIR / f"search_{tag}_spec.txt"
            goal_spec(g_bits, g_cares, header, spec_path)
            result = OUT_DIR / f"search_{tag}_r{rnd}.txt"
            proc = subprocess.Popen(
                [
                    str(DATAGEN),
                    "--spec",
                    str(spec_path),
                    "--search",
                    str(SEARCH_SECONDS),
                    "--seeds",
                    str(seeds_path),
                    "--threads",
                    str(SEARCH_THREADS),
                    "--seed",
                    str(rnd + 1),
                    "--out",
                    str(result),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            searches.append((g_name, proc, result))
        torch.save(
            {"model": model.state_dict(), "opt": opt.state_dict(), "round": rnd}, ckpt
        )
        store.save(OUT_DIR / "data.rsd")


if __name__ == "__main__":
    main()
