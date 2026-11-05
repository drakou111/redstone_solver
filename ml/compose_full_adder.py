"""Builds a full adder from two SAT-made half adders, then lets the solver
wire them, one small problem at a time:

1. module: a half adder in a small box (MODULE_SIZE) whose east and west
   faces are empty except for its ports, so two copies can touch.
2. stage 1: HA1(A, B) and HA2(x, C) pinned side by side; HA1's XOR output x
   feeds HA2 directly. The solver only routes C (south of HA1) and is asked
   for the sum alone.
3. stage 2: stage 1's circuit pinned; the solver routes HA1's AND output
   north of HA2 into HA2's AND output, giving the carry.

Every solve frees only the cells its wire needs, which keeps it fast. The
result is a spec with hint lines, checked by redstone_cli --check.
"""

import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc" / "Release" / "redstone_cli.exe"
OUT_DIR = ROOT / "output" / "compose_full_adder"
MODULE_SIZE = (5, 3, 3)  # x, y, z of the half adder module
# Rows of the module's outputs on its east face. HA2 is shifted down by
# XOR_ROW so its first input meets HA1's XOR output; AND_ROW < XOR_ROW lets
# HA1's AND output pass north of HA2. XOR_ROW 1 gives a box one row shorter
# when such a module exists (not for 5x3x3; 6x3x3 gives 12x3x4).
XOR_ROW = 2
AND_ROW = 0
HORIZON = 80  # ticks to come to rest (two gates in a chain are slow)
MODULE_SECONDS = 600
STAGE_SECONDS = 600
THREADS = 8
LINE = re.compile(r"^\((\d+), (\d+), (\d+)\) (.+)$")

HALF_ADDER_ROWS = [
    "row IN1(ss0),IN2(ss0) -> OUT1(ss0),OUT2(ss0)",
    "row IN1(ss15),IN2(ss0) -> OUT1(ss1..15),OUT2(ss0)",
    "row IN1(ss0),IN2(ss15) -> OUT1(ss1..15),OUT2(ss0)",
    "row IN1(ss15),IN2(ss15) -> OUT1(ss0),OUT2(ss1..15)",
]


def full_adder_rows(with_carry: bool) -> list[str]:
    rows = []
    for v in range(8):
        a, b, c = v & 1, (v >> 1) & 1, (v >> 2) & 1
        ins = f"IN1(ss{15 * a}),IN2(ss{15 * b}),IN3(ss{15 * c})"
        outs = f"OUT1({'ss1..15' if (a + b + c) % 2 else 'ss0'})"
        if with_carry:
            outs += f",OUT2({'ss1..15' if a + b + c >= 2 else 'ss0'})"
        rows.append(f"row {ins} -> {outs}")
    return rows


def block_words(text: str) -> str:
    """CLI block text -> spec block words; ports become plain blocks."""
    if m := re.fullmatch(r"input\d+\(repeater, (\w+)\)", text):
        return f"repeater {m[1]} 1"
    if re.fullmatch(r"output\d+\(dust, \w+\)", text):
        return "dust"
    if m := re.fullmatch(r"torch\(on (\w+)\)", text):
        return f"torch {m[1]}"
    if m := re.fullmatch(r"repeater\((\w+), delay (\d)\)", text):
        return f"repeater {m[1]} {m[2]}"
    if m := re.fullmatch(r"comparator\((\w+), (\w+)\)", text):
        return f"comparator {m[1]} {m[2]}"
    if m := re.fullmatch(r"observer\(watching (\w+)\)", text):
        return f"observer {m[1]}"
    if m := re.fullmatch(r"(hopper|barrel|chest)\(ss (\d+)\)", text):
        return f"{m[1]} {m[2]}"
    return text


def read_blocks(log: Path) -> dict[tuple[int, int, int], str]:
    blocks = {}
    for line in log.read_text().splitlines():
        if m := LINE.match(line.strip()):
            blocks[(int(m[1]), int(m[2]), int(m[3]))] = block_words(m[4])
    return blocks


@dataclass(frozen=True)
class Box:
    size: tuple[int, int, int]
    ports: list[str]
    port_cells: frozenset


def spec_text(box: Box, rows: list[str], pinned: dict, free) -> str:
    """Pinned blocks become keep lines; cells neither pinned nor free stay
    empty, so the solver only searches where the wire can go."""
    lines = [
        f"size {box.size[0]} {box.size[1]} {box.size[2]}",
        *box.ports,
        "timing none",
        f"horizon {HORIZON}",
        *rows,
    ]
    for (x, y, z), words in sorted(pinned.items()):
        if (x, y, z) not in box.port_cells:
            lines.append(f"keep {x} {y} {z} {words}")
    for x in range(box.size[0]):
        for y in range(box.size[1]):
            for z in range(box.size[2]):
                p = (x, y, z)
                if p not in pinned and p not in box.port_cells and not free(p):
                    lines.append(f"empty {x} {y} {z}")
    return "\n".join(lines) + "\n"


def solve(spec: Path, seconds: int, log) -> Path | None:
    """Runs the solver; returns its log if it verified a circuit."""
    run = subprocess.run(
        [
            str(CLI),
            str(spec),
            "--time-limit",
            str(seconds),
            "--threads",
            str(THREADS),
            "--first",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    out = spec.with_suffix(".log")
    out.write_text(run.stdout + run.stderr)
    status = next(
        (l for l in out.read_text().splitlines() if l.startswith("Status")), "no status"
    )
    found = "verified a" in out.read_text()
    line = f"{time.strftime('%H:%M:%S')} {spec.stem}: {'found' if found else 'none'}; {status}"
    print(line, flush=True)
    log.write(line + "\n")
    log.flush()
    return out if found else None


def make_module(log) -> Path | None:
    """Half adder: IN1 (0,1,0), IN2 (0,1,mz-1) east; OUT1 xor (mx-1,1,2),
    OUT2 and (mx-1,1,0); east and west faces empty except the ports."""
    mx, my, mz = MODULE_SIZE
    ports = [
        "input IN1 repeater 0 1 0 east",
        f"input IN2 repeater 0 1 {mz - 1} east",
        f"output OUT1 dust {mx - 1} 1 {XOR_ROW}",
        f"output OUT2 dust {mx - 1} 1 {AND_ROW}",
    ]
    keep_open = {(0, 1, 0), (0, 1, mz - 1), (mx - 1, 1, XOR_ROW), (mx - 1, 1, AND_ROW)}
    lines = [
        f"size {mx} {my} {mz}",
        *ports,
        "timing none",
        "horizon 30",
        *HALF_ADDER_ROWS,
    ]
    for x in (0, mx - 1):
        for y in range(1, my):
            for z in range(mz):
                if (x, y, z) not in keep_open:
                    lines.append(f"empty {x} {y} {z}")
    spec = OUT_DIR / "module.txt"
    spec.write_text("\n".join(lines) + "\n")
    return solve(spec, MODULE_SECONDS, log)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    log = (OUT_DIR / "log.txt").open("a")
    module_log = make_module(log)
    if module_log is None:
        return
    module = read_blocks(module_log)
    mx, my, mz = MODULE_SIZE
    x2, z2 = mx, XOR_ROW  # HA2 right after HA1, its IN1 at HA1's XOR output
    z_c = mz  # C enters one row below HA1
    size = (x2 + mx, my, max(z2 + mz, z_c + 1))
    out1 = (x2 + mx - 1, 1, z2 + XOR_ROW)  # HA2's XOR output
    out2 = (x2 + mx - 1, 1, z2 + AND_ROW)  # HA2's AND output
    inputs = [
        "input IN1 repeater 0 1 0 east",
        f"input IN2 repeater 0 1 {mz - 1} east",
        f"input IN3 repeater 0 1 {z_c} east",
    ]
    in_cells = {(0, 1, 0), (0, 1, mz - 1), (0, 1, z_c)}
    pinned = dict(module)
    for (x, y, z), words in module.items():
        pinned[(x + x2, y, z + z2)] = words

    stage1 = Box(
        size,
        [*inputs, f"output OUT1 dust {out1[0]} 1 {out1[2]}"],
        frozenset(in_cells | {out1}),
    )
    spec1 = OUT_DIR / "stage1.txt"
    spec1.write_text(
        spec_text(
            stage1,
            full_adder_rows(False),
            pinned,
            free=lambda p: p[1] == 0 or (p[2] >= mz and p[0] < x2),
        )
    )
    log1 = solve(spec1, STAGE_SECONDS, log)
    if log1 is None:
        return

    stage2 = Box(
        size,
        [
            *inputs,
            f"output OUT1 dust {out1[0]} 1 {out1[2]}",
            f"output OUT2 dust {out2[0]} 1 {out2[2]}",
        ],
        frozenset(in_cells | {out1, out2}),
    )
    spec2 = OUT_DIR / "stage2.txt"
    spec2.write_text(
        spec_text(
            stage2,
            full_adder_rows(True),
            read_blocks(log1),
            free=lambda p: p[1] == 0 or (p[2] < z2 and p[0] >= mx - 1),
        )
    )
    log2 = solve(spec2, STAGE_SECONDS, log)
    if log2 is None:
        return

    result = OUT_DIR / f"full_adder_{size[0]}x{size[1]}x{size[2]}.txt"
    header = [
        l
        for l in spec2.read_text().splitlines()
        if l.split()[0] in ("size", "input", "output", "timing", "horizon", "row")
    ]
    hints = [
        f"hint {x} {y} {z} {w}"
        for (x, y, z), w in sorted(read_blocks(log2).items())
        if (x, y, z) not in stage2.port_cells and (x, y, z) not in in_cells
    ]
    result.write_text("\n".join(header + hints) + "\n")
    check = subprocess.run(
        [str(CLI), str(result), "--check"], capture_output=True, text=True, check=False
    )
    verdict = check.stdout.strip().splitlines()[-1]
    log.write(f"{result.name}: {len(hints)} blocks, check {verdict}\n")
    print(f"{result.name}: {len(hints)} blocks, check {verdict}")


if __name__ == "__main__":
    main()
