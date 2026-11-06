package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import org.junit.jupiter.api.Test;

class SignalRangeTest {
    @Test
    void parsesSingleValuesRangesAndAny() {
        assertThat(SignalRange.parse("5")).isEqualTo(new SignalRange(5, 5));
        assertThat(SignalRange.parse("1..15")).isEqualTo(new SignalRange(1, 15));
        assertThat(SignalRange.parse("2-4")).isEqualTo(new SignalRange(2, 4));
        assertThat(SignalRange.parse("ss3..7")).isEqualTo(new SignalRange(3, 7));
        assertThat(SignalRange.parse("*")).isEqualTo(SignalRange.ANY);
        assertThat(SignalRange.parse(" ")).isEqualTo(SignalRange.ANY);
    }

    @Test
    void rejectsRangesOutsideZeroToFifteen() {
        assertThatThrownBy(() -> SignalRange.parse("16")).isInstanceOf(IllegalArgumentException.class);
        assertThatThrownBy(() -> SignalRange.parse("5..2")).isInstanceOf(IllegalArgumentException.class);
        assertThatThrownBy(() -> SignalRange.parse("on")).hasMessageContaining("not a signal range");
    }

    @Test
    void formatsLikeItParses() {
        assertThat(new SignalRange(1, 15)).hasToString("1..15");
        assertThat(new SignalRange(0, 0)).hasToString("0");
        assertThat(SignalRange.ANY).hasToString("*");
    }
}
