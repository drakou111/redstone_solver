package io.github.drakou111.redstonesolver.core;

import java.util.Locale;
import java.util.Optional;

/** What reads an output port; the solver's names. */
public enum OutputKind {
    DUST,
    REPEATER,
    COMPARATOR;

    public String jsonName() {
        return this.name().toLowerCase(Locale.ROOT);
    }

    public boolean hasFacing() {
        return this != DUST;
    }

    public static Optional<OutputKind> parse(String name) {
        for (OutputKind kind : values()) {
            if (kind.jsonName().equals(name)) return Optional.of(kind);
        }
        return Optional.empty();
    }
}
