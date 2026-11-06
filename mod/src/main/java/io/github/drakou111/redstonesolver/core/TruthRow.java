package io.github.drakou111.redstonesolver.core;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * One truth table row by port name, written {@code IN1=1..15 IN2=0 -> OUT1=0}. Inputs left out
 * mean any value; outputs left out may be anything.
 */
public record TruthRow(Map<String, SignalRange> inputs, Map<String, SignalRange> outputs) {
    private static final String ARROW = "->";

    public TruthRow {
        inputs = Collections.unmodifiableMap(new LinkedHashMap<>(inputs));
        outputs = Collections.unmodifiableMap(new LinkedHashMap<>(outputs));
        if (outputs.isEmpty())
            throw new IllegalArgumentException("a row needs at least one output, like -> OUT1=0");
    }

    public static TruthRow parse(String text) {
        int arrow = text.indexOf(ARROW);
        if (arrow < 0) throw new IllegalArgumentException("a row looks like: IN1=1..15 IN2=0 -> OUT1=0");
        Map<String, SignalRange> inputs = parseTerms(text.substring(0, arrow));
        Map<String, SignalRange> outputs = parseTerms(text.substring(arrow + ARROW.length()));
        return new TruthRow(inputs, outputs);
    }

    /** The ranges in port order, as the solver expects them. */
    public List<SignalRange> rangesFor(List<String> portNames, boolean input) {
        Map<String, SignalRange> terms = input ? this.inputs : this.outputs;
        List<SignalRange> ranges = new ArrayList<>(portNames.size());
        for (String name : portNames) ranges.add(terms.getOrDefault(name, SignalRange.ANY));
        return ranges;
    }

    @Override
    public String toString() {
        return formatTerms(this.inputs) + " " + ARROW + " " + formatTerms(this.outputs);
    }

    private static Map<String, SignalRange> parseTerms(String text) {
        Map<String, SignalRange> terms = new LinkedHashMap<>();
        for (String term : text.trim().split("[\\s,]+")) {
            if (term.isEmpty()) continue;
            int equals = term.indexOf('=');
            if (equals <= 0) throw new IllegalArgumentException("'" + term + "' should look like IN1=0");
            String name = term.substring(0, equals);
            if (terms.put(name, SignalRange.parse(term.substring(equals + 1))) != null)
                throw new IllegalArgumentException(name + " is listed twice");
        }
        return terms;
    }

    private static String formatTerms(Map<String, SignalRange> terms) {
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, SignalRange> term : terms.entrySet()) {
            if (!text.isEmpty()) text.append(' ');
            text.append(term.getKey()).append('=').append(term.getValue());
        }
        return text.toString();
    }
}
