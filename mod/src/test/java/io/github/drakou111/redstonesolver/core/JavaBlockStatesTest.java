package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.google.gson.JsonParser;
import java.util.Optional;
import org.junit.jupiter.api.Test;

class JavaBlockStatesTest {
    private static Layout.Block block(String type, Dir facing) {
        return new Layout.Block(new Cell(0, 0, 0), type, Optional.ofNullable(facing), 1, "compare", 0, "", 0);
    }

    @Test
    void diodesFaceTheirInputLikeJava() {
        assertThat(JavaBlockStates.stateFor(new Layout.Block(new Cell(0, 0, 0), "repeater", Optional.of(Dir.EAST), 3,
                "compare", 0, "", 0))).contains("minecraft:repeater[facing=west,delay=3]");
        assertThat(JavaBlockStates.stateFor(new Layout.Block(new Cell(0, 0, 0), "comparator", Optional.of(Dir.NORTH), 1,
                "subtract", 0, "", 0))).contains("minecraft:comparator[facing=south,mode=subtract]");
    }

    @Test
    void torchesStandOrHangOnAWall() {
        assertThat(JavaBlockStates.stateFor(block("torch", Dir.DOWN))).contains("minecraft:redstone_torch");
        assertThat(JavaBlockStates.stateFor(block("torch", Dir.WEST)))
                .contains("minecraft:redstone_wall_torch[facing=east]");
        assertThatThrownBy(() -> JavaBlockStates.stateFor(block("torch", Dir.UP)))
                .isInstanceOf(IllegalArgumentException.class);
    }

    @Test
    void portsBecomeTheirBlocks() {
        Layout.Block comparatorInput = new Layout.Block(new Cell(0, 1, 0), "input", Optional.of(Dir.EAST), 1, "compare",
                0, "comparator", 0);
        Layout.Block dustOutput = new Layout.Block(new Cell(3, 1, 0), "output", Optional.of(Dir.NORTH), 1, "compare", 0,
                "dust", 0);
        assertThat(JavaBlockStates.stateFor(comparatorInput)).contains("minecraft:comparator[facing=west,mode=compare]");
        assertThat(JavaBlockStates.stateFor(dustOutput)).contains(JavaBlockStates.DUST_CROSS);
        assertThat(JavaBlockStates.needsSupport(dustOutput)).isTrue();
        assertThat(JavaBlockStates.needsSupport(block("solid", null))).isFalse();
    }

    @Test
    void airIsSkippedAndUnknownBlocksFail() {
        assertThat(JavaBlockStates.stateFor(block("air", null))).isEmpty();
        assertThatThrownBy(() -> JavaBlockStates.stateFor(block("lamp", null))).hasMessageContaining("lamp");
    }

    @Test
    void layoutJsonFromTheSolverParses() {
        Layout layout = Layout.fromJson(JsonParser.parseString("""
                {"size":[4,2,1],"blocks":[
                  {"type":"solid","position":[1,0,0]},
                  {"type":"repeater","facing":"east","delay":2,"position":[1,1,0]},
                  {"type":"barrel","signal":7,"position":[2,1,0]},
                  {"type":"input","kind":"comparator","channel":0,"facing":"east","position":[0,1,0]}]}
                """).getAsJsonObject());

        assertThat(layout.size()).isEqualTo(new Cell(4, 2, 1));
        assertThat(layout.blocks()).hasSize(4);
        assertThat(layout.blocks().get(1).delay()).isEqualTo(2);
        assertThat(layout.blocks().get(2).signal()).isEqualTo(7);
        assertThat(JavaBlockStates.isContainer(layout.blocks().get(2))).isTrue();
        assertThat(layout.countPlacedBlocks()).isEqualTo(4);
    }
}
