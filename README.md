# redstone_solver

Searches for Minecraft Java Edition redstone circuits that satisfy a truth table over signal
strengths, within a time window and a build volume. The core is a game-tick simulator of vanilla
redstone (classic dust update order) used to verify candidates, and OR-Tools CP-SAT as the solver.
The solver encoding and UI are in progress.

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
ctest --preset release                          # run the test suite
build/msvc/Release/redstone_solver.exe --help
```
