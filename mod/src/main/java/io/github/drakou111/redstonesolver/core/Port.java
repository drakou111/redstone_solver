package io.github.drakou111.redstonesolver.core;

import java.util.Objects;

/**
 * An input or output of the circuit at a world position. {@code facing} is the direction the
 * signal travels and only matters for repeater and comparator ports.
 */
public record Port(String name, boolean input, String kind, Cell position, Dir facing) {
    public Port {
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(kind, "kind");
        Objects.requireNonNull(position, "position");
        Objects.requireNonNull(facing, "facing");
        boolean known = input ? InputKind.parse(kind).isPresent() : OutputKind.parse(kind).isPresent();
        if (!known) throw new IllegalArgumentException("unknown " + (input ? "input" : "output") + " kind " + kind);
    }

    public static Port input(String name, InputKind kind, Cell position, Dir facing) {
        return new Port(name, true, kind.jsonName(), position, facing);
    }

    public static Port output(String name, OutputKind kind, Cell position, Dir facing) {
        return new Port(name, false, kind.jsonName(), position, facing);
    }

    public boolean hasFacing() {
        return this.input
                ? InputKind.parse(this.kind).orElseThrow().hasFacing()
                : OutputKind.parse(this.kind).orElseThrow().hasFacing();
    }

    public String describe() {
        String where = "at " + this.position;
        return this.hasFacing()
                ? "%s: %s %s, facing %s".formatted(this.name, this.kind, where, this.facing.jsonName())
                : "%s: %s %s".formatted(this.name, this.kind, where);
    }
}
