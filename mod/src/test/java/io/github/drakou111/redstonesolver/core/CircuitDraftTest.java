package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.google.gson.JsonArray;
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
    void loopsAreOffUntilAllowedAndSavedWithTheDraft() {
        CircuitDraft draft = buffer();
        assertThat(draft.toSpecJson().get("feedback").getAsBoolean()).isFalse();

        draft.setAllowLoops(true);
        assertThat(draft.toSpecJson().get("feedback").getAsBoolean()).isTrue();
        assertThat(CircuitDraft.fromJson(draft.toJson()).isAllowLoops()).isTrue();

        JsonObject older = draft.toJson();
        older.remove("allowLoops");
        assertThat(CircuitDraft.fromJson(older).isAllowLoops()).isFalse();
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
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("Set the box");
        draft.setCorner1(new Cell(0, 0, 0));
        draft.setCorner2(new Cell(3, 1, 0));
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("Add an input");
    }

    @Test
    void keptCellsAreReadFromTheWorldAndEmptyCellsForbidden() {
        CircuitDraft draft = buffer();
        draft.markKept(new Cell(11, 64, -5));
        draft.markEmpty(new Cell(12, 65, -5));
        JsonObject solid = new JsonObject();
        solid.addProperty("type", "solid");

        JsonObject spec = draft.toSpecJson(cell -> cell.equals(new Cell(11, 64, -5)) ? Optional.of(solid) : Optional.empty());

        assertThat(spec.getAsJsonArray("fixed").toString()).isEqualTo("[{\"type\":\"solid\",\"position\":[1,0,0]}]");
        assertThat(spec.getAsJsonArray("forbidden").toString()).isEqualTo("[[2,1,0]]");
        assertThat(spec.getAsJsonArray("hints")).isEmpty();
        assertThatThrownBy(() -> draft.toSpecJson(cell -> Optional.empty())).hasMessageContaining("is empty");
    }

    @Test
    void sketchModeTurnsOtherBlocksIntoHints() {
        CircuitDraft draft = buffer();
        draft.markKept(new Cell(11, 64, -5));
        draft.setOtherBlocks(CircuitDraft.OtherBlocks.SKETCH);
        JsonObject dust = new JsonObject();
        dust.addProperty("type", "dust");

        JsonObject spec = draft.toSpecJson(cell -> Optional.of(dust));

        // 8 cells minus the two ports and the kept cell.
        assertThat(spec.getAsJsonArray("hints")).hasSize(5);
        assertThat(spec.getAsJsonArray("fixed")).hasSize(1);
    }

    @Test
    void marksReplaceEachOtherAndClearRemovesThem() {
        CircuitDraft draft = buffer();
        Cell cell = new Cell(11, 65, -5);
        draft.markKept(cell);
        draft.markEmpty(cell);
        assertThat(draft.getKeptCells()).isEmpty();
        assertThat(draft.getEmptyCells()).containsExactly(cell);
        assertThat(draft.clearCell(cell)).isTrue();
        assertThat(draft.clearCell(new Cell(10, 65, -5))).isTrue();
        assertThat(draft.getInputs()).isEmpty();
        assertThatThrownBy(() -> draft.markKept(new Cell(13, 65, -5))).hasMessageContaining("OUT1 is in that cell");
        assertThat(draft.nextFreeName("IN")).isEqualTo("IN1");
        assertThat(draft.nextFreeName("OUT")).isEqualTo("OUT2");
    }

    @Test
    void renamingAPortUpdatesTheTruthTable() {
        CircuitDraft draft = buffer();
        draft.renamePort("IN1", "A");
        assertThat(draft.getInputs().getFirst().name()).isEqualTo("A");
        assertThat(draft.getRows().getFirst()).hasToString("A=1..15 -> OUT1=1..15");
        assertThatThrownBy(() -> draft.renamePort("A", "OUT1")).hasMessageContaining("already used");
        assertThatThrownBy(() -> draft.renamePort("A", "a b")).hasMessageContaining("letters");
    }

    @Test
    void editingOneRangeOfARow() {
        TruthRow row = TruthRow.parse("IN1=0 -> OUT1=0");
        assertThat(row.withRange("IN1", true, SignalRange.ANY)).hasToString(" -> OUT1=0");
        assertThat(row.withRange("OUT1", false, new SignalRange(1, 15))).hasToString("IN1=0 -> OUT1=1..15");
        assertThatThrownBy(() -> row.withRange("OUT1", false, SignalRange.ANY)).hasMessageContaining("output");
    }

    @Test
    void adoptingAResultMovesTheBoxAndPorts() {
        CircuitDraft draft = buffer();
        draft.markEmpty(new Cell(12, 65, -5));
        JsonObject spec = draft.toSpecJson();
        spec.add("size", new Cell(6, 3, 2).toJson());
        JsonObject output = spec.getAsJsonArray("outputs").get(0).getAsJsonObject();
        output.add("position", new Cell(5, 1, 1).toJson());
        output.addProperty("kind", "repeater");
        output.addProperty("facing", "east");

        draft.adoptSpec(spec, new Cell(10, 64, -5), false);

        assertThat(draft.requireBox().size()).isEqualTo(new Cell(6, 3, 2));
        Port out = draft.getOutputs().getFirst();
        assertThat(out.name()).isEqualTo("OUT1");
        assertThat(out.position()).isEqualTo(new Cell(15, 65, -4));
        assertThat(out.kind()).isEqualTo("repeater");
        assertThat(draft.getEmptyCells()).isEmpty();
        assertThat(draft.getRows()).hasSize(2);
    }

    private static JsonObject solverBlock(String type, String facing) {
        JsonObject block = new JsonObject();
        block.addProperty("type", type);
        if (facing != null) block.addProperty("facing", facing);
        return block;
    }

    @Test
    void placedBlocksDecideWhatAPortCanBe() {
        PortBlock repeater = PortBlock.fromSolverBlock(solverBlock("repeater", "west")).orElseThrow();
        Port input = repeater.toPort("IN1", true, new Cell(1, 1, 1));
        assertThat(input.kind()).isEqualTo("repeater");
        assertThat(input.facing()).isEqualTo(Dir.WEST);
        assertThat(repeater.describe()).isEqualTo("repeater, signal going west");

        PortBlock barrel = PortBlock.fromSolverBlock(solverBlock("barrel", null)).orElseThrow();
        assertThat(barrel.toPort("IN1", true, new Cell(0, 0, 0)).kind()).isEqualTo("container");
        assertThatThrownBy(() -> barrel.toPort("OUT1", false, new Cell(0, 0, 0))).hasMessageContaining("cannot be an output");
        assertThat(PortBlock.fromSolverBlock(solverBlock("solid", null))).isEmpty();
    }

    @Test
    void portsFollowTheBlocksInTheirCells() {
        CircuitDraft draft = buffer();
        Cell in = new Cell(10, 65, -5);
        List<String> stale = draft.refreshPorts(cell -> cell.equals(in)
                ? PortBlock.fromSolverBlock(solverBlock("repeater", "south"))
                : Optional.empty());

        assertThat(stale).containsExactly("OUT1");
        Port port = draft.getInputs().getFirst();
        assertThat(port.kind()).isEqualTo("repeater");
        assertThat(port.facing()).isEqualTo(Dir.SOUTH);
        assertThat(draft.getOutputs().getFirst().kind()).isEqualTo("dust");
    }

    @Test
    void aSavedDraftReadsBackTheSame() {
        CircuitDraft draft = buffer();
        draft.markKept(new Cell(11, 64, -5));
        draft.markEmpty(new Cell(12, 65, -5));
        draft.setLatestTicks(Optional.of(6));
        draft.setOtherBlocks(CircuitDraft.OtherBlocks.SKETCH);
        draft.addRow(TruthRow.parse(" -> OUT1=3"));

        CircuitDraft loaded = CircuitDraft.fromJson(draft.toJson());

        assertThat(loaded.toJson()).isEqualTo(draft.toJson());
        assertThat(loaded.getRows()).hasSize(3);
        assertThat(loaded.getLatestTicks()).contains(6);
        assertThat(loaded.getKeptCells()).containsExactly(new Cell(11, 64, -5));
    }

    @Test
    void brokenSavesAreRejectedWithAReason() {
        JsonObject json = buffer().toJson();
        json.addProperty("version", 99);
        assertThatThrownBy(() -> CircuitDraft.fromJson(json)).hasMessageContaining("version 99");
        JsonObject missing = buffer().toJson();
        missing.remove("rows");
        assertThatThrownBy(() -> CircuitDraft.fromJson(missing)).hasMessageContaining("not a valid draft");
    }

    @Test
    void aBuildInTheBoxBecomesALayoutWithPortBlocks() {
        CircuitDraft draft = buffer();
        JsonObject solid = new JsonObject();
        solid.addProperty("type", "solid");

        JsonObject layout = draft.buildLayoutJson(cell -> cell.y() == 64 ? Optional.of(solid) : Optional.empty());

        assertThat(layout.get("size").toString()).isEqualTo("[4,2,1]");
        assertThat(layout.getAsJsonArray("blocks")).hasSize(6);
        assertThat(layout.getAsJsonArray("blocks").toString())
                .contains("{\"type\":\"input\",\"kind\":\"comparator\",\"channel\":0,\"facing\":\"east\",\"position\":[0,1,0]}")
                .contains("{\"type\":\"output\",\"kind\":\"dust\",\"channel\":0,\"facing\":\"north\",\"position\":[3,1,0]}");
    }

    @Test
    void theSolverOnlyGetsTheAllowedBlocks() {
        CircuitDraft draft = buffer();
        draft.setAllowed("target", false);
        draft.setAllowed("chest", false);
        assertThat(draft.toSpecJson().getAsJsonArray("palette").toString()).doesNotContain("target", "chest", "observer", "note_block", "hopper")
                .contains("repeater");
        assertThat(CircuitDraft.fromJson(draft.toJson()).getPalette()).isEqualTo(draft.getPalette());

        for (String block : CircuitDraft.DEFAULT_PALETTE) {
            if (!block.equals("dust")) draft.setAllowed(block, false);
        }
        assertThatThrownBy(() -> draft.setAllowed("dust", false)).hasMessageContaining("at least one");
        assertThatThrownBy(() -> draft.setAllowed("lamp", true)).hasMessageContaining("unknown block");
    }

    @Test
    void aDelayHoldsTheOldOutputUntilTheEarliestTick() {
        CircuitDraft draft = buffer();
        draft.setEarliestTicks(Optional.of(4));
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("needs a deadline");
        draft.setLatestTicks(Optional.of(2));
        assertThatThrownBy(draft::toSpecJson).hasMessageContaining("must not be after");
        draft.setLatestTicks(Optional.of(4));

        JsonObject timing = draft.toSpecJson().getAsJsonObject("timing");
        assertThat(timing.get("earliest").getAsInt()).isEqualTo(4);
        assertThat(timing.get("latest").getAsInt()).isEqualTo(4);
        assertThat(timing.get("holdPrevious").getAsBoolean()).isTrue();
        assertThat(CircuitDraft.fromJson(draft.toJson()).getEarliestTicks()).contains(4);
    }

    /** A flat box one block high: IN1 dust at the west end, OUT1 dust at the east end. */
    private static CircuitDraft flat() {
        CircuitDraft draft = new CircuitDraft();
        draft.setCorner1(new Cell(0, 70, 0));
        draft.setCorner2(new Cell(3, 70, 1));
        draft.putPort(Port.input("IN1", InputKind.DUST, new Cell(0, 70, 0), Dir.NORTH));
        draft.putPort(Port.output("OUT1", OutputKind.DUST, new Cell(3, 70, 0), Dir.NORTH));
        draft.addRow(TruthRow.parse("IN1=1..15 -> OUT1=1..15"));
        draft.addRow(TruthRow.parse("IN1=0 -> OUT1=0"));
        return draft;
    }

    @Test
    void aOneHighBoxStandsOnAnAssumedFloor() {
        CircuitDraft draft = flat();
        assertThat(draft.assumesFloor()).isTrue();
        assertThat(draft.specOrigin()).isEqualTo(new Cell(0, 69, 0));

        JsonObject spec = draft.toSpecJson();
        assertThat(spec.get("size").toString()).isEqualTo("[4,2,2]");
        assertThat(spec.getAsJsonArray("inputs").get(0).getAsJsonObject().get("position").toString()).isEqualTo("[0,1,0]");
        assertThat(spec.getAsJsonArray("fixed")).hasSize(8);
        assertThat(spec.getAsJsonArray("fixed").toString()).contains("{\"type\":\"solid\",\"position\":[3,0,1]}");
        assertThat(draft.buildLayoutJson(cell -> Optional.empty()).getAsJsonArray("blocks")).hasSize(10);

        draft.setCorner2(new Cell(3, 71, 1));
        assertThat(draft.assumesFloor()).isFalse();
        assertThat(draft.toSpecJson().getAsJsonArray("fixed")).isEmpty();
    }

    @Test
    void adoptingAFloorResultKeepsTheBoxOneHigh() {
        CircuitDraft draft = flat();
        JsonObject spec = draft.toSpecJson();
        spec.add("size", new Cell(3, 2, 2).toJson());
        draft.adoptSpec(spec, draft.specOrigin(), true);
        assertThat(draft.requireBox()).isEqualTo(new Box(new Cell(0, 70, 0), new Cell(2, 70, 1)));
        assertThat(draft.getInputs().getFirst().position()).isEqualTo(new Cell(0, 70, 0));
    }

    @Test
    void portsMayMoveAlongTheirSideAtTheirHeight() {
        JsonObject spec = new JsonObject();
        spec.add("size", new Cell(5, 2, 4).toJson());
        JsonArray inputs = new JsonArray();
        JsonArray outputs = new JsonArray();
        for (Cell cell : List.of(new Cell(0, 1, 2), new Cell(2, 1, 1))) {
            JsonObject port = new JsonObject();
            port.add("position", cell.toJson());
            port.add("region", new JsonArray());
            inputs.add(port);
        }
        JsonObject output = new JsonObject();
        output.add("position", new Cell(2, 1, 3).toJson());
        output.add("region", new JsonArray());
        outputs.add(output);
        spec.add("inputs", inputs);
        spec.add("outputs", outputs);

        CircuitDraft.freePortsAlongTheirSide(spec);

        // West side: every z at height 1.
        assertThat(spec.getAsJsonArray("inputs").get(0).getAsJsonObject().getAsJsonArray("region"))
                .containsExactly(new Cell(0, 1, 0).toJson(), new Cell(0, 1, 1).toJson(), new Cell(0, 1, 2).toJson(),
                        new Cell(0, 1, 3).toJson());
        // Inside the box: stays fixed.
        assertThat(spec.getAsJsonArray("inputs").get(1).getAsJsonObject().getAsJsonArray("region")).isEmpty();
        // South side: every x at height 1.
        assertThat(spec.getAsJsonArray("outputs").get(0).getAsJsonObject().getAsJsonArray("region")).hasSize(5);
    }
}
