package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonObject;
import java.util.Optional;

/**
 * A block the player placed to mark a port, and what it can stand for: a repeater, comparator or
 * dust can be an input or an output, a barrel only an input. {@code facing} is the direction the
 * signal travels (ignored for dust and barrels).
 */
public record PortBlock(String blockName, Optional<InputKind> input, Optional<OutputKind> output, Dir facing) {
    /** The port a solver block (as read from the world) can be, if any. */
    public static Optional<PortBlock> fromSolverBlock(JsonObject block) {
        String type = block.get("type").getAsString();
        Dir facing = block.has("facing")
                ? Dir.parse(block.get("facing").getAsString()).orElse(Dir.NORTH)
                : Dir.NORTH;
        return switch (type) {
            case "repeater" -> Optional.of(new PortBlock(type, Optional.of(InputKind.REPEATER),
                    Optional.of(OutputKind.REPEATER), facing));
            case "comparator" -> Optional.of(new PortBlock(type, Optional.of(InputKind.COMPARATOR),
                    Optional.of(OutputKind.COMPARATOR), facing));
            case "dust" -> Optional.of(new PortBlock(type, Optional.of(InputKind.DUST), Optional.of(OutputKind.DUST),
                    Dir.NORTH));
            case "barrel" -> Optional.of(new PortBlock(type, Optional.of(InputKind.CONTAINER), Optional.empty(),
                    Dir.NORTH));
            default -> Optional.empty();
        };
    }

    public Port toPort(String name, boolean asInput, Cell cell) {
        if (asInput) {
            InputKind kind = this.input.orElseThrow(() -> new IllegalArgumentException(
                    "a " + this.blockName + " cannot be an input"));
            return Port.input(name, kind, cell, this.facing);
        }
        OutputKind kind = this.output.orElseThrow(() -> new IllegalArgumentException(
                "a " + this.blockName + " cannot be an output"));
        return Port.output(name, kind, cell, this.facing);
    }

    public String describe() {
        boolean directional = this.input.map(InputKind::hasFacing).orElse(false);
        return directional ? this.blockName + ", signal going " + this.facing.jsonName() : this.blockName;
    }
}
