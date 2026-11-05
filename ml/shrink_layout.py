"""Shrinks a verified layout (a spec with hint lines) one slice at a time:
removes a column (x) or row (z) without ports, shifts the rest together,
and lets the solver repair the sketch. Every cut of the current layout is
tried, several at a time; the first that verifies is kept and the search
starts over from it, until no cut works. Each kept step is saved.
"""

import subprocess
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path

from compose_full_adder import LINE, block_words

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc" / "Release" / "redstone_cli.exe"
SOURCE = (
    ROOT / "output" / "compose_full_adder" / "full_adder_10x2x6.txt"
)  # compose_full_adder.py output
OUT_DIR = ROOT / "output" / "shrink_layout"
SECONDS = 600  # per cut; repairs that worked took 10 s to 7 min
SKETCH_SECONDS = 30
PARALLEL = 3
THREADS = 4
AXES = ("x", "z")
SPEC_KEYS = (
    "size",
    "input",
    "output",
    "timing",
    "horizon",
    "settle",
    "row",
    "feedback",
    "palette",
)


def cut(lines: list[str], axis: str, k: int) -> list[str] | None:
    """The layout without slice k of axis, or None if a port is in it."""
    i = 0 if axis == "x" else 2
    out = []
    for line in lines:
        w = line.split()
        if not w:
            continue
        if w[0] == "size":
            size = [int(v) for v in w[1:4]]
            size[i] -= 1
            out.append(f"size {size[0]} {size[1]} {size[2]}")
            continue
        if w[0] in ("hint", "input", "output"):
            at = 1 if w[0] == "hint" else 3
            pos = [int(v) for v in w[at : at + 3]]
            if pos[i] == k:
                if w[0] != "hint":
                    return None
                continue
            if pos[i] > k:
                pos[i] -= 1
            out.append(" ".join(w[:at] + [str(v) for v in pos] + w[at + 3 :]))
            continue
        out.append(line)
    return out


def repaired(spec: Path) -> list[str] | None:
    """Solves a cut spec; returns it with the repaired layout as hints."""
    run = subprocess.run(
        [
            str(CLI),
            str(spec),
            "--time-limit",
            str(SECONDS),
            "--threads",
            str(THREADS),
            "--first",
        ]
        + ["--sketch-time", str(SKETCH_SECONDS)],
        capture_output=True,
        text=True,
        check=False,
    )
    spec.with_suffix(".log").write_text(run.stdout + run.stderr)
    if "verified a" not in run.stdout:
        return None
    lines = [
        l
        for l in spec.read_text().splitlines()
        if l.split() and l.split()[0] in SPEC_KEYS
    ]
    for line in run.stdout.splitlines():
        if (m := LINE.match(line.strip())) and not m[4].startswith(("input", "output")):
            lines.append(f"hint {m[1]} {m[2]} {m[3]} {block_words(m[4])}")
    return lines


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    log = (OUT_DIR / "log.txt").open("a")
    current = SOURCE.read_text().splitlines()
    step = 0
    while True:
        size = next(
            [int(v) for v in l.split()[1:4]] for l in current if l.startswith("size")
        )
        cuts = []
        for axis in AXES:
            for k in range(size[0 if axis == "x" else 2]):
                if (lines := cut(current, axis, k)) is not None:
                    path = OUT_DIR / f"step{step}_cut{axis}{k}.txt"
                    path.write_text("\n".join(lines) + "\n")
                    cuts.append(path)
        found = None
        with ThreadPoolExecutor(PARALLEL) as pool:
            pending = {pool.submit(repaired, path): path for path in cuts}
            while pending and found is None:
                done, _ = wait(pending, return_when=FIRST_COMPLETED)
                for job in done:
                    path = pending.pop(job)
                    if (result := job.result()) is not None and found is None:
                        found = (path, result)
            for job in pending:
                job.cancel()
        if found is None:
            line = f"{time.strftime('%H:%M:%S')} no cut of {size} works; done"
            print(line, flush=True)
            log.write(line + "\n")
            return
        path, current = found
        step += 1
        result = OUT_DIR / f"step{step}.txt"
        result.write_text("\n".join(current) + "\n")
        new_size = next(l for l in current if l.startswith("size"))
        line = f"{time.strftime('%H:%M:%S')} step {step}: {path.stem} -> {new_size} ({result.name})"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()


if __name__ == "__main__":
    main()
