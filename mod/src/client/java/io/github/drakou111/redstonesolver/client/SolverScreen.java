package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.SignalRange;
import io.github.drakou111.redstonesolver.core.TruthRow;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.function.Consumer;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;
import net.minecraft.util.FormattedCharSequence;

/**
 * The Solver panel (R): box, ports, truth table, solve settings and the last result. Left column:
 * box, ports, result. Right column: truth table, solve settings.
 */
public final class SolverScreen extends Screen {
    private static final int MARGIN = 10;
    private static final int PAD = 6;
    private static final int GAP = 4;
    private static final int ROW = 18;
    private static final int LINE = 11;
    private static final int NUMBER_WIDTH = 34;
    private static final int NAME_WIDTH = 48;
    private static final int CELL_WIDTH = 38;
    private static final int SMALL_BUTTON = 16;
    private static final int MAX_LOG_LINES = 12;
    private static final int RESULT_CARD_HEIGHT = 112;
    private static final int SOLVE_SECTION_HEIGHT = 5 * (ROW + GAP) + 2 * LINE;

    private static final int PANEL_COLOR = 0xE0101010;
    private static final int CARD_COLOR = 0x60000000;
    private static final int BORDER_COLOR = 0xFF8B1A1A;
    private static final int HEADING_COLOR = 0xFFFF5555;
    private static final int TEXT_COLOR = 0xFFFFFFFF;
    private static final int HINT_COLOR = 0xFFAAAAAA;
    private static final int GOOD_COLOR = 0xFF55FF55;
    private static final int BAD_COLOR = 0xFFFF6060;
    private static final int INPUT_COLOR = 0xFF55DD55;
    private static final int OUTPUT_COLOR = 0xFFFF7777;

    private record Label(String text, int x, int y, int color) {
    }

    private final ClientState state;
    private final ServerLink server;
    private final List<Label> labels = new ArrayList<>();
    private int seenVersion;
    private int tableScroll;
    private int visibleRows;
    private boolean advancedOpen;
    private boolean logOpen;
    private String message = "";
    private int resultTop;
    private int resultLeft;
    private int resultWidth;
    private int progressX;
    private int progressY;

    public SolverScreen(ClientState state, ServerLink server) {
        super(Component.literal("Redstone Solver"));
        this.state = state;
        this.server = server;
    }

    @Override
    protected void init() {
        this.labels.clear();
        this.seenVersion = this.state.getVersion();
        int left = MARGIN + PAD;
        int top = MARGIN + PAD;
        int inner = this.width - 2 * (MARGIN + PAD);
        int leftWidth = Math.max(200, inner * 2 / 5);
        int rightLeft = left + leftWidth + 2 * PAD;
        int rightWidth = left + inner - rightLeft;
        int bottom = this.height - MARGIN - PAD;

        this.initBox(left, top);
        this.initPorts(left, top + 4 * LINE + 2 * ROW + 2 * GAP, leftWidth, bottom - RESULT_CARD_HEIGHT - PAD);
        this.initResult(left, bottom - RESULT_CARD_HEIGHT, leftWidth);
        int solveTop = bottom - SOLVE_SECTION_HEIGHT - (this.advancedOpen ? ROW + GAP : 0);
        this.initTable(rightLeft, top, rightWidth, solveTop - PAD);
        this.initSolve(rightLeft, solveTop, rightWidth);
    }

    @Override
    public void tick() {
        if (this.state.getVersion() != this.seenVersion) this.rebuildWidgets();
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        graphics.fill(MARGIN, MARGIN, this.width - MARGIN, this.height - MARGIN, PANEL_COLOR);
        graphics.outline(MARGIN, MARGIN, this.width - 2 * MARGIN, this.height - 2 * MARGIN, BORDER_COLOR);
        graphics.fill(this.resultLeft - 2, this.resultTop - 2, this.resultLeft + this.resultWidth + 2,
                this.resultTop + RESULT_CARD_HEIGHT, CARD_COLOR);
        for (Label label : this.labels) graphics.text(this.font, label.text(), label.x(), label.y(), label.color());
        this.extractResultText(graphics);
        String status = this.state.isSolving() ? this.state.getProgress() : this.message;
        if (!status.isEmpty()) {
            int color = this.state.isSolving() ? TEXT_COLOR : status.startsWith("!") ? BAD_COLOR : HINT_COLOR;
            graphics.text(this.font, status.startsWith("!") ? status.substring(1) : status, this.progressX, this.progressY, color);
        }
        super.extractRenderState(graphics, mouseX, mouseY, partialTick);
    }

    @Override
    public boolean mouseScrolled(double mouseX, double mouseY, double scrollX, double scrollY) {
        int rows = this.state.getDraft().getRows().size();
        int maxScroll = Math.max(0, rows - this.visibleRows);
        int next = Math.clamp(this.tableScroll - (int) Math.signum(scrollY), 0, maxScroll);
        if (next != this.tableScroll) {
            this.tableScroll = next;
            this.rebuildWidgets();
            return true;
        }
        return super.mouseScrolled(mouseX, mouseY, scrollX, scrollY);
    }

    @Override
    public boolean isPauseScreen() {
        return false;
    }

    // Box: lowest corner and size as numbers, kept in sync with the J/K corners.
    private void initBox(int x, int y) {
        CircuitDraft draft = this.state.getDraft();
        this.heading("Box", x, y);
        Optional<Box> box = draft.getBox();
        int[] values = box.map(b -> new int[] {b.min().x(), b.min().y(), b.min().z(), b.size().x(), b.size().y(), b.size().z()})
                .orElse(new int[6]);
        List<EditBox> fields = new ArrayList<>();
        String[] tips = {"Lowest x", "Lowest y", "Lowest z", "Size along x (east)", "Size along y (up)", "Size along z (south)"};
        for (int i = 0; i < 6; i++) {
            int row = i / 3;
            int column = i % 3;
            int fieldX = x + 30 + column * (NUMBER_WIDTH + GAP);
            int fieldY = y + LINE + row * (ROW + GAP);
            EditBox field = this.field(fieldX, fieldY, NUMBER_WIDTH, box.isPresent() ? String.valueOf(values[i]) : "", tips[i]);
            fields.add(field);
        }
        this.label("From", x, y + LINE + 5, TEXT_COLOR);
        this.label("Size", x, y + LINE + ROW + GAP + 5, TEXT_COLOR);
        Consumer<String> apply = text -> {
            int[] parsed = new int[6];
            for (int i = 0; i < 6; i++) {
                try {
                    parsed[i] = Integer.parseInt(fields.get(i).getValue().trim());
                } catch (NumberFormatException e) {
                    markAll(fields, false);
                    return;
                }
            }
            if (parsed[3] < 1 || parsed[4] < 1 || parsed[5] < 1) {
                markAll(fields, false);
                return;
            }
            Cell min = new Cell(parsed[0], parsed[1], parsed[2]);
            draft.setCorner1(min);
            draft.setCorner2(min.plus(new Cell(parsed[3] - 1, parsed[4] - 1, parsed[5] - 1)));
            markAll(fields, true);
        };
        for (EditBox field : fields) field.setResponder(apply);
        this.label("Or press J and K on two corner blocks. Bottom layer: supports.", x, y + LINE + 2 * (ROW + GAP) + 2,
                HINT_COLOR);
    }

    private void initPorts(int x, int y, int width, int bottom) {
        CircuitDraft draft = this.state.getDraft();
        this.heading("Ports", x, y);
        List<Port> ports = new ArrayList<>(draft.getInputs());
        ports.addAll(draft.getOutputs());
        if (ports.isEmpty()) {
            this.label("None yet: place a repeater, comparator, dust or barrel, look at it, press P.", x, y + LINE + 2,
                HINT_COLOR);
            return;
        }
        int rowY = y + LINE;
        int shown = 0;
        for (Port port : ports) {
            if (rowY + ROW > bottom) {
                this.label("... and %d more (see /rs show)".formatted(ports.size() - shown), x, rowY + 2, HINT_COLOR);
                break;
            }
            this.portRow(port, x, rowY, width);
            rowY += ROW + 2;
            shown++;
        }
    }

    private void portRow(Port port, int x, int y, int width) {
        CircuitDraft draft = this.state.getDraft();
        Cell cell = port.position();
        EditBox name = this.field(x, y, NAME_WIDTH, port.name(), "Rename (letters, digits, _); the truth table follows");
        name.setTextColor(port.input() ? INPUT_COLOR : OUTPUT_COLOR);
        name.setResponder(text -> draft.portAt(cell).ifPresent(current -> {
            try {
                draft.renamePort(current.name(), text.trim());
                name.setTextColor(current.input() ? INPUT_COLOR : OUTPUT_COLOR);
            } catch (IllegalArgumentException e) {
                name.setTextColor(BAD_COLOR);
            }
        }));
        String kind = port.hasFacing() ? port.kind() + " " + port.facing().jsonName() : port.kind();
        this.label(kind + "  at " + cell, x + NAME_WIDTH + GAP, y + 5, HINT_COLOR);
        this.button(x + width - SMALL_BUTTON, y, SMALL_BUTTON, "x", "Remove " + port.name(), () -> {
            draft.removePort(port.name());
            this.rebuildWidgets();
        });
    }

    private void initTable(int x, int y, int width, int bottom) {
        CircuitDraft draft = this.state.getDraft();
        this.heading("Truth table", x, y);
        List<Port> inputs = draft.getInputs();
        List<Port> outputs = draft.getOutputs();
        if (inputs.isEmpty() || outputs.isEmpty()) {
            this.label("Add at least one input and one output first (P on a port block).", x, y + LINE + 2, HINT_COLOR);
            this.visibleRows = 0;
            return;
        }
        this.label("Ranges: 0, 15, 1..15, or * for anything. Scroll for more rows.", x, y + LINE, HINT_COLOR);
        int headerY = y + 2 * LINE + 2;
        int columnX = x;
        for (Port port : inputs) {
            this.label(port.name(), columnX + 2, headerY, INPUT_COLOR);
            columnX += CELL_WIDTH + 2;
        }
        this.label("->", columnX + 2, headerY, HINT_COLOR);
        columnX += 16;
        for (Port port : outputs) {
            this.label(port.name(), columnX + 2, headerY, OUTPUT_COLOR);
            columnX += CELL_WIDTH + 2;
        }

        List<TruthRow> rows = draft.getRows();
        int rowsTop = headerY + LINE;
        this.visibleRows = Math.max(1, (bottom - rowsTop - ROW - GAP) / (ROW + 2));
        this.tableScroll = Math.clamp(this.tableScroll, 0, Math.max(0, rows.size() - this.visibleRows));
        int rowY = rowsTop;
        for (int i = this.tableScroll; i < rows.size() && i < this.tableScroll + this.visibleRows; i++) {
            this.tableRow(i, rows.get(i), inputs, outputs, x, rowY);
            rowY += ROW + 2;
        }
        if (rows.isEmpty()) this.label("No rows yet.", x, rowY + 5, HINT_COLOR);
        int addY = Math.min(rowY, bottom - ROW) + (rows.isEmpty() ? LINE + 4 : 0);
        this.button(x, addY, 70, "+ Row", "Add a row (inputs 0, outputs 0); edit its ranges", () -> {
            Map<String, SignalRange> in = new LinkedHashMap<>();
            Map<String, SignalRange> out = new LinkedHashMap<>();
            for (Port port : inputs) in.put(port.name(), new SignalRange(0, 0));
            for (Port port : outputs) out.put(port.name(), new SignalRange(0, 0));
            draft.addRow(new TruthRow(in, out));
            this.tableScroll = Math.max(0, draft.getRows().size() - this.visibleRows);
            this.rebuildWidgets();
        });
        if (rows.size() > this.visibleRows) {
            this.label("rows %d-%d of %d".formatted(this.tableScroll + 1,
                    Math.min(rows.size(), this.tableScroll + this.visibleRows), rows.size()), x + 76, addY + 5, HINT_COLOR);
        }
    }

    private void tableRow(int index, TruthRow row, List<Port> inputs, List<Port> outputs, int x, int y) {
        int columnX = x;
        for (Port port : inputs) {
            this.rangeCell(index, port.name(), true, row.inputs().getOrDefault(port.name(), SignalRange.ANY), columnX, y);
            columnX += CELL_WIDTH + 2;
        }
        columnX += 16;
        for (Port port : outputs) {
            this.rangeCell(index, port.name(), false, row.outputs().getOrDefault(port.name(), SignalRange.ANY), columnX, y);
            columnX += CELL_WIDTH + 2;
        }
        this.button(columnX + GAP, y, SMALL_BUTTON, "x", "Remove row " + (index + 1), () -> {
            this.state.getDraft().removeRow(index);
            this.rebuildWidgets();
        });
    }

    private void rangeCell(int rowIndex, String portName, boolean input, SignalRange value, int x, int y) {
        EditBox cell = this.field(x, y, CELL_WIDTH, value.toString(), (input ? "Input " : "Output ") + portName
                + ": 0, 15, 1..15 or *");
        cell.setResponder(text -> {
            CircuitDraft draft = this.state.getDraft();
            try {
                SignalRange range = SignalRange.parse(text);
                draft.setRow(rowIndex, draft.getRows().get(rowIndex).withRange(portName, input, range));
                cell.setTextColor(TEXT_COLOR);
            } catch (IllegalArgumentException e) {
                cell.setTextColor(BAD_COLOR);
            }
        });
    }

    private void initSolve(int x, int y, int width) {
        CircuitDraft draft = this.state.getDraft();
        this.heading("Solve", x, y);
        int rowY = y + LINE;
        int fieldX = x + 70;

        this.label("Timing", x, rowY + 5, TEXT_COLOR);
        EditBox timing = this.field(fieldX, rowY, 40, draft.getLatestTicks().map(String::valueOf).orElse("any"),
                "Outputs must be right this many game ticks after an input changes (20 per second), or any");
        timing.setResponder(text -> {
            String trimmed = text.trim();
            try {
                draft.setLatestTicks(trimmed.equalsIgnoreCase("any") || trimmed.isEmpty()
                        ? Optional.empty() : Optional.of(Integer.parseInt(trimmed)));
                timing.setTextColor(TEXT_COLOR);
            } catch (IllegalArgumentException e) {
                timing.setTextColor(BAD_COLOR);
            }
        });
        this.label("game ticks, or any", fieldX + 44, rowY + 5, HINT_COLOR);

        rowY += ROW + GAP;
        this.label("Time limit", x, rowY + 5, TEXT_COLOR);
        EditBox limit = this.field(fieldX, rowY, 40, formatNumber(this.state.getTimeLimitSeconds()),
                "How long the solver may search (for Compose: per step)");
        limit.setResponder(text -> {
            try {
                this.state.setTimeLimitSeconds(Double.parseDouble(text.trim()));
                limit.setTextColor(TEXT_COLOR);
            } catch (IllegalArgumentException e) {
                limit.setTextColor(BAD_COLOR);
            }
        });
        this.label("seconds", fieldX + 44, rowY + 5, HINT_COLOR);

        rowY += ROW + GAP;
        this.label("Method", x, rowY + 5, TEXT_COLOR);
        boolean compose = this.state.getMethod() == ClientState.Method.COMPOSE;
        this.button(fieldX, rowY, 80, compose ? "Compose" : "Solve",
                "Solve: one search of the whole box (smallest circuits). Compose: on/off tables only, built from "
                        + "gates; handles bigger circuits and places the ports itself (the box grows to fit)",
                () -> {
                    this.state.setMethod(compose ? ClientState.Method.SOLVE : ClientState.Method.COMPOSE);
                    this.rebuildWidgets();
                });

        rowY += ROW + GAP;
        this.label("Your blocks", x, rowY + 5, TEXT_COLOR);
        boolean sketch = draft.getOtherBlocks() == CircuitDraft.OtherBlocks.SKETCH;
        this.button(fieldX, rowY, 110, sketch ? "Use as sketch" : "Ignore",
                "What the solver does with blocks you built in the box (other than kept cells): ignore them and "
                        + "start empty, or start from them and change what does not work",
                () -> {
                    draft.setOtherBlocks(sketch ? CircuitDraft.OtherBlocks.IGNORE : CircuitDraft.OtherBlocks.SKETCH);
                    this.rebuildWidgets();
                });

        rowY += ROW + GAP;
        this.button(x, rowY, 90, this.advancedOpen ? "Advanced -" : "Advanced +", "Settings most solves never need", () -> {
            this.advancedOpen = !this.advancedOpen;
            this.rebuildWidgets();
        });
        if (this.advancedOpen) {
            rowY += ROW + GAP;
            boolean drawn = this.state.isOnePlacement();
            this.button(x, rowY, 120, drawn ? "Only as drawn: on" : "Only as drawn: off",
                    "Check the circuit only as built here, facing this way. Allows faster circuits that may fail "
                            + "when built rotated or elsewhere",
                    () -> {
                        this.state.setOnePlacement(!drawn);
                        this.rebuildWidgets();
                    });
            this.label("Sketch step", x + 128, rowY + 5, TEXT_COLOR);
            EditBox sketchStep = this.field(x + 190, rowY, 34, formatNumber(this.state.getSketchSeconds()),
                    "Seconds the solver tries to keep a sketch before freeing part of it");
            sketchStep.setResponder(text -> {
                try {
                    this.state.setSketchSeconds(Double.parseDouble(text.trim()));
                    sketchStep.setTextColor(TEXT_COLOR);
                } catch (IllegalArgumentException e) {
                    sketchStep.setTextColor(BAD_COLOR);
                }
            });
        }

        rowY += ROW + GAP + 2;
        if (this.state.isSolving()) {
            this.button(x, rowY, 90, "Cancel", "Stop; the best circuit so far (if any) still comes back", () -> this.run(this.server::cancel));
        } else {
            this.button(x, rowY, 90, "Solve", "Send the circuit to the solver", () -> this.run(this.server::solve));
        }
        this.progressX = x + 96;
        this.progressY = rowY + 5;
    }

    private void initResult(int x, int y, int width) {
        this.resultLeft = x;
        this.resultTop = y;
        this.resultWidth = width;
        this.heading("Result", x, y);
        Optional<JsonObject> result = this.state.getLastResult();
        int buttonY = y + RESULT_CARD_HEIGHT - ROW - 2;
        int buttonX = x;
        if (result.isPresent() && this.state.isResultWaiting()) {
            boolean occupied = result.get().has("boxOccupied") && result.get().get("boxOccupied").getAsBoolean();
            buttonX += this.button(buttonX, buttonY, 80, occupied ? "Replace box" : "Place",
                    occupied ? "Clears the box (your build in it is lost) and builds the circuit" : "Builds the circuit in the box",
                    () -> this.run(this.server::place)) + GAP;
            buttonX += this.button(buttonX, buttonY, 60, "Discard", "Forget this circuit", () -> this.run(this.server::discard)) + GAP;
        }
        if (result.isPresent() && result.get().has("blockCount") && !this.state.isSolving()) {
            this.button(buttonX, buttonY, 56, "Shrink",
                    "Remove columns, rows and layers from this circuit while it still works (uses the time limit)",
                    () -> this.run(this.server::shrink));
        }
        if (result.isPresent() && result.get().has("blockCount")) {
            buttonX += 56 + GAP;
            this.button(buttonX, buttonY, 50, "Export", "Copy the circuit as /setblock commands to the clipboard",
                    () -> this.run(this.server::export));
        }
        if (result.isPresent() && result.get().has("log")) {
            this.button(x + width - 50, buttonY, 50, this.logOpen ? "Log -" : "Log +", "The solver's log", () -> {
                this.logOpen = !this.logOpen;
                this.rebuildWidgets();
            });
        }
    }

    private void extractResultText(GuiGraphicsExtractor graphics) {
        int x = this.resultLeft;
        int y = this.resultTop + LINE;
        int width = this.resultWidth;
        Optional<JsonObject> found = this.state.getLastResult();
        if (found.isEmpty()) {
            graphics.text(this.font, "Nothing solved yet.", x, y, HINT_COLOR);
            return;
        }
        JsonObject result = found.get();
        List<String> lines = new ArrayList<>();
        int color = TEXT_COLOR;
        if (this.logOpen) {
            JsonArray log = result.getAsJsonArray("log");
            for (int i = Math.max(0, log.size() - MAX_LOG_LINES); i < log.size(); i++) lines.add(log.get(i).getAsString());
            color = HINT_COLOR;
        } else if (result.has("error")) {
            lines.add("Failed: " + result.get("error").getAsString());
            color = BAD_COLOR;
        } else if (!result.has("blockCount")) {
            lines.add("No circuit found (%s after %.0f s).".formatted(result.get("status").getAsString(),
                    result.get("seconds").getAsDouble()));
            lines.add("Try a bigger box, looser timing or more time.");
            color = BAD_COLOR;
        } else {
            JsonArray size = result.getAsJsonArray("size");
            boolean optimal = result.get("optimal").getAsBoolean();
            graphics.text(this.font, "%s: %d blocks, %dx%dx%d, %.1f s".formatted(optimal ? "Smallest circuit" : "Circuit found",
                    result.get("blockCount").getAsInt(), size.get(0).getAsInt(), size.get(1).getAsInt(),
                    size.get(2).getAsInt(), result.get("seconds").getAsDouble()), x, y, GOOD_COLOR);
            y += LINE;
            if (!optimal) lines.add("(the solver did not prove it is the smallest)");
            lines.add("Timing: " + this.timingText(result));
            StringBuilder counts = new StringBuilder("Blocks: ");
            boolean first = true;
            for (Map.Entry<String, JsonElement> entry : result.getAsJsonObject("blocksByType").entrySet()) {
                if (!first) counts.append(", ");
                counts.append(entry.getValue().getAsInt()).append(' ').append(entry.getKey());
                first = false;
            }
            lines.add(counts.toString());
            if (!this.state.isResultWaiting()) lines.add("Placed or discarded.");
        }
        int maxY = this.resultTop + RESULT_CARD_HEIGHT - ROW - 4;
        for (String line : lines) {
            for (FormattedCharSequence part : this.font.split(Component.literal(line), width)) {
                if (y + LINE > maxY) return;
                graphics.text(this.font, part, x, y, color);
                y += LINE;
            }
        }
    }

    private String timingText(JsonObject result) {
        if (result.has("analysisError")) return "unknown (" + result.get("analysisError").getAsString() + ")";
        JsonArray settle = result.getAsJsonArray("settleTicks");
        if (settle == null || settle.isEmpty()) return "unknown";
        List<Port> outputs = this.state.getDraft().getOutputs();
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < settle.size(); i++) {
            if (!text.isEmpty()) text.append(", ");
            String name = i < outputs.size() ? outputs.get(i).name() : "OUT" + (i + 1);
            text.append(name).append(' ').append(settle.get(i).getAsInt()).append(" ticks");
        }
        return text.toString();
    }

    private void run(Runnable action) {
        try {
            action.run();
            this.message = "";
        } catch (IllegalArgumentException | IllegalStateException e) {
            this.message = "!" + e.getMessage();
        }
        this.rebuildWidgets();
    }

    private void heading(String text, int x, int y) {
        this.label(text, x, y, HEADING_COLOR);
    }

    private void label(String text, int x, int y, int color) {
        this.labels.add(new Label(text, x, y, color));
    }

    private EditBox field(int x, int y, int width, String value, String tooltip) {
        EditBox box = this.addRenderableWidget(new EditBox(this.font, x, y, width, ROW, Component.literal(tooltip)));
        box.setMaxLength(32);
        box.setValue(value);
        box.setTooltip(Tooltip.create(Component.literal(tooltip)));
        return box;
    }

    private int button(int x, int y, int width, String text, String tooltip, Runnable action) {
        this.addRenderableWidget(Button.builder(Component.literal(text), b -> action.run())
                .bounds(x, y, width, ROW)
                .tooltip(Tooltip.create(Component.literal(tooltip)))
                .build());
        return width;
    }

    private static void markAll(List<EditBox> fields, boolean valid) {
        for (EditBox field : fields) field.setTextColor(valid ? TEXT_COLOR : BAD_COLOR);
    }

    private static String formatNumber(double value) {
        return value == Math.rint(value) ? String.valueOf((long) value) : String.valueOf(value);
    }
}
