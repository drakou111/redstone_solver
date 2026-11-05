# redstone_solver

Finds Minecraft Java Edition redstone circuits that satisfy a truth table over signal strengths,
within a time window and a build volume. A CP-SAT model (OR-Tools) proposes the circuit with the
fewest blocks; a game-tick simulator of vanilla redstone (classic dust update order) then checks it
at 8 orientations and several world positions, and any counterexample is fed back into the model.

## Setup

Windows: Visual Studio 2022 (MSVC), CMake 3.25+ and Node.js 20+. Use a CMake build with working
TLS (the one bundled with Visual Studio or CLion; MSYS2/MinGW CMake cannot verify GitHub's
certificate). OR-Tools, the web UI's npm packages and the other libraries are downloaded on the
first build.

```
cmake --preset msvc
cmake --build --preset release
```

Only `Release` and `RelWithDebInfo` exist: the prebuilt OR-Tools is release-only.

## Usage

Run `build/msvc/Release/redstone_solver.exe`. Describe the circuit on the left (volume, inputs and
outputs with their block type, truth table, timing, allowed blocks), or directly in the 3D view
with the tools at its top: add or move ports, pre-place blocks the solver must keep, and mark cells
it must leave empty. Press Solve, then explore the circuit: switch inputs by clicking them or with
the sliders, step the simulator, edit blocks (every edit is re-checked) and export `/setblock`
commands. Block textures are read from your own Minecraft client jar (found automatically for
Prism and the vanilla launcher); nothing from Minecraft is bundled.

Next to Solve: "Compose full adder" builds a full adder from two solver-made half adders in
seconds (it places the ports itself), Shrink removes columns and rows from a solved circuit while
it still works, and Sketch checks, keeps or clears hint blocks (a draft the solver starts from).
Options holds "only as drawn" (faster circuits that may fail when built rotated or elsewhere).

Command line and tests:

```
build/msvc/Release/redstone_cli.exe examples/and_gate.txt --time-limit 60
build/msvc/Release/redstone_cli.exe my_sketch.txt --check   # simulate the layout in its hint lines
build/msvc/Release/redstone_datagen.exe --spec examples/full_adder.txt --count 100000 --out data.rsd
python ml/loop3.py                            # train the circuit sketcher (PyTorch, CUDA)
python ml/compose_full_adder.py               # flat full adder from two SAT-made half adders (~10 s)
python ml/shrink_layout.py                    # shrink it a column or row at a time (minutes per step)
ctest --preset release                        # C++ tests
cd web && npm test && npx playwright test     # UI unit and end-to-end tests
```

A spec file (see `examples/` and `redstone_cli --help`):

```
size 4 3 3                          # x y z
input IN1 comparator 0 1 0 east     # comparator input facing east (needs a block below)
input IN2 comparator 0 1 2 east
output OUT1 dust 3 1 1              # output read from this dust
timing 4                            # correct within 4 game ticks ("2rt" = redstone ticks)
keep 1 0 1 solid                    # optional: block the solver must keep
empty 2 2 1                         # optional: cell the solver must leave empty
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
```

A port can also be placed by the solver: `input IN1 repeater free` plus one or more
`region IN1 0 1 0..2` lines listing the cells it may use (ranges allowed). The solver also picks
the facing of repeater and comparator ports. In the app, set a port to "solver picks" and draw its
cells with the Port area tool (P).

`support_layer on` (the default) keeps the bottom layer to solid (wool) support blocks, which makes solving
much faster. `tile z` asks for a circuit whose copies can sit side by side along z without influencing each
other. `feedback off` (the default) rules out loops and repeater locks, which would act as memory.
