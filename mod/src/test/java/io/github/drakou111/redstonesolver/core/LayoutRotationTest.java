package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;

import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;

class LayoutRotationTest {
    private static Layout.Block repeater(Cell at, Dir facing) {
        return new Layout.Block(at, "repeater", Optional.of(facing), 2, "compare", 0, "", 0);
    }

    @Test
    void aQuarterTurnMovesBlocksAndTurnsFacings() {
        // A 3x1x2 layout: a repeater at the west end pointing east.
        Layout layout = new Layout(new Cell(3, 1, 2), List.of(repeater(new Cell(0, 0, 0), Dir.EAST)));
        Layout turned = layout.rotated(1);
        assertThat(turned.size()).isEqualTo(new Cell(2, 1, 3));
        Layout.Block block = turned.blocks().getFirst();
        assertThat(block.position()).isEqualTo(new Cell(1, 0, 0));
        assertThat(block.facing()).contains(Dir.SOUTH);
        assertThat(block.delay()).isEqualTo(2);
    }

    @Test
    void fourTurnsAreTheSameLayoutAndNegativeTurnsGoBack() {
        Layout layout = new Layout(new Cell(3, 2, 2), List.of(repeater(new Cell(2, 1, 1), Dir.NORTH),
                new Layout.Block(new Cell(1, 0, 0), "solid", Optional.empty(), 1, "compare", 0, "", 0)));
        assertThat(layout.rotated(4)).isEqualTo(layout);
        assertThat(layout.rotated(-1)).isEqualTo(layout.rotated(3));
        assertThat(layout.rotated(1).rotated(3)).isEqualTo(layout);
    }
}
