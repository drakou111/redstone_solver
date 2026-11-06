package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import org.junit.jupiter.api.Test;

class DurationsTest {
    @Test
    void readsSecondsMinutesAndHours() {
        assertThat(Durations.parseSeconds("90")).isEqualTo(90);
        assertThat(Durations.parseSeconds("90s")).isEqualTo(90);
        assertThat(Durations.parseSeconds(" 10m ")).isEqualTo(600);
        assertThat(Durations.parseSeconds("1.5H")).isEqualTo(5400);
        assertThatThrownBy(() -> Durations.parseSeconds("0")).hasMessageContaining("more than zero");
        assertThatThrownBy(() -> Durations.parseSeconds("soon")).hasMessageContaining("90s, 10m or 1h");
    }

    @Test
    void writesTheShortestExactForm() {
        assertThat(Durations.format(3600)).isEqualTo("1h");
        assertThat(Durations.format(120)).isEqualTo("2m");
        assertThat(Durations.format(90)).isEqualTo("90s");
        assertThat(Durations.format(2.5)).isEqualTo("2.5s");
    }
}
