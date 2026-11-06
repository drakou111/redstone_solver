package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.PortBlock;
import java.util.Optional;
import java.util.function.Supplier;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * The P menu for the looked-at block: if it is a repeater, comparator, dust or barrel, make it an
 * input or output (kind and facing come from the block). Also: keep the block, force the cell in
 * front of it empty, or clear marks.
 */
public final class CellMenuScreen extends Screen {
    private static final int PANEL_WIDTH = 300;
    private static final int PANEL_HEIGHT = 150;
    private static final int PADDING = 8;
    private static final int ROW_HEIGHT = 20;
    private static final int GAP = 4;
    private static final int LINE = 12;
    private static final int NAME_WIDTH = 60;
    private static final int PANEL_COLOR = 0xE0101010;
    private static final int BORDER_COLOR = 0xFF8B1A1A;
    private static final int TITLE_COLOR = 0xFFFF5555;
    private static final int TEXT_COLOR = 0xFFFFFFFF;
    private static final int HINT_COLOR = 0xFFAAAAAA;
    private static final int ERROR_COLOR = 0xFFFF6060;
    private static final int MAX_NAME_LENGTH = 16;

    private final ClientState state;
    private final Cell blockCell;
    private final Cell frontCell;
    private final String blockName;
    private final Optional<PortBlock> portBlock;
    private EditBox name;
    private String message = "";
    private boolean messageIsError;
    private int left;
    private int top;

    /**
     * @param blockCell the looked-at block (ports and keep marks)
     * @param frontCell the cell in front of the looked-at face (empty marks)
     */
    public CellMenuScreen(ClientState state, Cell blockCell, Cell frontCell, String blockName,
            Optional<PortBlock> portBlock) {
        super(Component.literal("Redstone Solver block"));
        this.state = state;
        this.blockCell = blockCell;
        this.frontCell = frontCell;
        this.blockName = blockName;
        this.portBlock = portBlock;
    }

    @Override
    protected void init() {
        this.left = (this.width - PANEL_WIDTH) / 2;
        this.top = (this.height - PANEL_HEIGHT) / 2;
        int x = this.left + PADDING;
        int y = this.top + PADDING + 2 * LINE + GAP;

        this.name = this.addRenderableWidget(new EditBox(this.font, x + 34, y, NAME_WIDTH, ROW_HEIGHT,
                Component.literal("Port name")));
        this.name.setMaxLength(MAX_NAME_LENGTH);
        this.name.setValue(this.defaultName());
        this.name.setTooltip(Tooltip.create(Component.literal("The port's name, used in the truth table")));
        int buttonX = x + 34 + NAME_WIDTH + GAP;
        int buttonWidth = (this.left + PANEL_WIDTH - PADDING - buttonX - GAP) / 2;
        Button input = this.addRenderableWidget(Button.builder(Component.literal("Make input"), b -> this.makePort(true))
                .bounds(buttonX, y, buttonWidth, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The circuit reads its input from this block")))
                .build());
        Button output = this.addRenderableWidget(Button.builder(Component.literal("Make output"), b -> this.makePort(false))
                .bounds(buttonX + buttonWidth + GAP, y, buttonWidth, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The circuit's result is read at this block")))
                .build());
        input.active = this.portBlock.flatMap(PortBlock::input).isPresent();
        output.active = this.portBlock.flatMap(PortBlock::output).isPresent();
        this.name.active = input.active || output.active;

        y += ROW_HEIGHT + 2 * GAP + LINE;
        int third = (PANEL_WIDTH - 2 * PADDING - 2 * GAP) / 3;
        this.addRenderableWidget(Button.builder(Component.literal("Keep block"), b -> this.keep())
                .bounds(x, y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The solver must keep this " + this.blockName + " exactly as it is")))
                .build());
        this.addRenderableWidget(Button.builder(Component.literal("Front: empty"), b -> this.markEmpty())
                .bounds(x + third + GAP, y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("The solver may not put anything in the cell in front of the face you look at ("
                        + this.frontCell + ")")))
                .build());
        this.addRenderableWidget(Button.builder(Component.literal("Clear"), b -> this.clear())
                .bounds(x + 2 * (third + GAP), y, third, ROW_HEIGHT)
                .tooltip(Tooltip.create(Component.literal("Remove the port or keep mark of this block and the empty mark in front of it")))
                .build());

        this.addRenderableWidget(Button.builder(Component.literal("Done"), b -> this.onClose())
                .bounds(this.left + PANEL_WIDTH - PADDING - 60, this.top + PANEL_HEIGHT - PADDING - ROW_HEIGHT, 60, ROW_HEIGHT)
                .build());
        if (this.name.active) this.setInitialFocus(this.name);
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        graphics.fill(this.left, this.top, this.left + PANEL_WIDTH, this.top + PANEL_HEIGHT, PANEL_COLOR);
        graphics.outline(this.left, this.top, PANEL_WIDTH, PANEL_HEIGHT, BORDER_COLOR);
        int x = this.left + PADDING;
        int y = this.top + PADDING;
        String what = this.portBlock.map(PortBlock::describe).orElse(this.blockName);
        graphics.text(this.font, "Block " + this.blockCell + ": " + what + this.markText(), x, y, TITLE_COLOR);
        graphics.text(this.font, this.portBlock.isPresent()
                ? "Its kind and facing become the port's."
                : "For a port, place a repeater, comparator, dust or barrel.", x, y + LINE, HINT_COLOR);
        int rowY = y + 2 * LINE + GAP;
        graphics.text(this.font, "Name", x, rowY + 6, TEXT_COLOR);
        graphics.text(this.font, "Marks", x, rowY + ROW_HEIGHT + 2 * GAP + 1, TEXT_COLOR);
        if (!this.message.isEmpty()) {
            int messageY = rowY + 2 * ROW_HEIGHT + 3 * GAP + LINE + 4;
            graphics.textWithWordWrap(this.font, Component.literal(this.message), x, messageY,
                    PANEL_WIDTH - 2 * PADDING - 70, this.messageIsError ? ERROR_COLOR : TEXT_COLOR);
        }
        super.extractRenderState(graphics, mouseX, mouseY, partialTick);
    }

    @Override
    public boolean isPauseScreen() {
        return false;
    }

    private void makePort(boolean input) {
        String portName = this.name.getValue().trim();
        if (!portName.matches(CircuitDraft.PORT_NAME_PATTERN)) {
            this.show("Names use letters, digits and _ only.", true);
            return;
        }
        this.apply(() -> {
            Port port = this.portBlock.orElseThrow().toPort(portName, input, this.blockCell);
            CircuitDraft draft = this.state.getDraft();
            Optional<Port> old = draft.portAt(this.blockCell);
            if (old.isPresent() && !old.get().name().equals(portName)) draft.removePort(old.get().name());
            draft.putPort(port);
            return (input ? "Input " : "Output ") + port.describe();
        });
    }

    private void keep() {
        this.apply(() -> {
            this.state.getDraft().markKept(this.blockCell);
            return "The solver keeps this " + this.blockName + ".";
        });
    }

    private void markEmpty() {
        this.apply(() -> {
            this.state.getDraft().markEmpty(this.frontCell);
            return "Cell " + this.frontCell + " must stay empty.";
        });
    }

    private void clear() {
        boolean changed = this.state.getDraft().clearCell(this.blockCell) | this.state.getDraft().clearCell(this.frontCell);
        this.show(changed ? "Cleared." : "Nothing to clear here.", false);
        this.name.setValue(this.defaultName());
    }

    private void apply(Supplier<String> change) {
        try {
            this.show(change.get(), false);
            this.name.setValue(this.defaultName());
        } catch (IllegalArgumentException | IllegalStateException e) {
            this.show(e.getMessage(), true);
        }
    }

    private void show(String text, boolean error) {
        this.message = text;
        this.messageIsError = error;
    }

    private String defaultName() {
        CircuitDraft draft = this.state.getDraft();
        Optional<Port> existing = draft.portAt(this.blockCell);
        if (existing.isPresent()) return existing.get().name();
        boolean onlyOutput = this.portBlock.isPresent() && this.portBlock.get().input().isEmpty();
        return draft.nextFreeName(onlyOutput ? "OUT" : "IN");
    }

    private String markText() {
        CircuitDraft draft = this.state.getDraft();
        if (draft.getBox().isEmpty()) return " (set the box first: J and K)";
        if (!draft.getBox().get().contains(this.blockCell)) return " (outside the box)";
        Optional<Port> port = draft.portAt(this.blockCell);
        if (port.isPresent()) return " [" + port.get().name() + "]";
        if (draft.getKeptCells().contains(this.blockCell)) return " [kept]";
        return "";
    }
}
