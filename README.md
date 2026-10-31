# redstone_solver

Searches for Minecraft Java Edition redstone circuits that satisfy a truth table over signal
strengths, within a time window and a build volume. The core is a game-tick simulator of vanilla
redstone (classic dust update order) used to verify candidates; the solver and UI are in progress.

## Setup

Requires CMake 3.25+, Ninja and a C++23 compiler (tested with MinGW-w64 GCC 15). Catch2 is
fetched automatically.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

```
build/redstone_tests      # run the simulator test suite
build/redstone_solver --help
```
