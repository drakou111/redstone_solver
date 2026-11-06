package io.github.drakou111.redstonesolver.client;

import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.AbstractButton;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.narration.NarrationElementOutput;
import net.minecraft.client.input.InputWithModifiers;
import net.minecraft.network.chat.Component;
import net.minecraft.world.item.ItemStack;

/** An inventory-style slot showing a block the solver may use; clicking allows or forbids it. */
final class BlockToggle extends AbstractButton {
    static final int SIZE = 22;
    private static final int ICON_INSET = 3;
    private static final int ALLOWED_BACKGROUND = 0xFF2E3B2E;
    private static final int FORBIDDEN_BACKGROUND = 0xFF2A1616;
    private static final int ALLOWED_BORDER = 0xFF5FBF5F;
    private static final int FORBIDDEN_BORDER = 0xFF8B2A2A;
    private static final int HOVER_BORDER = 0xFFFFFFFF;
    private static final int DIM = 0xB0101010;
    private static final int CROSS_COLOR = 0xFFFF5555;

    private final ItemStack icon;
    private final boolean allowed;
    private final Runnable action;
    private final Font font;

    BlockToggle(int x, int y, Font font, String name, ItemStack icon, boolean allowed, Runnable action) {
        super(x, y, SIZE, SIZE, Component.literal(name));
        this.font = font;
        this.icon = icon;
        this.allowed = allowed;
        this.action = action;
        this.setTooltip(Tooltip.create(Component.literal(name + (allowed
                ? ": the solver may use it (click to forbid)"
                : ": the solver may not use it (click to allow)"))));
    }

    @Override
    public void onPress(InputWithModifiers input) {
        this.action.run();
    }

    @Override
    protected void extractContents(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        int x = this.getX();
        int y = this.getY();
        graphics.fill(x, y, x + SIZE, y + SIZE, this.allowed ? ALLOWED_BACKGROUND : FORBIDDEN_BACKGROUND);
        graphics.outline(x, y, SIZE, SIZE, this.isHoveredOrFocused() ? HOVER_BORDER : this.allowed ? ALLOWED_BORDER : FORBIDDEN_BORDER);
        graphics.item(this.icon, x + ICON_INSET, y + ICON_INSET);
        if (this.allowed) return;
        // Above the item: dim it and cross it out.
        graphics.nextStratum();
        graphics.fill(x + 1, y + 1, x + SIZE - 1, y + SIZE - 1, DIM);
        graphics.centeredText(this.font, "X", x + SIZE / 2, y + (SIZE - this.font.lineHeight) / 2 + 1, CROSS_COLOR);
    }

    @Override
    protected void updateWidgetNarration(NarrationElementOutput output) {
        this.defaultButtonNarrationText(output);
    }
}
