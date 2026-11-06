package io.github.drakou111.redstonesolver;

import net.minecraft.ChatFormatting;
import net.minecraft.network.chat.ClickEvent;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.HoverEvent;
import net.minecraft.network.chat.MutableComponent;
import net.minecraft.network.chat.Style;

/** Chat text in the mod's few styles. */
public final class Chat {
    private static final String PREFIX = "[Solver] ";

    private Chat() {
    }

    public static MutableComponent info(String text) {
        return Component.literal(PREFIX).withStyle(ChatFormatting.DARK_RED).append(Component.literal(text)
                .withStyle(ChatFormatting.WHITE));
    }

    public static MutableComponent good(String text) {
        return Component.literal(PREFIX).withStyle(ChatFormatting.DARK_RED).append(Component.literal(text)
                .withStyle(ChatFormatting.GREEN));
    }

    public static MutableComponent error(String text) {
        return Component.literal(PREFIX).withStyle(ChatFormatting.DARK_RED).append(Component.literal(text)
                .withStyle(ChatFormatting.RED));
    }

    public static MutableComponent hint(String text) {
        return Component.literal(text).withStyle(ChatFormatting.GRAY);
    }

    /** A clickable [label] that runs {@code command}. */
    public static MutableComponent button(String label, String command, String tooltip) {
        return Component.literal("[" + label + "]").withStyle(Style.EMPTY.withColor(ChatFormatting.AQUA)
                .withClickEvent(new ClickEvent.RunCommand(command))
                .withHoverEvent(new HoverEvent.ShowText(Component.literal(tooltip))));
    }
}
