package io.github.drakou111.redstonesolver.core;

import java.util.Objects;

/** Solver progress shaped for one line of text (the action bar draws line breaks as squares). */
public final class ProgressLine {
    public static final int MAX_LENGTH = 90;
    private static final String ELLIPSIS = "...";

    private ProgressLine() {
    }

    /** The first non-empty line of {@code text}, cut to {@code maxLength} characters with "..." when longer. */
    public static String of(String text, int maxLength) {
        Objects.requireNonNull(text, "text");
        if (maxLength <= ELLIPSIS.length())
            throw new IllegalArgumentException("maxLength must be more than " + ELLIPSIS.length());

        String line = "";
        for (String part : text.split("\\R")) {
            String trimmed = part.strip();
            if (!trimmed.isEmpty()) {
                line = trimmed;
                break;
            }
        }

        if (line.length() <= maxLength) return line;
        return line.substring(0, maxLength - ELLIPSIS.length()) + ELLIPSIS;
    }

    public static String of(String text) {
        return of(text, MAX_LENGTH);
    }
}
