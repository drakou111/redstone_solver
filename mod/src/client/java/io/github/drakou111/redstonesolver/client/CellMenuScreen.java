package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import java.util.Optional;
import java.util.function.Supplier;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * The P menu for marks on the looked-at block: keep it, force the cell in front of it empty, or
 * clear marks. I and O set input and output ports directly.
 */
public final class CellMenuScreen extends Screen {
    private static final int PANEL_WIDTH = 300;
    private static final int PANEL_HEIGHT = 112;
    private static final int PADDING = 8;
    private static final int ROW_HEIGHT = 20;
    private static final int GAP = 4;
    private static final int LINE = 12;
    private static final int PANEL_COLOR = 0xE0101010;
    private static final int BORDER_COLOR = 0xFF8B1A1A;
    private static final int TITLE_COLOR = 0xFFFF5555;
    private static final int HINT_COLOR = 0xFFAAAAAA;
    private static final int ERROR_COLOR = 0xFFFF6060;

    private final ClientState state;
    private final Cell blockCell;
    private final Cell frontCell;
    private final String blockName;
    private String message = "";
    private boolean messageIsError;
    private int left;
    private int top;

    /**
     * @param blockCell the looked-at block (keep marks)
     * @param frontCell the cell in front of the looked-at face (empty marks)
     */
    public CellMenuScreen(ClientState state, Cell blockCell, Cell frontCell, String blockName) {
        super(Component.literal("Redstone Solver block"));
        this.state = state;
        this.blockCell = blockCell;
        this.frontCell = frontCell;
        this.blockName = blockName;
    }

    @Override
    protected void init() {
        this.left = (this.width - PANEL_WIDTH) / 2;
        this.top = (this.height - PANEL_HEIGHT) / 2;
        int x = this.left + PADDING;
        int y = this.top + PADDING + 2 * LINE + 2 * GAP;
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
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        graphics.fill(this.left, this.top, this.left + PANEL_WIDTH, this.top + PANEL_HEIGHT, PANEL_COLOR);
        graphics.outline(this.left, this.top, PANEL_WIDTH, PANEL_HEIGHT, BORDER_COLOR);
        int x = this.left + PADDING;
        int y = this.top + PADDING;
        graphics.text(this.font, "Block " + this.blockCell + ": " + this.blockName + this.markText(), x, y,
                TITLE_COLOR);
        graphics.text(this.font, "I: input  |  O: output  |  P: keep/empty/clear", x, y + LINE, HINT_COLOR);
        if (!this.message.isEmpty())
            graphics.textWithWordWrap(this.font, Component.literal(this.message), x, y + 2 * LINE + GAP,
                    PANEL_WIDTH - 2 * PADDING, this.messageIsError ? ERROR_COLOR : HINT_COLOR);
        super.extractRenderState(graphics, mouseX, mouseY, partialTick);
    }

    @Override
    public boolean isPauseScreen() {
        return false;
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
    }

    private void apply(Supplier<String> change) {
        try {
            this.show(change.get(), false);
        } catch (IllegalArgumentException | IllegalStateException e) {
            this.show(e.getMessage(), true);
        }
    }

    private void show(String text, boolean error) {
        this.message = text;
        this.messageIsError = error;
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
