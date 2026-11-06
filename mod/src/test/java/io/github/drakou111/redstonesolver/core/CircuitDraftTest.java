package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.google.gson.JsonObject;
import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;

class CircuitDraftTest {
    /** A 4x2x1 box at world (10, 64, -5) with a comparator input and a dust output: a buffer. */
    static CircuitDraft buffer() {
        CircuitDraft draft = new CircuitDraft();
        draft.setCorner1(new Cell(13, 65, -5));
        draft.setCorner2(new Cell(10, 64, -5));
        draft.putPort(Port.input("IN1", InputKind.COMPARATOR, new Cell(10, 65, -5), Dir.EAST));
        draft.putPort(Port.output("OUT1", OutputKind.DUST, new Cell(13, 65, -5), Dir.NORTH));
        draft.addRow(TruthRow.parse("IN1=1..15 -> OUT1=1..15"));
        draft.addRow(TruthRow.parse("IN1=0 -> OUT1=0"));
        return draft;
    }

    @Test
    void rowsKeepPortNamesAndFillInAnyValue() {
        TruthRow row = TruthRow.parse("IN2=0, IN1=1..15 -> OUT1=*");
        assertThat(row.rangesFor(List.of("IN1", "IN2", "IN3"), true))
                .containsExactly(new SignalRange(1, 15), new SignalRange(0, 0), SignalRange.ANY);
        assertThat(row).hasToString("IN2=0 IN1=1..15 -> OUT1=*");
        assertThatThrownBy(() -> TruthRow.parse("IN1=0")).hasMessageContaining("->");
        assertThatThrownBy(() -> TruthRow.parse("IN1=0 -> ")).hasMessageContaining("output");
        assertThatThrownBy(() -> TruthRow.parse("IN1=0 IN1=1 -> OUT1=0")).hasMessageContaining("twice");
    }

    @Test
    void specIsRelativeToTheLowestCorner() {
        JsonObject spec = buffer().toSpecJson();

        assertThat(spec.get("size").toString()).isEqualTo("[4,2,1]");
        JsonObject input = spec.getAsJsonArray("inputs").get(0).getAsJsonObject();
        assertThat(input.get("kind").getAsString()).isEqualTo("comparator");
        assertThat(input.get("position").toString()).isEqualTo("[0,1,0]");
        assertThat(input.get("facing").getAsString()).isEqualTo("east");
        assertThat(spec.getAsJsonArray("outputs").get(0).getAsJsonObject().get("position").toString())
                .isEqualTo("[3,1,0]");
        assertThat(spec.getAsJsonArray("rows").get(0).toString()).isEqualTo("{\"inputs\":[[1,15]],\"outputs\":[[1,15]]}");
        assertThat(spec.getAsJsonObject("timing").get("latest").isJsonNull()).isTrue();
    }

    @Test
    void timingSetsTheLatestTick() {
        CircuitDraft draft = buffer();
        draft.setLatestTicks(Optional.of(4));
        assertThat(draft.toSpecJson().getAsJsonObject("timing").get("latest").getAsInt()).isEqualTo(4);
        assertThatThrownBy(() -> draft.setLatestTicks(Optional.of(-1))).isInstanceOf(IllegalArgumentException.class);
    }

    @Test
    void portsMustBeInsideTheBoxAndNotShareACell() {
        CircuitDraft draft = buffer();
        assertThatThrownBy(() -> draft.putPort(Port.input("IN2", InputKind.DUST, new Cell(14, 65, -5), Dir.EAST)))
                .hasMessageContaining("outside the box");
        assertThatThrownBy(() -> draft.putPort(Port.input("IN2", InputKind.DUST, new Cell(10, 65, -5), Dir.EAST)))
                .hasMessageContaining("IN1 is already at");
        assertThatThrownBy(() -> draft.putPort(Port.input("OUT1", InputKind.DUST, new Cell(11, 65, -5), Dir.EAST)))
                .hasMessageContaining("already the name of an output");
    }

    @Test
    void rowsMayOnlyNameExistingPorts() {
        CircuitDraft draft = buffer();
        assertThatThrownBy(() -> draft.addRow(TruthRow.parse("IN9=0 -> OUT1=0"))).hasMessageContaining("IN9");
        assertThatThrownBy(() -> draft.addRow(TruthRow.parse("IN1=0 -> OUT9=0"))).hasMessageContaining("OUT9");
    }

    @Test
    void incompleteDraftsExplainWhatIsMissing() {
        CircuitDraft draft = new CircuitDraft();
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("Select the box");
        draft.setCorner1(new Cell(0, 0, 0));
        draft.setCorner2(new Cell(3, 1, 0));
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("/rs input");
    }
}
