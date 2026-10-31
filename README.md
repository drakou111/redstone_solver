# redstone_solver

Finds Minecraft Java Edition redstone circuits that satisfy a truth table over signal strengths,
within a time window and a build volume. A CP-SAT model (OR-Tools) proposes the circuit with the
fewest blocks; a game-tick simulator of vanilla redstone (classic dust update order) then checks it
at 8 orientations and several world positions, and any counterexample is fed back into the model.

## Setup

Windows: Visual Studio 2022 (MSVC) and CMake 3.25+. Use a CMake build with working TLS (the one
bundled with Visual Studio or CLion; MSYS2/MinGW CMake cannot verify GitHub's certificate).
OR-Tools 9.15 and Catch2 are downloaded automatically on first configure.

```
cmake --preset msvc
cmake --build --preset release
```

Only `Release` and `RelWithDebInfo` exist: the prebuilt OR-Tools is release-only, so a Debug
runtime cannot link against it. In CLion, select the Visual Studio toolchain and the `msvc` preset.

Other platforms: install OR-Tools, put it on `CMAKE_PREFIX_PATH`, and configure normally.

## Usage

```
build/msvc/Release/redstone_solver.exe examples/and_gate.txt --time-limit 60
build/msvc/Release/redstone_solver.exe examples/and_gate.txt --first   # stop at the first verified circuit
ctest --preset release
```

A spec file describes the volume, the ports, the timing and the truth table (see `examples/` and
`--help` for every keyword):

```
size 4 3 3                          # x y z
input IN1 strong 0 1 0 east         # comparator-like source facing east
input IN2 strong 0 1 2 east
output OUT1 dust 3 1 1              # output read from this dust
timing 4                            # correct within 4 game ticks ("2rt" = redstone ticks)
row IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15)
row IN1(ss0) -> OUT1(ss0)
row IN2(ss0) -> OUT1(ss0)
```

`tile z` asks for a circuit whose copies can sit side by side along z without influencing each
other. `feedback off` (the default) rules out loops and repeater locks, which would act as memory.
