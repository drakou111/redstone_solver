package io.github.drakou111.redstonesolver.core;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Durations typed as "90", "90s", "10m", "1.5h" (plain numbers are seconds). */
public final class Durations {
    private static final Pattern DURATION = Pattern.compile("(\\d+(?:\\.\\d+)?)\\s*([smh]?)");
    private static final int SECONDS_PER_MINUTE = 60;
    private static final int SECONDS_PER_HOUR = 3600;

    private Durations() {
    }

    public static double parseSeconds(String text) {
        Matcher matcher = DURATION.matcher(text.trim().toLowerCase(Locale.ROOT));
        if (!matcher.matches()) throw new IllegalArgumentException("'" + text + "' is not a time; try 90s, 10m or 1h");
        double value = Double.parseDouble(matcher.group(1));
        double seconds = switch (matcher.group(2)) {
            case "m" -> value * SECONDS_PER_MINUTE;
            case "h" -> value * SECONDS_PER_HOUR;
            default -> value;
        };
        if (!(seconds > 0)) throw new IllegalArgumentException("a time must be more than zero");
        return seconds;
    }

    /** The shortest exact form: whole hours or minutes when possible, else seconds. */
    public static String format(double seconds) {
        if (seconds == Math.rint(seconds)) {
            long whole = (long) seconds;
            if (whole % SECONDS_PER_HOUR == 0) return whole / SECONDS_PER_HOUR + "h";
            if (whole % SECONDS_PER_MINUTE == 0) return whole / SECONDS_PER_MINUTE + "m";
            return whole + "s";
        }
        return seconds + "s";
    }
}
