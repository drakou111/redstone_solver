package io.github.drakou111.redstonesolver.core;

import java.util.Locale;
import java.util.Optional;

/** What drives an input port; the solver's names. */
public enum InputKind {
    COMPARATOR,
    REPEATER,
    DUST,
    CONTAINER;

    public String jsonName() {
        return this.name().toLowerCase(Locale.ROOT);
    }

    /** Repeater and comparator ports send their signal along a facing. */
    public boolean hasFacing() {
        return this == COMPARATOR || this == REPEATER;
    }

    public static Optional<InputKind> parse(String name) {
        for (InputKind kind : values()) {
            if (kind.jsonName().equals(name)) return Optional.of(kind);
        }
        return Optional.empty();
    }
}
