package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;

/** One player's circuit description in world coordinates, edited by keys, the panel and /rs. */
public final class CircuitDraft {
    public static final List<String> DEFAULT_PALETTE = List.of("solid", "glass", "dust", "torch", "redstone_block",
            "repeater", "comparator", "target", "note_block", "hopper", "barrel", "chest", "observer");

    private static final int DEFAULT_SETTLE_TICKS = 4;
    private static final int DEFAULT_HORIZON_TICKS = 20;
    public static final String PORT_NAME_PATTERN = "[A-Za-z0-9_]+";

    /** Reads the solver block (JSON without position) at a world cell; empty for air. */
    @FunctionalInterface
    public interface BlockSource {
        /** For drafts that never read the world (no kept cells, other blocks ignored). */
        BlockSource NONE = cell -> {
            throw new IllegalStateException("this draft cannot read blocks from the world");
        };

        Optional<JsonObject> blockAt(Cell cell);
    }

    /** What the solver does with blocks in the box that are neither ports nor kept cells. */
    public enum OtherBlocks {
        /** The solver starts from an empty box. */
        IGNORE,
        /** The solver starts from them as a sketch it may change. */
        SKETCH
    }

    private Cell corner1;
    private Cell corner2;
    private final Map<String, Port> ports = new LinkedHashMap<>();
    private final Set<Cell> keptCells = new LinkedHashSet<>();
    private final Set<Cell> emptyCells = new LinkedHashSet<>();
    private final List<TruthRow> rows = new ArrayList<>();
    private Integer latestTicks;
    private OtherBlocks otherBlocks = OtherBlocks.IGNORE;

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
                "Set the box first: press J on one corner block and K on the opposite one."));
    }

    /** Adds the port, or replaces the port of the same name. */
    public void putPort(Port port) {
        this.requireInsideBox(port.position(), port.name());
        Port existing = this.ports.get(port.name());
        if (existing != null && existing.input() != port.input())
            throw new IllegalArgumentException(
                    port.name() + " is already the name of an " + (existing.input() ? "input" : "output"));
        for (Port other : this.ports.values()) {
            if (!other.name().equals(port.name()) && other.position().equals(port.position()))
                throw new IllegalArgumentException(other.name() + " is already at " + port.position());
        }
        this.keptCells.remove(port.position());
        this.emptyCells.remove(port.position());
        this.ports.put(port.name(), port);
    }

    public boolean removePort(String name) {
        return this.ports.remove(name) != null;
    }

    /** Renames a port and every truth table entry that names it. */
    public void renamePort(String oldName, String newName) {
        Port port = this.ports.get(oldName);
        if (port == null) throw new IllegalArgumentException("there is no port named " + oldName);
        if (oldName.equals(newName)) return;
        if (!newName.matches(PORT_NAME_PATTERN))
            throw new IllegalArgumentException("port names use letters, digits and _ only");
        if (this.ports.containsKey(newName)) throw new IllegalArgumentException(newName + " is already used");

        Map<String, Port> renamed = new LinkedHashMap<>();
        for (Map.Entry<String, Port> entry : this.ports.entrySet()) {
            if (entry.getKey().equals(oldName))
                renamed.put(newName, new Port(newName, port.input(), port.kind(), port.position(), port.facing()));
            else renamed.put(entry.getKey(), entry.getValue());
        }
        this.ports.clear();
        this.ports.putAll(renamed);
        for (int i = 0; i < this.rows.size(); i++) this.rows.set(i, this.rows.get(i).withRenamedPort(oldName, newName));
    }

    public Optional<Port> getPort(String name) {
        return Optional.ofNullable(this.ports.get(name));
    }

    public Optional<Port> portAt(Cell cell) {
        for (Port port : this.ports.values()) {
            if (port.position().equals(cell)) return Optional.of(port);
        }
        return Optional.empty();
    }

    /** The first of PREFIX1, PREFIX2, ... that no port uses. */
    public String nextFreeName(String prefix) {
        for (int number = 1; ; number++) {
            String name = prefix + number;
            if (!this.ports.containsKey(name)) return name;
        }
    }

    public List<Port> getInputs() {
        return this.portsOfSide(true);
    }

    public List<Port> getOutputs() {
        return this.portsOfSide(false);
    }

    /** The solver must keep the block that stands in this cell when solving starts. */
    public void markKept(Cell cell) {
        this.requireInsideBox(cell, "the kept cell");
        this.requireNoPort(cell);
        this.emptyCells.remove(cell);
        this.keptCells.add(cell);
    }

    /** The solver must leave this cell empty. */
    public void markEmpty(Cell cell) {
        this.requireInsideBox(cell, "the empty cell");
        this.requireNoPort(cell);
        this.keptCells.remove(cell);
        this.emptyCells.add(cell);
    }

    /** Removes any port, keep or empty mark from the cell; false if it had none. */
    public boolean clearCell(Cell cell) {
        boolean changed = this.keptCells.remove(cell) | this.emptyCells.remove(cell);
        Optional<Port> port = this.portAt(cell);
        if (port.isPresent()) changed |= this.removePort(port.get().name());
        return changed;
    }

    public Set<Cell> getKeptCells() {
        return Set.copyOf(this.keptCells);
    }

    public Set<Cell> getEmptyCells() {
        return Set.copyOf(this.emptyCells);
    }

    public OtherBlocks getOtherBlocks() {
        return this.otherBlocks;
    }

    public void setOtherBlocks(OtherBlocks otherBlocks) {
        this.otherBlocks = otherBlocks;
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

    public void setRow(int index, TruthRow row) {
        if (index < 0 || index >= this.rows.size()) throw new IllegalArgumentException("there is no row " + (index + 1));
        this.rows.set(index, row);
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

    /**
     * Makes the box and ports match a placed result whose spec differs from the draft (Compose
     * places the ports itself, Shrink makes the box smaller). Ports keep their names and are matched
     * by order. Kept and empty marks are dropped since the cells moved.
     */
    public void adoptSpec(JsonObject spec, Cell origin) {
        List<Port> inputs = this.getInputs();
        List<Port> outputs = this.getOutputs();
        JsonArray specInputs = spec.getAsJsonArray("inputs");
        JsonArray specOutputs = spec.getAsJsonArray("outputs");
        if (specInputs.size() != inputs.size() || specOutputs.size() != outputs.size())
            throw new IllegalArgumentException("the result has a different number of ports than the draft");

        Cell size = Cell.fromJson(spec.get("size"));
        Map<String, Port> adopted = new LinkedHashMap<>();
        for (int i = 0; i < inputs.size(); i++) adopted.put(inputs.get(i).name(), adoptPort(inputs.get(i), specInputs, i, origin));
        for (int i = 0; i < outputs.size(); i++)
            adopted.put(outputs.get(i).name(), adoptPort(outputs.get(i), specOutputs, i, origin));
        this.corner1 = origin;
        this.corner2 = origin.plus(size).minus(new Cell(1, 1, 1));
        this.ports.clear();
        this.ports.putAll(adopted);
        this.keptCells.clear();
        this.emptyCells.clear();
    }

    private static Port adoptPort(Port port, JsonArray specPorts, int index, Cell origin) {
        JsonObject entry = specPorts.get(index).getAsJsonObject();
        String facingName = entry.get("facing").getAsString();
        Dir facing = Dir.parse(facingName).orElseThrow(() -> new IllegalArgumentException("unknown facing " + facingName));
        return new Port(port.name(), port.input(), entry.get("kind").getAsString(),
                origin.plus(Cell.fromJson(entry.get("position"))), facing);
    }

    /** The spec for a draft without kept cells or sketch; see {@link #toSpecJson(BlockSource)}. */
    public JsonObject toSpecJson() {
        return this.toSpecJson(BlockSource.NONE);
    }

    /**
     * The spec in the solver's JSON format, with positions relative to the box's lowest corner.
     * Kept cells and (in sketch mode) the other blocks of the box are read from {@code blocks}.
     */
    public JsonObject toSpecJson(BlockSource blocks) {
        Box box = this.requireBox();
        List<Port> inputs = this.getInputs();
        List<Port> outputs = this.getOutputs();
        if (inputs.isEmpty()) throw new IllegalStateException("Add an input first (look at a cell and press P).");
        if (outputs.isEmpty()) throw new IllegalStateException("Add an output first (look at a cell and press P).");
        if (this.rows.isEmpty()) throw new IllegalStateException("Add a truth table row first.");
        for (Port port : this.ports.values()) {
            if (!box.contains(port.position()))
                throw new IllegalStateException(port.name() + " is outside the box; move it or the box");
        }
        for (Cell cell : this.keptCells) {
            if (!box.contains(cell)) throw new IllegalStateException("A kept cell at " + cell + " is outside the box");
        }
        for (Cell cell : this.emptyCells) {
            if (!box.contains(cell)) throw new IllegalStateException("An empty cell at " + cell + " is outside the box");
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

        JsonArray fixed = new JsonArray();
        for (Cell cell : this.keptCells) {
            JsonObject block = blocks.blockAt(cell).orElseThrow(() -> new IllegalStateException(
                    "The kept cell at " + cell + " is empty; place a block there or clear the mark"));
            fixed.add(positioned(block, cell, box));
        }
        spec.add("fixed", fixed);
        JsonArray forbidden = new JsonArray();
        for (Cell cell : this.emptyCells) forbidden.add(cell.minus(box.min()).toJson());
        spec.add("forbidden", forbidden);
        spec.add("hints", this.otherBlocks == OtherBlocks.SKETCH ? this.sketchJson(box, blocks) : new JsonArray());
        return spec;
    }

    private JsonArray sketchJson(Box box, BlockSource blocks) {
        JsonArray hints = new JsonArray();
        for (int x = box.min().x(); x <= box.max().x(); x++) {
            for (int y = box.min().y(); y <= box.max().y(); y++) {
                for (int z = box.min().z(); z <= box.max().z(); z++) {
                    Cell cell = new Cell(x, y, z);
                    if (this.keptCells.contains(cell) || this.emptyCells.contains(cell) || this.portAt(cell).isPresent())
                        continue;
                    blocks.blockAt(cell).ifPresent(block -> hints.add(positioned(block, cell, box)));
                }
            }
        }
        return hints;
    }

    private void requireInsideBox(Cell cell, String what) {
        Box box = this.requireBox();
        if (!box.contains(cell)) throw new IllegalArgumentException("%s at %s is outside the box".formatted(what, cell));
    }

    private void requireNoPort(Cell cell) {
        Optional<Port> port = this.portAt(cell);
        if (port.isPresent())
            throw new IllegalArgumentException(port.get().name() + " is in that cell; clear it first");
    }

    private List<Port> portsOfSide(boolean input) {
        List<Port> side = new ArrayList<>();
        for (Port port : this.ports.values()) {
            if (port.input() == input) side.add(port);
        }
        return side;
    }

    private static JsonObject positioned(JsonObject block, Cell cell, Box box) {
        JsonObject entry = block.deepCopy();
        entry.add("position", cell.minus(box.min()).toJson());
        return entry;
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
