# Solve methods

## Search the box

Turns the whole box and truth table into one big logic puzzle (SAT) and solves it exactly. Every candidate is checked in the simulator; in 2-high boxes Evolve runs beside it.

**Pros**
- Finds the smallest circuit and can prove it is the smallest.
- Can prove that no circuit fits the box.
- Handles analog signal strengths and timing exactly.

**Cons**
- Slows down fast as the box or truth table grows.
- Weak with loops: candidates that settle the wrong way are rejected one at a time.

## Build from gates

Splits the truth table into gates (AND, OR, XOR, ...), takes each gate from a library of solved modules, places them and routes wires between them.

**Pros**
- Handles big truth tables that Search the box cannot.
- Can put inputs and outputs in order, or in order or flipped.
- Stays within the box height and places the ports itself.

**Cons**
- On/off tables only, no analog values.
- Results are wide and loose; they need Then shrink.
- Never finds tricks beyond plain gates (comparator math, loops).

## Evolve

Fills a 2-high box block by block by trial and error: changes a few blocks, tests the result in the simulator, keeps what scores better, sometimes keeps worse to escape dead ends.

**Pros**
- Finds compact, clever designs, such as analog comparator tricks and loops that settle.
- Good at shrinking a working circuit you sketch.
- Shows its closest try as a hologram, with a score.

**Cons**
- 2-high boxes and on/off inputs only.
- No guarantees: it may stall one or two outputs short, and results vary from run to run.
- Loop circuits it finds can latch, so some "all right" tries are refused by the final check.

## Shrink

Not a solve method on its own: takes a working circuit and removes rows, columns and layers while it still works, then tidies blocks.

**Pros**
- Turns a loose result into a compact one.

**Cons**
- Only as good as the circuit it starts from.
- Slow on circuits with loops.
