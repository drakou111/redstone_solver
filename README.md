# redstone_solver

Finds Minecraft Java Edition redstone circuits from a truth table. You describe a box, where the
inputs and outputs are, what each output should be for each input combination (signal strengths
0-15), how fast it must react and which blocks it may use. The solver then looks for the circuit
with the fewest blocks. The circuit is encoded as SAT clauses and solved by several CaDiCaL
instances racing with different seeds; a game-tick simulator of vanilla redstone checks each
candidate in every orientation, and any failure is fed back as new clauses.

It comes as a Fabric mod: you describe the circuit in your world and the mod places the result.
The same C++ solver also has command line tools.

## Install the mod (players)

Requirements: Windows x64, Minecraft Java 26.3, [Fabric Loader](https://fabricmc.net/use/) 0.20.1
or newer, and [Fabric API](https://modrinth.com/mod/fabric-api).

1. Download `redstone-solver-mod-<version>.jar` from the Releases page.
2. Put it in your `mods/` folder and start the game.

On first start, the mod unpacks its solver into `.minecraft/redstone_solver/`. That happens once,
and again only when a new version of the mod brings a changed solver. Nothing else needs to be
installed. Linux and macOS are not supported yet: the jar carries only the Windows solver.

## Use it in game

In a world with cheats (all keys can be rebound under Controls):

- **J** and **K** on two corner blocks set the box. Its bottom layer is kept for support blocks.
- Place a repeater, comparator, dust or barrel where a port goes, look at it and press **I** or
  **O** to make it an input or output (its kind and facing come from the block).
- **P** opens a menu to keep a block, force a cell empty or clear marks.
- **R** opens the Solver panel. There you set the truth table (one row per input combination, or
  one formula per output such as `(A + B) % 16`), the timing, the allowed blocks, the time limit
  and the method. Then press Solve, and Place, Discard or Shrink the result.

The panel's methods (details and trade-offs in [docs/solve_methods.md](docs/solve_methods.md)):

- **Search the box:** an exact search of the whole box for the fewest blocks. It is the best for
  small circuits, and it also runs Evolve alongside.
- **Build from gates:** splits the truth table into 2-input gates and wires prebuilt gate modules
  together. It handles larger circuits, though they come out bigger.
- **Evolve:** a randomized search that keeps the closest tries. It can find circuits the exact
  search runs out of time on, and shows its best attempt so far as a hologram.
- **Then shrink:** after Build from gates or Evolve, removes rows and columns while the circuit
  still works.

The options:

- **Loops** allows feedback loops and repeater locks. Leave it off for plain logic; turn it on
  when a memory-like circuit is fine.
- **Inputs** and **Outputs** say whether the ports must keep the order you placed them in, or may
  be swapped or flipped.

Your draft is saved per world in `config/redstone_solver/drafts` and restored when you come back.
`/rs save <name>` and `/rs load <name>` keep named copies, Export copies the circuit as
`/setblock` commands, and `/rs help` lists everything as typed commands.

If the solver fails, the game log (`logs/latest.log`) has its output and exit code; please
include it in bug reports.

## Build from source

Windows: Visual Studio 2022 (MSVC), CMake 3.25+ and a JDK 25 for the mod. Use a CMake
with working TLS, such as the one bundled with Visual Studio or CLion; MSYS2/MinGW CMake cannot
verify GitHub's certificate. OR-Tools and the other libraries are downloaded on
the first build.

```
cmake --preset msvc
cmake --build --preset release
ctest --preset release      # C++ tests
```

Only `Release` and `RelWithDebInfo` exist, because the prebuilt OR-Tools is release-only.

The mod (in `mod/`) bundles the solver you just built, so build that first. Gradle runs itself on
a JDK 25 it finds installed:

```
cd mod
./gradlew build          # jar in mod/build/libs; the tests run the real solver
./gradlew runClient      # a dev game using build/msvc/Release/redstone_solver.exe
```

To use a solver other than the bundled one, set `solver_exe` in
`config/redstone_solver.properties`.

## Command line

```
build/msvc/Release/redstone_cli.exe examples/and_gate.txt --time-limit 60
build/msvc/Release/redstone_cli.exe my_sketch.txt --check    # simulate the layout in its hint lines
build/msvc/Release/redstone_evolve.exe examples/and_gate.txt --seconds 60
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

More spec lines:

- **Free ports:** `input IN1 repeater free`, plus one or more `region IN1 0 1 0..2` lines, lets
  the solver place the port within those cells and pick its facing.
- **`support_layer on`** (the default) keeps the bottom layer to solid support blocks, which
  makes solving much faster.
- **`tile z`** asks for a circuit whose copies can sit side by side along z.
- **`feedback off`** (the default) rules out loops and repeater locks.

## Repository layout

| Path | Contents |
|---|---|
| `src/redstone/` | solver library: simulator, SAT encoder, compose, shrink, evolve |
| `src/host/` | `redstone_solver.exe`: the local HTTP host the mod starts and talks to |
| `src/app`, `src/evolve`, `src/bench`, `src/datagen` | command line tools (`redstone_cli`, `redstone_evolve`, ...) |
| `mod/` | the Fabric mod |
| `tests/`, `bench/`, `examples/` | C++ tests, benchmark specs, example specs |
| `docs/` | how the solve methods compare; `ideas.md` is the roadmap of open ideas |
| `ml/` | experimental: a PyTorch circuit sketcher and layout scripts, not used by the mod or app |

## License

MIT, see [LICENSE](LICENSE). The solver and the mod jar include third-party libraries under
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
