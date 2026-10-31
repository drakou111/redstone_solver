The goal of this project is to make a SAT solver (or some variant) to solve minecraft redstone problems (JAVA 26.3).

You will need to accurately simulate minecraft redstone, optimize it and solve for certain tasks.

Currently, I think that having to *simulate* things will make the runtime of the sat solver incredibly slow, so I don't think it is a good idea to implement pistons (as they can change the state of the machine itself and etc) and item transportation (hoppers/dispensers). Ideally, instead of verifying with a simulator, we should force with SAT constraints.

Here are the blocks that i want you to simulate:

- Air
- Solid Blocks (cyan wool)
- Transparent Blocks (cyan glass)
- Redstone dust
- Redstone torch
- Redstone block
- Repeater
- Comparator
- Target block
- Noteblock
- Hopper (acts similarly to noteblock, but is transparent and can still have dust on it)
- Barrel (solid block that can hold a signal strength 0-15)
- Chest (transparent block that can hold a signal strength 0-15)
- Observer
- (If you have more suggestions of inputs, feel free)

There should also be custom blocks placed by the user (maybe add a thing in the future that lets the SAT solver place them too?):
- Input block
- Output block

For both of those, i think a good idea is to add a "power type", like weak (EG: barrel for input, dust for output) or strong (EG: comparator for input, comparator for output).

The goal of the SAT solver should be to respect some kind of truth table from the inputs and outputs. For example, this would be an AND gate:
1. IN1(ss1..15),IN2(ss1..15) -> OUT1(ss1..15) // if input 1 has signal strength from 1 to 15 and input 2 has signal strength from 1 to 15, then out should have an output ranging from 1 to 15.
2. IN1(ss0),IN2(ss1..15) -> OUT1(ss0)
3. IN1(ss1..15),IN2(ss0) -> OUT1(ss0)
4. IN1(ss0),IN2(ss0) -> OUT1(ss0)

(in this format, there are some special cases like OUT1(ss0..15), which means "can be anything", or "IN1(ss0..15)" which is "for any input")

Another parameter should be the "output time range". For example, i might want the redstone to compute the outputs in <12t, or [12,16]t, or no range at all.

Yet another parameter should specify the volume. (2D/3D? max XYZ dimensions? Should it be tilable on some axis (or multiple) ? (tilable would mean can be right next to each other without any interference)). If you have other ideas feel free to suggest.

Now, another thing to be careful of is how redstone dust ordering works. Certain redstone machines are locational/rotational, so we must be aware of that. Ideally, we should restrict ourselves to redstone that works regardless of locationality or rotationality.

I expect the final result to be a .exe file which has a clean UI for this.