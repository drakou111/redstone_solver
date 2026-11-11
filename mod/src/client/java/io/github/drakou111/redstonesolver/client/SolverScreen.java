package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Durations;
import io.github.drakou111.redstonesolver.core.Formula;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.SignalRange;
import io.github.drakou111.redstonesolver.core.TruthRow;
import java.io.UncheckedIOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.function.Consumer;
import java.util.function.DoubleConsumer;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.util.FormattedCharSequence;
import net.minecraft.world.item.ItemStack;

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
    // Buttons beside a heading: short enough to stay inside the card.
    private static final int HEADER_BUTTON_HEIGHT = 14;
    private static final int HEADER_BUTTON_RISE = 3;
    private static final int MAX_FORMULA_LENGTH = 200;
    private static final int MANY_ROWS = 64;
    private static final Map<String, String> PALETTE_LABELS = Map.ofEntries(Map.entry("solid", "Solid block (wool)"),
            Map.entry("glass", "Glass (does not conduct)"), Map.entry("dust", "Redstone dust"),
            Map.entry("torch", "Redstone torch"),
            Map.entry("repeater", "Repeater"), Map.entry("comparator", "Comparator"), Map.entry("target", "Target"),
            Map.entry("barrel", "Barrel"),
            Map.entry("chest", "Chest"));
    private static final Map<String, String> PALETTE_ITEMS = Map.ofEntries(Map.entry("solid", "cyan_wool"),
            Map.entry("glass", "cyan_stained_glass"), Map.entry("dust", "redstone"), Map.entry("torch", "redstone_torch"),
            Map.entry("repeater", "repeater"),
            Map.entry("comparator", "comparator"), Map.entry("target", "target"),
            Map.entry("barrel", "barrel"), Map.entry("chest", "chest"));

    private static final int PANEL_COLOR = 0xE0101010;
    private static final int CARD_COLOR = 0x70000000;
    private static final int CARD_EDGE_COLOR = 0x60FF5555;
    private static final int CARD_PAD = 4;
    private static final int SECTION_GAP = 2 * CARD_PAD + 4;
    private static final int BOX_SECTION_HEIGHT = 3 * LINE + 2 * ROW + 2 * GAP;
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

    // A darker panel behind one section.
    private record Card(int left, int top, int right, int bottom) {
    }

    private final ClientState state;
    private final ServerLink server;
    private final DraftFiles files;
    private final List<Label> labels = new ArrayList<>();
    private final List<Card> cards = new ArrayList<>();
    private int seenVersion;
    private int tableScroll;
    private int visibleRows;
    private boolean dryRun;
    private int flowLeft;
    private int flowRight;
    private int flowX;
    private int flowY;
    private boolean logOpen;
    private String message = "";
    private String draftName = "";
    private String confirmOverwrite = "";
    private int resultTop;
    private int resultLeft;
    private int resultWidth;
    private int resultButtonsTop;
    private int progressX;
    private String formulaStatus = "";
    private boolean formulaStatusGood;
    private int formulaStatusX;
    private int formulaStatusY;
    private int formulaStatusWidth;
    private int progressY;

    public SolverScreen(ClientState state, ServerLink server, DraftFiles files) {
        super(Component.literal("Redstone Solver"));
        this.state = state;
        this.server = server;
        this.files = files;
    }

    @Override
    protected void init() {
        this.labels.clear();
        this.cards.clear();
        this.seenVersion = this.state.getVersion();
        int left = MARGIN + CARD_PAD + PAD;
        int top = MARGIN + CARD_PAD + PAD;
        int inner = this.width - 2 * (MARGIN + CARD_PAD + PAD);
        int leftWidth = Math.max(200, inner * 2 / 5);
        int rightLeft = left + leftWidth + 2 * CARD_PAD + SECTION_GAP;
        int rightWidth = left + inner - rightLeft;
        int bottom = this.height - MARGIN - CARD_PAD - PAD;

        // Left column: box, drafts, ports, result. Right: truth table, blocks, solve.
        int draftsTop = top + BOX_SECTION_HEIGHT + SECTION_GAP;
        int portsTop = draftsTop + LINE + ROW + SECTION_GAP;
        int resultTop = bottom - RESULT_CARD_HEIGHT;
        this.initBox(left, top);
        this.card(left, top, leftWidth, draftsTop - SECTION_GAP);
        this.initDrafts(left, draftsTop, leftWidth);
        this.card(left, draftsTop, leftWidth, portsTop - SECTION_GAP);
        this.initPorts(left, portsTop, leftWidth, resultTop - SECTION_GAP);
        this.card(left, portsTop, leftWidth, resultTop - SECTION_GAP);
        this.initResult(left, resultTop, leftWidth);
        this.card(left, resultTop, leftWidth, bottom);

        int solveTop = bottom - this.initSolve(rightLeft, 0, rightWidth, true);
        int blocksTop = solveTop - SECTION_GAP - this.initBlocks(rightLeft, 0, rightWidth, true);
        this.initTable(rightLeft, top, rightWidth, blocksTop - SECTION_GAP);
        this.card(rightLeft, top, rightWidth, blocksTop - SECTION_GAP);
        this.initBlocks(rightLeft, blocksTop, rightWidth, false);
        this.card(rightLeft, blocksTop, rightWidth, solveTop - SECTION_GAP);
        this.initSolve(rightLeft, solveTop, rightWidth, false);
        this.card(rightLeft, solveTop, rightWidth, bottom);
    }

    @Override
    public void tick() {
        if (this.state.getVersion() != this.seenVersion) this.rebuildWidgets();
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        graphics.fill(MARGIN, MARGIN, this.width - MARGIN, this.height - MARGIN, PANEL_COLOR);
        graphics.outline(MARGIN, MARGIN, this.width - 2 * MARGIN, this.height - 2 * MARGIN, BORDER_COLOR);
        for (Card card : this.cards) {
            graphics.fill(card.left(), card.top(), card.right(), card.bottom(), CARD_COLOR);
            graphics.fill(card.left(), card.top(), card.right(), card.top() + 1, CARD_EDGE_COLOR);
        }
        for (Label label : this.labels) graphics.text(this.font, label.text(), label.x(), label.y(), label.color());
        this.extractResultText(graphics);
        if (this.state.getDraft().isFormulaMode() && !this.formulaStatus.isEmpty()) {
            int y = this.formulaStatusY;
            for (FormattedCharSequence part : this.font.split(Component.literal(this.formulaStatus), this.formulaStatusWidth)) {
                graphics.text(this.font, part, this.formulaStatusX, y, this.formulaStatusGood ? GOOD_COLOR : BAD_COLOR);
                y += LINE;
            }
        }
        String status = this.state.isSolving()
                ? (this.state.isCancelling() ? "Cancelling... " : "") + this.state.getProgress()
                : this.message;
        if (!status.isEmpty()) {
            int color = this.state.isSolving() ? TEXT_COLOR : status.startsWith("!") ? BAD_COLOR : HINT_COLOR;
            String shown = this.font.plainSubstrByWidth(status.startsWith("!") ? status.substring(1) : status,
                    this.width - MARGIN - PAD - this.progressX);
            graphics.text(this.font, shown, this.progressX, this.progressY, color);
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

    // Named drafts: save, load or start over without typing commands.
    private void initDrafts(int x, int y, int width) {
        this.heading("Drafts", x, y);
        List<String> names = this.files.listNames();
        String saved = names.isEmpty() ? "none saved yet" : String.join(", ", names);
        int labelX = x + this.font.width("Drafts") + 2 * GAP;
        this.label(this.font.plainSubstrByWidth("Saved: " + saved, Math.max(0, width - (labelX - x))), labelX, y, HINT_COLOR);
        int rowY = y + LINE;
        EditBox name = this.field(x, rowY, 80, this.draftName, "Draft name (letters, digits, _ and -)");
        name.setResponder(text -> {
            this.draftName = text.trim();
            this.confirmOverwrite = "";
        });
        int buttonX = x + 80 + GAP;
        boolean confirming = !this.confirmOverwrite.isEmpty() && this.confirmOverwrite.equals(this.draftName);
        buttonX += this.button(buttonX, rowY, confirming ? 64 : 40, confirming ? "Replace?" : "Save",
                "Save the box, ports, truth table and settings under this name", this::saveDraft) + GAP;
        buttonX += this.button(buttonX, rowY, 40, "Load", "Load the draft with this name (your current one is replaced)",
                this::loadDraft) + GAP;
        this.button(buttonX, rowY, 40, "New", "Start an empty draft (save first if you want to keep this one)", () -> {
            this.state.replaceDraft(new CircuitDraft());
            this.message = "Started a new, empty draft.";
            this.rebuildWidgets();
        });
    }

    private void saveDraft() {
        boolean overwrite = !this.confirmOverwrite.isEmpty() && this.confirmOverwrite.equals(this.draftName);
        try {
            this.files.save(this.draftName, this.state, overwrite);
            this.message = "Saved as " + this.draftName + ".";
            this.confirmOverwrite = "";
        } catch (IllegalStateException e) {
            this.message = "!" + this.draftName + " exists; click Replace? to overwrite it.";
            this.confirmOverwrite = this.draftName;
        } catch (IllegalArgumentException | UncheckedIOException e) {
            this.message = "!" + e.getMessage();
        }
        this.rebuildWidgets();
    }

    private void loadDraft() {
        try {
            this.message = this.files.load(this.draftName, this.state)
                    ? "Loaded " + this.draftName + "."
                    : "!There is no draft named " + this.draftName + ".";
        } catch (IllegalArgumentException | UncheckedIOException e) {
            this.message = "!" + e.getMessage();
        }
        this.tableScroll = 0;
        this.rebuildWidgets();
    }

    private void initPorts(int x, int y, int width, int bottom) {
        CircuitDraft draft = this.state.getDraft();
        this.heading("Ports", x, y);
        List<Port> ports = new ArrayList<>(draft.getInputs());
        ports.addAll(draft.getOutputs());
        if (ports.isEmpty()) {
            this.label("None yet: place a port block, look at it, press I or O.", x, y + LINE + 2,
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
        boolean formulas = draft.isFormulaMode();
        this.headerButton(x + this.font.width("Truth table") + 2 * GAP, y, 96, formulas ? "Using: formulas" : "Using: rows",
                "Rows: type the output range for each input case. Formulas: one formula per output, such as "
                        + "(A + B) % 16, expanded into every case for you",
                () -> {
                    draft.setFormulaMode(!formulas);
                    this.rebuildWidgets();
                });
        if (formulas) {
            this.initFormulas(x, y + ROW, width, inputs, outputs);
            this.visibleRows = 0;
            return;
        }
        this.label("Ranges: 0, 15, 1..15, or * for anything. Scroll for more rows.", x, y + ROW, HINT_COLOR);
        int headerY = y + ROW + LINE + 2;
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

    // One formula field per output and an on/off or 0-15 switch per input.
    private void initFormulas(int x, int y, int width, List<Port> inputs, List<Port> outputs) {
        CircuitDraft draft = this.state.getDraft();
        this.label("Use input names with + - * / %, min max abs, < <= > >= == !=, and or not, ? :", x, y, HINT_COLOR);
        this.label("A comparison or and/or/not means on/off; anything else is the exact strength.", x, y + LINE,
                HINT_COLOR);

        int rowY = y + 2 * LINE + 2;
        this.label("Inputs:", x, rowY + 5, TEXT_COLOR);
        int buttonX = x + this.font.width("Inputs:") + GAP;
        for (Port input : inputs) {
            boolean analog = draft.isAnalogInput(input.name());
            String text = input.name() + (analog ? ": 0-15" : ": on/off");
            int buttonWidth = this.font.width(text) + 12;
            if (buttonX + buttonWidth > x + width) {
                rowY += ROW + 2;
                buttonX = x;
            }
            buttonX += this.button(buttonX, rowY, buttonWidth, text,
                    "on/off: the formula sees this input as 0 or 15. 0-15: every strength is tried (16 times the cases)",
                    () -> {
                        draft.setAnalogInput(input.name(), !analog);
                        this.rebuildWidgets();
                    }) + GAP;
        }

        rowY += ROW + GAP;
        int labelWidth = 0;
        for (Port output : outputs) labelWidth = Math.max(labelWidth, this.font.width(output.name() + " ="));
        for (Port output : outputs) {
            this.label(output.name() + " =", x, rowY + 5, OUTPUT_COLOR);
            int fieldX = x + labelWidth + GAP;
            EditBox field = this.field(fieldX, rowY, x + width - fieldX, draft.getFormula(output.name()),
                    "Formula for " + output.name() + ", for example (A + B) % 16 or A + B >= 16");
            field.setMaxLength(MAX_FORMULA_LENGTH);
            field.setResponder(text -> {
                draft.setFormula(output.name(), text);
                field.setTextColor(formulaProblem(text, inputs).isEmpty() ? TEXT_COLOR : BAD_COLOR);
                this.updateFormulaStatus();
            });
            field.setTextColor(formulaProblem(draft.getFormula(output.name()), inputs).isEmpty() ? TEXT_COLOR : BAD_COLOR);
            rowY += ROW + 2;
        }
        this.formulaStatusX = x;
        this.formulaStatusY = rowY + 4;
        this.formulaStatusWidth = width;
        this.updateFormulaStatus();
    }

    private void updateFormulaStatus() {
        try {
            int rows = this.state.getDraft().formulaRows().size();
            this.formulaStatus = "Makes %d rows (every input case).".formatted(rows)
                    + (rows > MANY_ROWS ? " That many makes solving slow." : "");
            this.formulaStatusGood = true;
        } catch (IllegalArgumentException e) {
            this.formulaStatus = e.getMessage();
            this.formulaStatusGood = false;
        }
    }

    // Empty when the text is a formula over existing inputs; a blank field is not a problem yet.
    private static Optional<String> formulaProblem(String text, List<Port> inputs) {
        if (text.isBlank()) return Optional.empty();
        try {
            Formula formula = Formula.parse(text);
            for (String name : formula.variables()) {
                boolean known = false;
                for (Port input : inputs) known |= input.name().equals(name);
                if (!known) return Optional.of("there is no input named " + name);
            }
            return Optional.empty();
        } catch (IllegalArgumentException e) {
            return Optional.of(e.getMessage());
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

    // Solve settings; the controls flow left to right and wrap. Returns the height used, so the
    // section can be measured first (dry run) and then built at the bottom of the panel.
    private int initSolve(int x, int y, int width, boolean dryRun) {
        this.dryRun = dryRun;
        CircuitDraft draft = this.state.getDraft();
        this.heading("Solve", x, y);
        this.flowStart(x, y + LINE, x + width);

        this.flowLabel("Output changes: not before");
        this.ticksField(draft.getEarliestTicks(), draft::setEarliestTicks, "-",
                "A delay: outputs keep their old value until this many game ticks after an input changes "
                        + "(20 per second). Empty or - for none. Equal to 'by' gives an exact delay");
        this.flowLabel("by");
        this.ticksField(draft.getLatestTicks(), draft::setLatestTicks, "any",
                "Outputs must be right this many game ticks after an input changes (20 per second), or any");
        this.flowHint("ticks");
        this.flowLabel("Time limit");
        this.durationField(this.state.getTimeLimitSeconds(), this.state::setTimeLimitSeconds,
                "How long the solver may take in all (building, then shrinking): 90s, 10m, 1h");

        this.flowNewLine();
        boolean compose = this.state.getMethod() == ClientState.Method.COMPOSE;
        this.flowButton(compose ? "Build from gates" : "Search the box",
                "Search the box: one search for the smallest circuit in your box. Build from gates: on/off tables "
                        + "only; builds each gate and wires them up, handles bigger tables, places the ports itself "
                        + "and stays within your box's height (it is wider than a search result)",
                () -> {
                    this.state.setMethod(compose ? ClientState.Method.SOLVE : ClientState.Method.COMPOSE);
                    this.rebuildWidgets();
                });
        if (!compose) {
            boolean alongSide = this.state.isPortsAlongSide();
            this.flowButton(alongSide ? "Ports: along their side" : "Ports: as placed",
                    "As placed: inputs and outputs stay exactly where you put them. Along their side: each may "
                            + "move along the side of the box it is on (same height) and turn, which can give "
                            + "smaller circuits but searches longer; placing the result moves your ports",
                    () -> {
                        this.state.setPortsAlongSide(!alongSide);
                        this.rebuildWidgets();
                    });
        }
        if (compose) {
            boolean shrinkAfter = this.state.isAutoShrink();
            this.flowButton(shrinkAfter ? "Then shrink: on" : "Then shrink: off",
                    "Shrink the built circuit right away (gates come out spread wide; this takes more time)",
                    () -> {
                        this.state.setAutoShrink(!shrinkAfter);
                        this.rebuildWidgets();
                    });
            boolean inOrder = this.state.isInputsInOrder();
            this.flowButton(inOrder ? "Inputs: in order" : "Inputs: any order",
                    "In order: the inputs go down the west side as IN1, IN2, ... Any order: the solver picks the "
                            + "order, which usually gives a smaller circuit. Wires that must cross need a box 4 high",
                    () -> {
                        this.state.setInputsInOrder(!inOrder);
                        this.rebuildWidgets();
                    });
            ClientState.OutputOrder outputOrder = this.state.getOutputOrder();
            this.flowButton(outputOrder.getLabel(),
                    "Any order: outputs end where the circuit makes them. In order: they come out down the east "
                            + "side as OUT1, OUT2, ... In order or flipped: OUT1 first or last, whichever gives the "
                            + "smaller circuit",
                    () -> {
                        this.state.setOutputOrder(outputOrder.next());
                        this.rebuildWidgets();
                    });
        }
        boolean sketch = draft.getOtherBlocks() == CircuitDraft.OtherBlocks.SKETCH;
        this.flowButton(sketch ? "Your blocks: sketch" : "Your blocks: ignore",
                "What the solver does with blocks you built in the box (other than kept ones): ignore them and "
                        + "start empty, or start from them as a sketch and change what does not work",
                () -> {
                    draft.setOtherBlocks(sketch ? CircuitDraft.OtherBlocks.IGNORE : CircuitDraft.OtherBlocks.SKETCH);
                    this.rebuildWidgets();
                });
        this.flowLabel("Sketch step");
        EditBox sketchStep = this.flowField(30, formatNumber(this.state.getSketchSeconds()),
                "Seconds the solver tries to keep a sketch before freeing part of it");
        if (sketchStep != null) {
            sketchStep.setResponder(text -> {
                try {
                    this.state.setSketchSeconds(Double.parseDouble(text.trim()));
                    sketchStep.setTextColor(TEXT_COLOR);
                } catch (IllegalArgumentException e) {
                    sketchStep.setTextColor(BAD_COLOR);
                }
            });
        }

        this.flowNewLine();
        if (this.state.isSolving()) {
            boolean cancelling = this.state.isCancelling();
            Button cancel = this.flowButton(Component.literal(cancelling ? "Cancelling..." : "Cancel"), 90,
                    "Stop; the best circuit so far (if any) still comes back", () -> this.run(this.server::cancel));
            if (cancel != null) cancel.active = !cancelling;
        } else {
            this.flowButton(Component.literal("Solve"), 90, "Send the circuit to the solver", () -> this.run(this.server::solve));
            this.flowButton(Component.literal("Shrink my build"), 100,
                    "Shrink the circuit you built in the box: it is checked first, then columns, rows and layers are "
                            + "removed while it still works",
                    () -> this.run(this.server::shrinkBuild));
        }
        this.progressX = x;
        this.progressY = this.flowY + ROW + 3;
        return this.progressY + LINE - y;
    }

    private void initResult(int x, int y, int width) {
        this.resultLeft = x;
        this.resultTop = y;
        this.resultWidth = width;
        this.heading("Result", x, y);
        Optional<JsonObject> result = this.state.getSelectedResult();
        int count = this.state.getResultCount();
        if (count > 1) {
            // Older and newer results: each can be placed, shrunk or exported.
            int navX = x + this.font.width("Result") + 2 * GAP;
            this.headerButton(navX, y, SMALL_BUTTON, "<", "Older result", () -> this.state.step(-1));
            this.label("%d/%d".formatted(this.state.getSelectedIndex() + 1, count), navX + SMALL_BUTTON + GAP, y,
                    HINT_COLOR);
            this.headerButton(navX + SMALL_BUTTON + 2 * GAP + this.font.width(count + "/" + count), y, SMALL_BUTTON, ">",
                    "Newer result", () -> this.state.step(1));
        }
        if (result.isPresent() && result.get().has("log")) {
            this.headerButton(x + width - 40, y, 40, this.logOpen ? "Log -" : "Log +", "The solver's log", () -> {
                this.logOpen = !this.logOpen;
                this.rebuildWidgets();
            });
        }
        // Enough rows for the buttons below the text: measured, then built.
        boolean wasDryRun = this.dryRun;
        this.dryRun = true;
        this.flowStart(x, 0, x + width);
        this.resultButtons(result);
        int buttonRows = this.flowY / (ROW + GAP) + 1;
        this.dryRun = wasDryRun;
        this.resultButtonsTop = y + RESULT_CARD_HEIGHT - buttonRows * (ROW + GAP) + GAP - 2;
        this.flowStart(x, this.resultButtonsTop, x + width);
        this.resultButtons(result);
    }

    private void resultButtons(Optional<JsonObject> result) {
        boolean hasCircuit = result.isPresent() && result.get().has("id");
        if (hasCircuit) {
            boolean occupied = result.get().has("boxOccupied") && result.get().get("boxOccupied").getAsBoolean();
            this.flowButton(Component.literal(occupied ? "Replace box" : "Place in box"), occupied ? 70 : 64,
                    occupied ? "Clears the box (blocks in it are lost) and builds the circuit"
                            : "Builds the circuit where it was solved; can be done again",
                    () -> this.run(this.server::place));
            this.flowButton(Component.literal("Place here"), 64,
                    "Builds a copy where you stand, turned to face the way you look (blocks there are replaced)",
                    () -> this.run(this::placeHere));
            this.flowButton(Component.literal("Discard"), 50, "Forget this circuit", () -> this.run(this.server::discard));
            if (result.get().has("fromLibrary") && result.get().get("fromLibrary").getAsBoolean() && !this.state.isSolving()) {
                this.flowButton(Component.literal("Search again"), 76,
                        "This circuit was found before and came from the circuit library; search anew, ignoring it",
                        () -> this.run(this.server::searchAgain));
            }
        }
        if (hasCircuit && !this.state.isSolving()) {
            this.flowButton(Component.literal("Shrink"), 46,
                    "Remove columns, rows and layers from this circuit while it still works (uses the time limit)",
                    () -> this.run(this.server::shrink));
        }
        if (hasCircuit) {
            this.flowButton(Component.literal("Export"), 46, "Copy the circuit as /setblock commands to the clipboard",
                    () -> this.run(this.server::export));
        }
    }

    // The blocks the solver may build with, as inventory slots. Returns the height used.
    private int initBlocks(int x, int y, int width, boolean dryRun) {
        CircuitDraft draft = this.state.getDraft();
        List<String> blocks = CircuitDraft.DEFAULT_PALETTE;
        int perRow = Math.max(1, (width + 2) / (BlockToggle.SIZE + 2));
        int rows = (blocks.size() + perRow - 1) / perRow;
        int height = LINE + 2 + rows * (BlockToggle.SIZE + 2);
        if (dryRun) return height;

        this.heading("Blocks it may use", x, y);
        this.label("%d of %d (click to change)".formatted(draft.getPalette().size(), blocks.size()),
                x + this.font.width("Blocks it may use") + 2 * GAP, y, HINT_COLOR);
        for (int i = 0; i < blocks.size(); i++) {
            String block = blocks.get(i);
            boolean allowed = draft.allows(block);
            int slotX = x + (i % perRow) * (BlockToggle.SIZE + 2);
            int slotY = y + LINE + 2 + (i / perRow) * (BlockToggle.SIZE + 2);
            ItemStack icon = new ItemStack(BuiltInRegistries.ITEM.getValue(
                    Identifier.withDefaultNamespace(PALETTE_ITEMS.get(block))));
            this.addRenderableWidget(new BlockToggle(slotX, slotY, this.font, PALETTE_LABELS.get(block), icon, allowed,
                    () -> this.run(() -> draft.setAllowed(block, !allowed))));
        }
        return height;
    }

    private void card(int x, int top, int width, int bottom) {
        this.cards.add(new Card(x - CARD_PAD, top - CARD_PAD, x + width + CARD_PAD, bottom + CARD_PAD));
    }

    // A time field (90s, 10m, 1h); red until the text is a valid time.
    private void durationField(double seconds, DoubleConsumer setter, String tooltip) {
        EditBox field = this.flowField(36, Durations.format(seconds), tooltip);
        if (field == null) return;
        field.setResponder(text -> {
            try {
                setter.accept(Durations.parseSeconds(text));
                field.setTextColor(TEXT_COLOR);
            } catch (IllegalArgumentException e) {
                field.setTextColor(BAD_COLOR);
            }
        });
    }

    // A ticks field where a word (or nothing) means no limit; red until the text is valid.
    private void ticksField(Optional<Integer> value, Consumer<Optional<Integer>> setter, String none, String tooltip) {
        EditBox field = this.flowField(32, value.map(String::valueOf).orElse(none), tooltip);
        if (field == null) return;
        field.setResponder(text -> {
            String trimmed = text.trim();
            try {
                boolean empty = trimmed.isEmpty() || trimmed.equals("-") || trimmed.equalsIgnoreCase("any");
                setter.accept(empty ? Optional.empty() : Optional.of(Integer.parseInt(trimmed)));
                field.setTextColor(TEXT_COLOR);
            } catch (IllegalArgumentException e) {
                field.setTextColor(BAD_COLOR);
            }
        });
    }

    // A tiny flow layout: controls go left to right and wrap to a new row at the right edge.
    private void flowStart(int left, int y, int right) {
        this.flowLeft = left;
        this.flowRight = right;
        this.flowX = left;
        this.flowY = y;
    }

    private void flowNewLine() {
        if (this.flowX == this.flowLeft) return;
        this.flowX = this.flowLeft;
        this.flowY += ROW + GAP;
    }

    private int flowPlace(int width) {
        if (this.flowX + width > this.flowRight && this.flowX > this.flowLeft) {
            this.flowX = this.flowLeft;
            this.flowY += ROW + GAP;
        }
        int x = this.flowX;
        this.flowX += width + GAP;
        return x;
    }

    private void flowLabel(String text) {
        int x = this.flowPlace(this.font.width(text));
        if (!this.dryRun) this.label(text, x, this.flowY + 5, TEXT_COLOR);
    }

    private void flowHint(String text) {
        int x = this.flowPlace(this.font.width(text) + 4);
        if (!this.dryRun) this.label(text, x, this.flowY + 5, HINT_COLOR);
    }

    private EditBox flowField(int width, String value, String tooltip) {
        int x = this.flowPlace(width);
        return this.dryRun ? null : this.field(x, this.flowY, width, value, tooltip);
    }

    private void flowButton(String text, String tooltip, Runnable action) {
        this.flowButton(Component.literal(text), this.font.width(text) + 12, tooltip, action);
    }

    private Button flowButton(Component text, int width, String tooltip, Runnable action) {
        int x = this.flowPlace(width);
        if (this.dryRun) return null;
        return this.addRenderableWidget(Button.builder(text, b -> action.run())
                .bounds(x, this.flowY, width, ROW)
                .tooltip(Tooltip.create(Component.literal(tooltip)))
                .build());
    }

    private void extractResultText(GuiGraphicsExtractor graphics) {
        int x = this.resultLeft;
        int y = this.resultTop + LINE;
        int width = this.resultWidth;
        Optional<JsonObject> found = this.state.getSelectedResult();
        if (found.isEmpty()) {
            graphics.text(this.font, "Nothing solved yet.", x, y, HINT_COLOR);
            return;
        }
        JsonObject result = found.get();
        List<String> lines = new ArrayList<>();
        int color = TEXT_COLOR;
        if (this.logOpen) {
            JsonArray log = result.getAsJsonArray("log");
            List<String> entries = new ArrayList<>();
            for (JsonElement entry : log) entries.addAll(List.of(entry.getAsString().split("\\R")));
            lines.addAll(entries.subList(Math.max(0, entries.size() - MAX_LOG_LINES), entries.size()));
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
            if (result.has("cancelled") && result.get("cancelled").getAsBoolean())
                lines.add("Stopped early (you cancelled): this is the best found so far.");
            else if (!optimal) lines.add("(the solver did not prove it is the smallest)");
            if (result.has("note")) lines.add(result.get("note").getAsString());
            if (result.has("fromLibrary") && result.get("fromLibrary").getAsBoolean())
                lines.add("From the circuit library (found before). Search again looks for a better one.");
            lines.add("Timing: " + this.timingText(result));
            this.deadlineNote(result).ifPresent(lines::add);
            StringBuilder counts = new StringBuilder("Blocks: ");
            boolean first = true;
            for (Map.Entry<String, JsonElement> entry : result.getAsJsonObject("blocksByType").entrySet()) {
                if (!first) counts.append(", ");
                counts.append(entry.getValue().getAsInt()).append(' ').append(entry.getKey());
                first = false;
            }
            lines.add(counts.toString());
        }
        int maxY = this.resultButtonsTop - 2;
        for (String line : lines) {
            for (FormattedCharSequence part : this.font.split(Component.literal(line), width)) {
                if (y + LINE > maxY) return;
                graphics.text(this.font, part, x, y, color);
                y += LINE;
            }
        }
    }

    // Build from gates checks the timing setting on the finished circuit; say how this result compares.
    private Optional<String> deadlineNote(JsonObject result) {
        boolean fromGates = result.has("builtFromGates") && result.get("builtFromGates").getAsBoolean();
        Optional<Integer> latest = this.state.getDraft().getLatestTicks();
        if (!fromGates || latest.isEmpty()) return Optional.empty();
        JsonArray settle = result.getAsJsonArray("settleTicks");
        if (settle == null || settle.isEmpty()) return Optional.empty();
        int slowest = 0;
        for (JsonElement ticks : settle) slowest = Math.max(slowest, ticks.getAsInt());
        return Optional.of(slowest <= latest.get()
                ? "Within your %d ticks (checked).".formatted(latest.get())
                : "Misses your deadline: %d ticks, you asked for %d.".formatted(slowest, latest.get()));
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
        if (this.dryRun) return;
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

    // A copy of the shown result where the player stands, facing their way.
    private void placeHere() {
        LocalPlayer player = Minecraft.getInstance().player;
        if (player == null) throw new IllegalStateException("Not in a world.");
        BlockPos pos = player.blockPosition();
        this.server.placeAt(new Cell(pos.getX(), pos.getY(), pos.getZ()), ClientRsCommands.toDir(player.getDirection()));
    }

    // A button beside a heading at y, kept inside the heading's card.
    private void headerButton(int x, int y, int width, String text, String tooltip, Runnable action) {
        this.addRenderableWidget(Button.builder(Component.literal(text), b -> action.run())
                .bounds(x, y - HEADER_BUTTON_RISE, width, HEADER_BUTTON_HEIGHT)
                .tooltip(Tooltip.create(Component.literal(tooltip)))
                .build());
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
