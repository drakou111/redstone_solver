package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/** One player's circuit description in world coordinates, built up by commands and the wand. */
public final class CircuitDraft {
    public static final List<String> DEFAULT_PALETTE = List.of("solid", "glass", "dust", "torch", "redstone_block",
            "repeater", "comparator", "target", "note_block", "hopper", "barrel", "chest", "observer");

    private static final int DEFAULT_SETTLE_TICKS = 4;
    private static final int DEFAULT_HORIZON_TICKS = 20;

    private Cell corner1;
    private Cell corner2;
    private final Map<String, Port> ports = new LinkedHashMap<>();
    private final List<TruthRow> rows = new ArrayList<>();
    private Integer latestTicks;

    public void setCorner1(Cell cell) {
        this.corner1 = cell;
    }

    public void setCorner2(Cell cell) {
        this.corner2 = cell;
    }

    public Optional<Box> getBox() {
        if (this.corner1 == null || this.corner2 == null) return Optional.empty();
        return Optional.of(Box.spanning(this.corner1, this.corner2));
    }

    /** The one corner set so far, while the box is incomplete. */
    public Optional<Cell> getAnyCorner() {
        return Optional.ofNullable(this.corner1 != null ? this.corner1 : this.corner2);
    }

    public Box requireBox() {
        return this.getBox().orElseThrow(() -> new IllegalStateException(
                "Select the box first: left-click one corner and right-click the other with the wand."));
    }

    /** Adds the port, or replaces the port of the same name. */
    public void putPort(Port port) {
        Box box = this.requireBox();
        if (!box.contains(port.position()))
            throw new IllegalArgumentException("%s at %s is outside the box".formatted(port.name(), port.position()));
        Port existing = this.ports.get(port.name());
        if (existing != null && existing.input() != port.input())
            throw new IllegalArgumentException(
                    port.name() + " is already the name of an " + (existing.input() ? "input" : "output"));
        for (Port other : this.ports.values()) {
            if (!other.name().equals(port.name()) && other.position().equals(port.position()))
                throw new IllegalArgumentException(other.name() + " is already at " + port.position());
        }
        this.ports.put(port.name(), port);
    }

    public boolean removePort(String name) {
        return this.ports.remove(name) != null;
    }

    public List<Port> getInputs() {
        return this.portsOfSide(true);
    }

    public List<Port> getOutputs() {
        return this.portsOfSide(false);
    }

    public void addRow(TruthRow row) {
        List<String> inputs = names(this.getInputs());
        List<String> outputs = names(this.getOutputs());
        for (String name : row.inputs().keySet()) {
            if (!inputs.contains(name)) throw new IllegalArgumentException("there is no input named " + name);
        }
        for (String name : row.outputs().keySet()) {
            if (!outputs.contains(name)) throw new IllegalArgumentException("there is no output named " + name);
        }
        this.rows.add(row);
    }

    public List<TruthRow> getRows() {
        return List.copyOf(this.rows);
    }

    public boolean removeRow(int index) {
        if (index < 0 || index >= this.rows.size()) return false;
        this.rows.remove(index);
        return true;
    }

    public void clearRows() {
        this.rows.clear();
    }

    public Optional<Integer> getLatestTicks() {
        return Optional.ofNullable(this.latestTicks);
    }

    /** Outputs must be correct this many game ticks after an input change; empty means any time. */
    public void setLatestTicks(Optional<Integer> ticks) {
        if (ticks.isPresent() && ticks.get() < 0) throw new IllegalArgumentException("timing must not be negative");
        this.latestTicks = ticks.orElse(null);
    }

    /** The spec in the solver's JSON format, with positions relative to the box's lowest corner. */
    public JsonObject toSpecJson() {
        Box box = this.requireBox();
        List<Port> inputs = this.getInputs();
        List<Port> outputs = this.getOutputs();
        if (inputs.isEmpty()) throw new IllegalStateException("Add an input first: /rs input IN1 comparator");
        if (outputs.isEmpty()) throw new IllegalStateException("Add an output first: /rs output OUT1 dust");
        if (this.rows.isEmpty())
            throw new IllegalStateException("Add a truth table row first: /rs row IN1=1..15 -> OUT1=0");
        for (Port port : this.ports.values()) {
            if (!box.contains(port.position()))
                throw new IllegalStateException(port.name() + " is outside the box; move it or the box");
        }

        JsonObject spec = new JsonObject();
        spec.add("size", box.size().toJson());
        spec.add("inputs", portsJson(inputs, box));
        spec.add("outputs", portsJson(outputs, box));
        JsonArray rows = new JsonArray();
        for (TruthRow row : this.rows) {
            JsonObject entry = new JsonObject();
            entry.add("inputs", rangesJson(row.rangesFor(names(inputs), true)));
            entry.add("outputs", rangesJson(row.rangesFor(names(outputs), false)));
            rows.add(entry);
        }
        spec.add("rows", rows);

        JsonObject timing = new JsonObject();
        timing.add("earliest", JsonNull.INSTANCE);
        if (this.latestTicks == null) timing.add("latest", JsonNull.INSTANCE);
        else timing.addProperty("latest", this.latestTicks);
        timing.addProperty("holdPrevious", false);
        timing.addProperty("settle", DEFAULT_SETTLE_TICKS);
        timing.addProperty("horizon", DEFAULT_HORIZON_TICKS);
        spec.add("timing", timing);

        JsonArray palette = new JsonArray();
        for (String block : DEFAULT_PALETTE) palette.add(block);
        spec.add("palette", palette);
        spec.addProperty("feedback", false);
        spec.addProperty("supportLayer", true);
        JsonArray tiled = new JsonArray();
        for (int axis = 0; axis < 3; axis++) tiled.add(false);
        spec.add("tiled", tiled);
        spec.add("fixed", new JsonArray());
        spec.add("forbidden", new JsonArray());
        spec.add("hints", new JsonArray());
        return spec;
    }

    private List<Port> portsOfSide(boolean input) {
        List<Port> side = new ArrayList<>();
        for (Port port : this.ports.values()) {
            if (port.input() == input) side.add(port);
        }
        return side;
    }

    private static List<String> names(List<Port> ports) {
        List<String> names = new ArrayList<>(ports.size());
        for (Port port : ports) names.add(port.name());
        return names;
    }

    private static JsonArray portsJson(List<Port> ports, Box box) {
        JsonArray array = new JsonArray();
        for (Port port : ports) {
            JsonObject entry = new JsonObject();
            entry.addProperty("kind", port.kind());
            entry.add("position", port.position().minus(box.min()).toJson());
            entry.addProperty("facing", port.facing().jsonName());
            entry.add("region", new JsonArray());
            array.add(entry);
        }
        return array;
    }

    private static JsonArray rangesJson(List<SignalRange> ranges) {
        JsonArray array = new JsonArray();
        for (SignalRange range : ranges) array.add(range.toJson());
        return array;
    }
}
