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

Command line and tests:

```
build/msvc/Release/redstone_cli.exe examples/and_gate.txt --time-limit 60
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

`support_layer on` (the default) keeps the bottom layer to wool/glass supports, which makes solving
much faster. `tile z` asks for a circuit whose copies can sit side by side along z without influencing each
other. `feedback off` (the default) rules out loops and repeater locks, which would act as memory.
