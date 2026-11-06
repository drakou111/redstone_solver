package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonArray;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** An inclusive range of signal strengths, written "5", "1..15" or "*" (anything). */
public record SignalRange(int min, int max) {
    public static final int MAX_SIGNAL = 15;
    public static final SignalRange ANY = new SignalRange(0, MAX_SIGNAL);

    private static final Pattern RANGE = Pattern.compile("(\\d+)\\s*(?:(?:\\.\\.|-)\\s*(\\d+))?");

    public SignalRange {
        if (min < 0 || max > MAX_SIGNAL || min > max)
            throw new IllegalArgumentException("invalid signal range %d..%d".formatted(min, max));
    }

    public static SignalRange parse(String text) {
        String trimmed = text.trim();
        if (trimmed.isEmpty() || trimmed.equals("*")) return ANY;
        if (trimmed.startsWith("ss")) trimmed = trimmed.substring(2);
        Matcher matcher = RANGE.matcher(trimmed);
        if (!matcher.matches())
            throw new IllegalArgumentException("'" + text + "' is not a signal range (try 0, 15, 1..15 or *)");
        int min = Integer.parseInt(matcher.group(1));
        int max = matcher.group(2) == null ? min : Integer.parseInt(matcher.group(2));
        if (min > max || max > MAX_SIGNAL)
            throw new IllegalArgumentException("'" + text + "' is not a range within 0..15");
        return new SignalRange(min, max);
    }

    public JsonArray toJson() {
        JsonArray array = new JsonArray();
        array.add(this.min);
        array.add(this.max);
        return array;
    }

    @Override
    public String toString() {
        if (this.equals(ANY)) return "*";
        return this.min == this.max ? String.valueOf(this.min) : this.min + ".." + this.max;
    }
}
