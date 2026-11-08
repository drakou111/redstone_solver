package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import org.junit.jupiter.api.Test;

class ProgressLineTest {
    @Test
    void keepsOnlyTheFirstLine() {
        assertThat(ProgressLine.of("gate network:\ng1 = XOR(IN1, IN2)\nOUT1 = g1\n")).isEqualTo("gate network:");
        assertThat(ProgressLine.of("\r\n  flush: 9x2x6\r\nmore")).isEqualTo("flush: 9x2x6");
    }

    @Test
    void cutsLongLines() {
        String line = ProgressLine.of("x".repeat(200), 20);
        assertThat(line).hasSize(20).endsWith("...");
        assertThat(ProgressLine.of("short", 20)).isEqualTo("short");
    }

    @Test
    void emptyTextStaysEmpty() {
        assertThat(ProgressLine.of("\n\n")).isEmpty();
        assertThatThrownBy(() -> ProgressLine.of("text", 3)).isInstanceOf(IllegalArgumentException.class);
    }
}
