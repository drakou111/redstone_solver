package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;

import java.util.List;
import org.junit.jupiter.api.Test;

class ContainerFillTest {
    private static final int HOPPER_SLOTS = 5;
    private static final int BARREL_SLOTS = 27;

    @Test
    void everySignalIsReachedExactly() {
        for (int slots : new int[] {HOPPER_SLOTS, BARREL_SLOTS}) {
            for (int signal = 0; signal <= SignalRange.MAX_SIGNAL; signal++) {
                List<Integer> stacks = ContainerFill.stacksForSignal(signal, slots);
                int items = stacks.stream().mapToInt(Integer::intValue).sum();
                assertThat(stacks.size()).isLessThanOrEqualTo(slots);
                assertThat(ContainerFill.signalOf(items, slots)).as("signal %d in %d slots", signal, slots).isEqualTo(signal);
            }
        }
    }

    @Test
    void usesTheFewestItems() {
        assertThat(ContainerFill.stacksForSignal(1, BARREL_SLOTS)).containsExactly(1);
        assertThat(ContainerFill.stacksForSignal(15, HOPPER_SLOTS)).containsExactly(64, 64, 64, 64, 64);
        List<Integer> two = ContainerFill.stacksForSignal(2, HOPPER_SLOTS);
        int items = two.stream().mapToInt(Integer::intValue).sum();
        assertThat(ContainerFill.signalOf(items - 1, HOPPER_SLOTS)).isEqualTo(1);
    }
}
