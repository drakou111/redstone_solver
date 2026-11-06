package io.github.drakou111.redstonesolver.core;

import java.util.Locale;
import java.util.Optional;

/** The six directions, named and laid out like the solver's (north = -z, east = +x). */
public enum Dir {
    DOWN(0, -1, 0),
    UP(0, 1, 0),
    NORTH(0, 0, -1),
    SOUTH(0, 0, 1),
    WEST(-1, 0, 0),
    EAST(1, 0, 0);

    private final int dx;
    private final int dy;
    private final int dz;

    Dir(int dx, int dy, int dz) {
        this.dx = dx;
        this.dy = dy;
        this.dz = dz;
    }

    public String jsonName() {
        return this.name().toLowerCase(Locale.ROOT);
    }

    public Dir opposite() {
        return switch (this) {
            case DOWN -> UP;
            case UP -> DOWN;
            case NORTH -> SOUTH;
            case SOUTH -> NORTH;
            case WEST -> EAST;
            case EAST -> WEST;
        };
    }

    public boolean isHorizontal() {
        return this.dy == 0;
    }

    public Cell offset() {
        return new Cell(this.dx, this.dy, this.dz);
    }

    public static Optional<Dir> parse(String name) {
        for (Dir dir : values()) {
            if (dir.jsonName().equals(name)) return Optional.of(dir);
        }
        return Optional.empty();
    }
}
