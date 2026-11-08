# Ideas

Ranked by expected payoff for the effort. "Done" items are in the code; the
rest are open.

## Building circuits (like flush Compose, but better)

0. **Done: path-finding wire router.** Build from gates draws wires with A*
   (dust on blocks, repeaters when weak, one cell of clearance, crossings two
   layers up) in milliseconds, checks each stage by simulation and leaves a
   wire to SAT only when nothing routes. Joined-wire ORs route too (each
   signal through a repeater first). Dead ends go back to the runner-up
   partial circuits before falling back to SAT. Crossings need 4 layers.

1. **Beam search for flush placement.** Flush places gates greedily: the first
   position that wires up is kept. Keeping the best few partial circuits per
   gate (a beam) and finishing each one finds smaller circuits, since an early
   choice decides how much room later gates have. Each partial circuit is
   cheap (small corridor solves), so a beam of 3-4 costs little.
2. **Stacking modules (3D flush).** With a box 4 or more high, a gate module
   (2 high) can sit on top of another instead of beside it. The flush step
   already solves corridors in 3D; candidate spots one module height up would
   let the circuit grow in height instead of length.
3. **Cut-and-repair as the main search, not just a shrink.** Start from any
   verified circuit (flush, a library entry, the user's build) and shrink with
   near-seam repairs (done: 3 phases). Next: also try *moving* a slice (shift
   one half by one cell towards the other) and removing two slices at once,
   which escapes the greedy shrink's dead ends.
4. **Done: less greedy shrink (backtracking).** When no cut works, go back one step
   and try the next cut that also worked. The full adder ended at 7x2x6 or
   6x2x7 depending on which cut won first; keeping two or three branches
   would find the better end more often.
5. **Done (as tidy, in leftover shrink time): peephole re-synthesis.** Take a verified circuit, free one small window
   (3x2x3), and ask the solver for the fewest blocks there with everything
   else pinned. Repeat over all windows. Cuts blocks without changing size.
6. **Partly done: timing-aware gates.** The finished build is checked
   against the request's timing, and of the routed builds one in time wins.
   Still open: Build from gates ignores timing. Each module could
   be built with its own delay bound and the wiring stages checked against the
   deadline, so a 1-tick-per-gate budget gives fast circuits.
7. **Port placement freedom.** Let the user mark "any cell on the west face"
   for inputs; Compose already places ports itself, Search the box could too
   (free ports exist in the spec format).
8. **Gate library of hand-picked modules.** Seed the gate cache with known
   compact modules (XOR in 3x2x3 using a comparator subtract, etc.) found by
   long offline searches, stored like the circuit library.

## Speed

1. **Done: parallel shrink** (half the threads), both variants side by side,
   gate module cache, one deadline for the whole job.
2. **Incremental SAT across shrink cuts.** Every cut re-encodes the whole
   circuit. Cuts of the same layout share almost everything; one solver with
   assumptions per cut would skip the encoding.
3. **Partly done: verify faster.** Routed stages get one pass over the
   rows; shrink simulates each cut as it is before solving. Still open: The verifier checks every ordered pair of input cases
   (n^2 switches) at every placement. With "only as drawn" always on, one
   placement is enough; next, a cheap first pass with single-input switches
   only, and the full pass only for the final circuit.
4. **GPU simulation for the AI loop.** The datagen simulator is single
   threaded C++; batch simulation of sketches on the GPU (they are all the same
   small box) would make each training round several times faster.
5. **Precompute the gate network.** Exact synthesis for 3 inputs takes
   seconds, for 6 it gives up after 60 s. A table of the smallest networks for
   every 3-input function (256 of them) answers instantly.

## AI side

1. **Done: circuit library.** Every verified circuit is kept per request;
   repeats are instant. This is also the dataset the model needs: real
   requests and their best circuits.
2. **Retrieval sketches.** For a new request, find the library circuit with
   the closest truth table and the same port layout, and give it to Search the
   box as a sketch. Often only a few cells need to change.
3. **Train on the library.** The model trains on circuits from random search
   (datagen). Library circuits are smaller and answer real requests; mixing
   them in (and weighting them up) should make its sketches look more like
   good circuits.
4. **Measure what matters.** The random-table score barely moved over 52
   rounds. Better metric: solver time with the model's sketch vs. without, on a
   fixed set of requests. If the sketches don't speed up the solver, the model
   isn't helping, whatever its loss says.
5. **Model inside the app.** Today the model only runs from Python. Exporting
   it (ONNX) and running it in the host would let Search the box start from a
   model sketch automatically.
