package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Function;

/** One player's circuit description in world coordinates, edited by keys, the panel and /rs. */
public final class CircuitDraft {
    public static final List<String> DEFAULT_PALETTE = List.of("solid", "glass", "dust", "torch", "redstone_block",
            "repeater", "comparator", "target", "note_block", "hopper", "barrel", "chest");

    private static final int DEFAULT_SETTLE_TICKS = 4;
    private static final int DEFAULT_HORIZON_TICKS = 20;
    private static final int SAVE_VERSION = 1;
    /** Formulas are expanded over every input combination; more would make the solver crawl. */
    public static final int MAX_FORMULA_COMBINATIONS = 4096;
    private static final int ON_SIGNAL = 15;
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
    private Integer earliestTicks;
    private Integer latestTicks;
    private OtherBlocks otherBlocks = OtherBlocks.IGNORE;
    private boolean formulaMode;
    private final Map<String, String> formulas = new LinkedHashMap<>();
    private final Set<String> analogInputs = new LinkedHashSet<>();
    private final Set<String> palette = new LinkedHashSet<>(DEFAULT_PALETTE);

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
        this.formulas.remove(name);
        this.analogInputs.remove(name);
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
        Map<String, String> renamedFormulas = new LinkedHashMap<>();
        for (Map.Entry<String, String> entry : this.formulas.entrySet()) {
            String key = entry.getKey().equals(oldName) ? newName : entry.getKey();
            renamedFormulas.put(key, Formula.renameVariable(entry.getValue(), oldName, newName));
        }
        this.formulas.clear();
        this.formulas.putAll(renamedFormulas);
        if (this.analogInputs.remove(oldName)) this.analogInputs.add(newName);
    }

    /**
     * Updates each port's kind and facing from the block now in its cell (the player may have
     * turned or swapped it). Returns the names of ports whose cell no longer holds a block that
     * can be that port; those keep their last kind and facing.
     */
    public List<String> refreshPorts(Function<Cell, Optional<PortBlock>> blocks) {
        List<String> stale = new ArrayList<>();
        for (Port port : List.copyOf(this.ports.values())) {
            Optional<PortBlock> block = blocks.apply(port.position());
            boolean fits = block.isPresent() && (port.input() ? block.get().input() : block.get().output()).isPresent();
            if (!fits) {
                stale.add(port.name());
                continue;
            }
            this.ports.put(port.name(), block.get().toPort(port.name(), port.input(), port.position()));
        }
        return stale;
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

    /** In formula mode the truth table comes from one formula per output instead of rows. */
    public boolean isFormulaMode() {
        return this.formulaMode;
    }

    public void setFormulaMode(boolean formulaMode) {
        this.formulaMode = formulaMode;
    }

    /** The formula text typed for an output (may be invalid; see {@link #formulaRows()}). */
    public String getFormula(String output) {
        return this.formulas.getOrDefault(output, "");
    }

    public void setFormula(String output, String text) {
        if (text.isBlank()) this.formulas.remove(output);
        else this.formulas.put(output, text);
    }

    /** True when formulas try this input at every value 0..15; otherwise only off (0) and on (15). */
    public boolean isAnalogInput(String input) {
        return this.analogInputs.contains(input);
    }

    public void setAnalogInput(String input, boolean analog) {
        if (analog) this.analogInputs.add(input);
        else this.analogInputs.remove(input);
    }

    /** How many rows the formulas expand into with the current input value settings. */
    public long formulaCombinations() {
        long combinations = 1;
        for (Port input : this.getInputs()) combinations *= this.isAnalogInput(input.name()) ? ON_SIGNAL + 1 : 2;
        return combinations;
    }

    /**
     * Expands the formulas into one row per input combination (on/off inputs at 0 and 15, the
     * others at 0..15). Throws IllegalArgumentException naming the output and the problem.
     */
    public List<TruthRow> formulaRows() {
        List<Port> inputs = this.getInputs();
        List<Port> outputs = this.getOutputs();
        long combinations = this.formulaCombinations();
        if (combinations > MAX_FORMULA_COMBINATIONS)
            throw new IllegalArgumentException("%d input combinations is too many (at most %d); set fewer inputs to 0-15"
                    .formatted(combinations, MAX_FORMULA_COMBINATIONS));
        List<String> inputNames = names(inputs);
        Map<String, Formula> parsed = new LinkedHashMap<>();
        for (Port output : outputs) {
            String text = this.getFormula(output.name());
            if (text.isBlank()) throw new IllegalArgumentException(output.name() + " has no formula");
            Formula formula;
            try {
                formula = Formula.parse(text);
            } catch (IllegalArgumentException e) {
                throw new IllegalArgumentException(output.name() + ": " + e.getMessage(), e);
            }
            for (String name : formula.variables()) {
                if (!inputNames.contains(name))
                    throw new IllegalArgumentException(output.name() + ": there is no input named " + name);
            }
            parsed.put(output.name(), formula);
        }

        List<TruthRow> rows = new ArrayList<>();
        int[] values = new int[inputs.size()];
        for (long index = 0; index < combinations; index++) {
            long rest = index;
            Map<String, Integer> assignment = new LinkedHashMap<>();
            Map<String, SignalRange> rowInputs = new LinkedHashMap<>();
            for (int i = 0; i < inputs.size(); i++) {
                String name = inputs.get(i).name();
                int choices = this.isAnalogInput(name) ? ON_SIGNAL + 1 : 2;
                int pick = (int) (rest % choices);
                rest /= choices;
                values[i] = this.isAnalogInput(name) ? pick : pick * ON_SIGNAL;
                assignment.put(name, values[i]);
                rowInputs.put(name, new SignalRange(values[i], values[i]));
            }
            Map<String, SignalRange> rowOutputs = new LinkedHashMap<>();
            for (Map.Entry<String, Formula> entry : parsed.entrySet()) {
                try {
                    rowOutputs.put(entry.getKey(), entry.getValue().outputFor(assignment));
                } catch (IllegalArgumentException e) {
                    throw new IllegalArgumentException(entry.getKey() + " " + e.getMessage(), e);
                }
            }
            rows.add(new TruthRow(rowInputs, rowOutputs));
        }
        return rows;
    }

    /** The blocks the solver may place, in the order of {@link #DEFAULT_PALETTE}. */
    public List<String> getPalette() {
        List<String> blocks = new ArrayList<>();
        for (String block : DEFAULT_PALETTE) {
            if (this.palette.contains(block)) blocks.add(block);
        }
        return blocks;
    }

    public boolean allows(String block) {
        return this.palette.contains(block);
    }

    /** Lets the solver use a block or not; it always keeps at least one. */
    public void setAllowed(String block, boolean allowed) {
        if (!DEFAULT_PALETTE.contains(block)) throw new IllegalArgumentException("unknown block " + block);
        if (allowed) {
            this.palette.add(block);
            return;
        }
        if (this.palette.size() == 1 && this.palette.contains(block))
            throw new IllegalArgumentException("the solver needs at least one block to build with");
        this.palette.remove(block);
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

    public Optional<Integer> getEarliestTicks() {
        return Optional.ofNullable(this.earliestTicks);
    }

    /**
     * Outputs keep their old value until this many game ticks after an input change (a delay);
     * empty means they may change at once.
     */
    public void setEarliestTicks(Optional<Integer> ticks) {
        if (ticks.isPresent() && ticks.get() < 0) throw new IllegalArgumentException("timing must not be negative");
        this.earliestTicks = ticks.orElse(null);
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

    /**
     * The circuit built in the box as the solver's layout (positions relative to the box): port
     * cells become the solver's input and output blocks, every other cell is read from the world.
     */
    public JsonObject buildLayoutJson(BlockSource blocks) {
        Box box = this.requireBox();
        List<Port> inputs = this.getInputs();
        List<Port> outputs = this.getOutputs();
        JsonArray entries = new JsonArray();
        for (int x = box.min().x(); x <= box.max().x(); x++) {
            for (int y = box.min().y(); y <= box.max().y(); y++) {
                for (int z = box.min().z(); z <= box.max().z(); z++) {
                    Cell cell = new Cell(x, y, z);
                    Optional<Port> port = this.portAt(cell);
                    Optional<JsonObject> block = port.isPresent()
                            ? Optional.of(portBlockJson(port.get(), port.get().input()
                                    ? inputs.indexOf(port.get()) : outputs.indexOf(port.get())))
                            : blocks.blockAt(cell);
                    block.ifPresent(found -> entries.add(positioned(found, cell, box)));
                }
            }
        }
        JsonObject layout = new JsonObject();
        layout.add("size", box.size().toJson());
        layout.add("blocks", entries);
        return layout;
    }

    private static JsonObject portBlockJson(Port port, int channel) {
        JsonObject json = new JsonObject();
        json.addProperty("type", port.input() ? "input" : "output");
        json.addProperty("kind", port.kind());
        json.addProperty("channel", channel);
        json.addProperty("facing", port.facing().jsonName());
        return json;
    }

    /** The whole draft in world coordinates, for saving; read back with {@link #fromJson}. */
    public JsonObject toJson() {
        JsonObject json = new JsonObject();
        json.addProperty("version", SAVE_VERSION);
        if (this.corner1 != null) json.add("corner1", this.corner1.toJson());
        if (this.corner2 != null) json.add("corner2", this.corner2.toJson());
        JsonArray ports = new JsonArray();
        for (Port port : this.ports.values()) {
            JsonObject entry = new JsonObject();
            entry.addProperty("name", port.name());
            entry.addProperty("input", port.input());
            entry.addProperty("kind", port.kind());
            entry.add("position", port.position().toJson());
            entry.addProperty("facing", port.facing().jsonName());
            ports.add(entry);
        }
        json.add("ports", ports);
        json.add("kept", cellsJson(this.keptCells));
        json.add("empty", cellsJson(this.emptyCells));
        JsonArray rows = new JsonArray();
        for (TruthRow row : this.rows) rows.add(row.toString());
        json.add("rows", rows);
        if (this.latestTicks != null) json.addProperty("latestTicks", this.latestTicks);
        if (this.earliestTicks != null) json.addProperty("earliestTicks", this.earliestTicks);
        json.addProperty("otherBlocks", this.otherBlocks.name());
        json.addProperty("formulaMode", this.formulaMode);
        JsonObject formulas = new JsonObject();
        for (Map.Entry<String, String> entry : this.formulas.entrySet()) formulas.addProperty(entry.getKey(), entry.getValue());
        json.add("formulas", formulas);
        JsonArray analog = new JsonArray();
        for (String name : this.analogInputs) analog.add(name);
        json.add("analogInputs", analog);
        JsonArray allowed = new JsonArray();
        for (String block : this.getPalette()) allowed.add(block);
        json.add("palette", allowed);
        return json;
    }

    /** Reads a saved draft; throws IllegalArgumentException with the reason when the file is not a valid draft. */
    public static CircuitDraft fromJson(JsonObject json) {
        try {
            int version = json.get("version").getAsInt();
            if (version != SAVE_VERSION) throw new IllegalArgumentException("unsupported draft version " + version);
            CircuitDraft draft = new CircuitDraft();
            if (json.has("corner1")) draft.corner1 = Cell.fromJson(json.get("corner1"));
            if (json.has("corner2")) draft.corner2 = Cell.fromJson(json.get("corner2"));
            for (JsonElement element : json.getAsJsonArray("ports")) {
                JsonObject entry = element.getAsJsonObject();
                String facing = entry.get("facing").getAsString();
                draft.putPort(new Port(entry.get("name").getAsString(), entry.get("input").getAsBoolean(),
                        entry.get("kind").getAsString(), Cell.fromJson(entry.get("position")),
                        Dir.parse(facing).orElseThrow(() -> new IllegalArgumentException("unknown facing " + facing))));
            }
            for (JsonElement cell : json.getAsJsonArray("kept")) draft.markKept(Cell.fromJson(cell));
            for (JsonElement cell : json.getAsJsonArray("empty")) draft.markEmpty(Cell.fromJson(cell));
            for (JsonElement row : json.getAsJsonArray("rows")) draft.addRow(TruthRow.parse(row.getAsString()));
            if (json.has("latestTicks")) draft.setLatestTicks(Optional.of(json.get("latestTicks").getAsInt()));
            if (json.has("earliestTicks")) draft.setEarliestTicks(Optional.of(json.get("earliestTicks").getAsInt()));
            draft.setOtherBlocks(OtherBlocks.valueOf(json.get("otherBlocks").getAsString()));
            // Formulas came later; older saves have none.
            if (json.has("formulaMode")) draft.setFormulaMode(json.get("formulaMode").getAsBoolean());
            if (json.has("formulas")) {
                for (Map.Entry<String, JsonElement> entry : json.getAsJsonObject("formulas").entrySet())
                    draft.setFormula(entry.getKey(), entry.getValue().getAsString());
            }
            if (json.has("analogInputs")) {
                for (JsonElement name : json.getAsJsonArray("analogInputs")) draft.setAnalogInput(name.getAsString(), true);
            }
            if (json.has("palette")) {
                Set<String> allowed = new LinkedHashSet<>();
                for (JsonElement block : json.getAsJsonArray("palette")) allowed.add(block.getAsString());
                for (String block : DEFAULT_PALETTE) {
                    if (!allowed.contains(block)) draft.setAllowed(block, false);
                }
            }
            return draft;
        } catch (NullPointerException | IllegalStateException | ClassCastException | UnsupportedOperationException e) {
            throw new IllegalArgumentException("not a valid draft: " + e.getMessage(), e);
        }
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
        List<TruthRow> tableRows = this.formulaMode ? this.formulaRows() : this.rows;
        if (tableRows.isEmpty()) throw new IllegalStateException("Add a truth table row first.");
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
        for (TruthRow row : tableRows) {
            JsonObject entry = new JsonObject();
            entry.add("inputs", rangesJson(row.rangesFor(names(inputs), true)));
            entry.add("outputs", rangesJson(row.rangesFor(names(outputs), false)));
            rows.add(entry);
        }
        spec.add("rows", rows);

        if (this.earliestTicks != null && this.latestTicks == null)
            throw new IllegalStateException("A delay (not before) also needs a deadline (by): set both timing numbers.");
        if (this.earliestTicks != null && this.earliestTicks > this.latestTicks)
            throw new IllegalStateException("Timing: 'not before' must not be after 'by'.");
        JsonObject timing = new JsonObject();
        if (this.earliestTicks == null) timing.add("earliest", JsonNull.INSTANCE);
        else timing.addProperty("earliest", this.earliestTicks);
        if (this.latestTicks == null) timing.add("latest", JsonNull.INSTANCE);
        else timing.addProperty("latest", this.latestTicks);
        // Before the earliest tick the outputs keep their old value: that is what makes it a delay.
        timing.addProperty("holdPrevious", this.earliestTicks != null);
        timing.addProperty("settle", DEFAULT_SETTLE_TICKS);
        timing.addProperty("horizon", DEFAULT_HORIZON_TICKS);
        spec.add("timing", timing);

        JsonArray palette = new JsonArray();
        for (String block : this.getPalette()) palette.add(block);
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

    private static JsonArray cellsJson(Set<Cell> cells) {
        JsonArray array = new JsonArray();
        for (Cell cell : cells) array.add(cell.toJson());
        return array;
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
