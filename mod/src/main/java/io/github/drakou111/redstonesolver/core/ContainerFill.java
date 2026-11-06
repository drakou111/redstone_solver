package io.github.drakou111.redstonesolver.core;

import java.util.ArrayList;
import java.util.List;

/** How many items make a container give a comparator signal, computed like the game does. */
public final class ContainerFill {
    public static final int STACK_SIZE = 64;

    private ContainerFill() {
    }

    /** Stack sizes, one per slot from the first, that give exactly {@code signal}. */
    public static List<Integer> stacksForSignal(int signal, int slots) {
        if (signal < 0 || signal > SignalRange.MAX_SIGNAL)
            throw new IllegalArgumentException("signal must be 0..15: " + signal);
        if (slots <= 0) throw new IllegalArgumentException("a container needs slots");
        if (signal == 0) return List.of();

        int items = Math.max(1, Math.ceilDiv((signal - 1) * STACK_SIZE * slots, SignalRange.MAX_SIGNAL - 1));
        while (signalOf(items, slots) < signal) items++;
        if (signalOf(items, slots) != signal)
            throw new IllegalStateException("no item count gives signal %d in %d slots".formatted(signal, slots));

        List<Integer> stacks = new ArrayList<>();
        for (int left = items; left > 0; left -= STACK_SIZE) stacks.add(Math.min(STACK_SIZE, left));
        return stacks;
    }

    /** The comparator signal of a container holding {@code items} items of a 64-stack, filled slot by slot. */
    public static int signalOf(int items, int slots) {
        float fullness = 0;
        for (int left = items; left > 0; left -= STACK_SIZE)
            fullness += Math.min(STACK_SIZE, left) / (float) STACK_SIZE;
        fullness /= slots;
        int span = SignalRange.MAX_SIGNAL;
        return (int) Math.floor(fullness * (span - 1)) + (fullness > 0 ? 1 : 0);
    }
}
