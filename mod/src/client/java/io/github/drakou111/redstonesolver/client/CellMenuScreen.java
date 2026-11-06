package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.InputKind;
import io.github.drakou111.redstonesolver.core.OutputKind;
import io.github.drakou111.redstonesolver.core.Port;
import java.util.Optional;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * The P menu for one cell: make it an input or output (facing the way the player looked), keep
 * the looked-at block, force the cell empty, or clear its marks.
 */
public final class CellMenuScreen extends Screen {
    private static final int PANEL_WIDTH = 300;
    private static final int PANEL_HEIGHT = 178;
    private static final int PADDING = 8;
    private static final int ROW_HEIGHT = 20;
    private static final int GAP = 4;
    private static final int NAME_WIDTH = 50;
    private static final int LABEL_WIDTH = 46;
    private static final int PANEL_COLOR = 0xE0101010;
    private static final int BORDER_COLOR = 0xFF8B1A1A;
    private static final int TITLE_COLOR = 0xFFFF5555;
    private static final int TEXT_COLOR = 0xFFFFFFFF;
    private static final int HINT_COLOR = 0xFFAAAAAA;
    private static final int ERROR_COLOR = 0xFFFF6060;
    private static final int MAX_NAME_LENGTH = 16;

    private final ClientState state;
    private final Cell placementCell;
    private final Optional<Cell> lookedAtCell;
    private final Dir facing;
    private final String lookedAtName;
    private EditBox inputName;
    private EditBox outputName;
    private String message = "";
    private boolean messageIsError;
    private int left;
    private int top;

    /**
     * @param placementCell the cell where a block would be placed (ports and empty marks go here)
     * @param lookedAtCell the block the player looks at (keep marks go here)
     */
    public CellMenuScreen(ClientState state, Cell placementCell, Optional<Cell> lookedAtCell, Dir facing,
            String lookedAtName) {
        super(Component.literal("Redstone Solver cell"));
        this.state = state;
        this.placementCell = placementCell;
        this.lookedAtCell = lookedAtCell;
        this.facing = facing;
        this.lookedAtName = lookedAtName;
    }

    @Override
    protected void init() {
        this.left = (this.width - PANEL_WIDTH) / 2;
        this.top = (this.height - PANEL_HEIGHT) / 2;
        CircuitDraft draft = this.state.getDraft();
        int x0 = this.left + PADDING + LABEL_WIDTH;
        int y = this.top + PADDING + 2 * 12 + GAP;

        this.inputName = this.addRenderableWidget(new EditBox(this.font, x0, y, NAME_WIDTH, ROW_HEIGHT,
                Component.literal("Input name")));
        this.inputName.setMaxLength(MAX_NAME_LENGTH);
        this.inputName.setValue(this.defaultName(true));
        this.inputName.setTooltip(Tooltip.create(Component.literal("The input's name, used in the truth table")));
        int x = x0 + NAME_WIDTH + GAP;
        for (InputKind kind : InputKind.values()) {
            x += this.kindButton(x, y, kind.jsonName(), () -> this.addPort(true, kind.jsonName()), inputTooltip(kind)) + 2;
        }

        y += ROW_HEIGHT + GAP;
        this.outputName = this.addRenderableWidget(new EditBox(this.font, x0, y, NAME_WIDTH, ROW_HEIGHT,
                Component.literal("Output name")));
        this.outputName.setMaxLength(MAX_NAME_LENGTH);
        this.outputName.setValue(this.defaultName(false));
        this.outputName.setTooltip(Tooltip.create(Component.literal("The output's name, used in the truth table")));
        x = x0 + NAME_WIDTH + GAP;
        for (OutputKind kind : OutputKind.values()) {
            x += this.kindButton(x, y, kind.jsonName(), () -> this.addPort(false, kind.jsonName()), outputTooltip(kind)) + 2;
        }

        y += ROW_HEIGHT + GAP * 3;
        int third = (PANEL_WIDTH - 2 * PADDING - 2 * GAP) / 3;
        Button keep = this.addRenderableWidget(Button.builder(Component.literal("Keep block"), button -> this.keep())
                .bounds(this.left + PADDING, y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The solver must keep the block you look at ("
                        + this.lookedAtName + ") exactly as it is")))
                .build());
        keep.active = this.lookedAtCell.isPresent();
        this.addRenderableWidget(Button.builder(Component.literal("Must stay empty"), button -> this.markEmpty())
                .bounds(this.left + PADDING + third + GAP, y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The solver may not put anything in this cell")))
                .build());
        this.addRenderableWidget(Button.builder(Component.literal("Clear"), button -> this.clear())
                .bounds(this.left + PADDING + 2 * (third + GAP), y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("Remove the port, keep or empty mark of this cell and the block you look at")))
                .build());

        this.addRenderableWidget(Button.builder(Component.literal("Done"), button -> this.onClose())
                .bounds(this.left + PANEL_WIDTH - PADDING - 60, this.top + PANEL_HEIGHT - PADDING - ROW_HEIGHT, 60, ROW_HEIGHT)
                .build());
        this.setInitialFocus(this.inputName);
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        graphics.fill(this.left, this.top, this.left + PANEL_WIDTH, this.top + PANEL_HEIGHT, PANEL_COLOR);
        graphics.outline(this.left, this.top, PANEL_WIDTH, PANEL_HEIGHT, BORDER_COLOR);
        int x = this.left + PADDING;
        int y = this.top + PADDING;
        graphics.text(this.font, "Cell " + this.placementCell + "  " + this.describeCell(), x, y, TITLE_COLOR);
        graphics.text(this.font, "Ports face " + this.facing.jsonName() + " (the way you look).", x, y + 12, HINT_COLOR);
        int rowY = y + 2 * 12 + GAP;
        graphics.text(this.font, "Input", x, rowY + 6, TEXT_COLOR);
        graphics.text(this.font, "Output", x, rowY + ROW_HEIGHT + GAP + 6, TEXT_COLOR);
        int marksY = rowY + 2 * (ROW_HEIGHT + GAP) + GAP;
        graphics.text(this.font, "Keep: the block you look at. Empty and ports: the cell in front of it.", x, marksY - 1,
                HINT_COLOR);
        if (!this.message.isEmpty()) {
            graphics.textWithWordWrap(this.font, Component.literal(this.message), x, marksY + ROW_HEIGHT + 2 * GAP + 6,
                    PANEL_WIDTH - 2 * PADDING - 70, this.messageIsError ? ERROR_COLOR : TEXT_COLOR);
        }
        super.extractRenderState(graphics, mouseX, mouseY, partialTick);
    }

    @Override
    public boolean isPauseScreen() {
        return false;
    }

    private int kindButton(int x, int y, String kind, Runnable action, String tooltip) {
        int width = this.font.width(kind) + 10;
        this.addRenderableWidget(Button.builder(Component.literal(kind), button -> action.run())
                .bounds(x, y, width, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal(tooltip)))
                .build());
        return width;
    }

    private void addPort(boolean input, String kind) {
        String name = (input ? this.inputName : this.outputName).getValue().trim();
        if (name.isEmpty() || !name.matches("[A-Za-z0-9_]+")) {
            this.show("Names use letters, digits and _ only.", true);
            return;
        }
        Port port = input
                ? Port.input(name, InputKind.parse(kind).orElseThrow(), this.placementCell, this.facing)
                : Port.output(name, OutputKind.parse(kind).orElseThrow(), this.placementCell, this.facing);
        this.apply(() -> {
            Optional<Port> old = this.state.getDraft().portAt(this.placementCell);
            if (old.isPresent() && !old.get().name().equals(name)) this.state.getDraft().removePort(old.get().name());
            this.state.getDraft().putPort(port);
        }, (input ? "Input " : "Output ") + port.describe());
    }

    private void keep() {
        this.apply(() -> this.state.getDraft().markKept(this.lookedAtCell.orElseThrow()),
                "The solver keeps the " + this.lookedAtName + " at " + this.lookedAtCell.orElseThrow() + ".");
    }

    private void markEmpty() {
        this.apply(() -> this.state.getDraft().markEmpty(this.placementCell), "Cell " + this.placementCell + " must stay empty.");
    }

    private void clear() {
        boolean changed = this.state.getDraft().clearCell(this.placementCell);
        if (this.lookedAtCell.isPresent()) changed |= this.state.getDraft().clearCell(this.lookedAtCell.get());
        this.show(changed ? "Cleared." : "Nothing to clear here.", false);
    }

    private void apply(Runnable change, String success) {
        try {
            change.run();
            this.show(success, false);
            this.inputName.setValue(this.defaultName(true));
            this.outputName.setValue(this.defaultName(false));
        } catch (IllegalArgumentException | IllegalStateException e) {
            this.show(e.getMessage(), true);
        }
    }

    private void show(String text, boolean error) {
        this.message = text;
        this.messageIsError = error;
    }

    private String defaultName(boolean input) {
        Optional<Port> existing = this.state.getDraft().portAt(this.placementCell);
        if (existing.isPresent() && existing.get().input() == input) return existing.get().name();
        return this.state.getDraft().nextFreeName(input ? "IN" : "OUT");
    }

    private String describeCell() {
        CircuitDraft draft = this.state.getDraft();
        if (draft.getBox().isEmpty()) return "(set the box first: J and K)";
        if (!draft.getBox().get().contains(this.placementCell)) return "(outside the box)";
        Optional<Port> port = draft.portAt(this.placementCell);
        if (port.isPresent()) return "(" + port.get().name() + ", " + port.get().kind() + ")";
        if (draft.getEmptyCells().contains(this.placementCell)) return "(must stay empty)";
        return "";
    }

    private static String inputTooltip(InputKind kind) {
        return switch (kind) {
            case COMPARATOR -> "A comparator facing the way you look sends the input signal (0-15)";
            case REPEATER -> "A repeater facing the way you look sends on (15) or off";
            case DUST -> "Dust held at the input signal";
            case CONTAINER -> "A barrel whose comparator signal is the input";
        };
    }

    private static String outputTooltip(OutputKind kind) {
        return switch (kind) {
            case DUST -> "The output is the power of dust in this cell";
            case REPEATER -> "The output is what a repeater facing the way you look receives";
            case COMPARATOR -> "The output is what a comparator facing the way you look reads";
        };
    }
}
