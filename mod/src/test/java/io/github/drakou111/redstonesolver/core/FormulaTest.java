package io.github.drakou111.redstonesolver.core;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class FormulaTest {
    private static int eval(String text, Map<String, Integer> values) {
        return Formula.parse(text).evaluate(values);
    }

    @Test
    void arithmeticFollowsTheUsualPrecedence() {
        Map<String, Integer> values = Map.of("A", 7, "B", 12);
        assertThat(eval("A + B * 2", values)).isEqualTo(31);
        assertThat(eval("(A + B) % 16", values)).isEqualTo(3);
        assertThat(eval("B / 5 - -1", values)).isEqualTo(3);
        assertThat(eval("max(A, B) - min(A, B)", values)).isEqualTo(5);
        assertThat(eval("abs(A - B)", values)).isEqualTo(5);
        assertThat(eval("A > B ? A : B", values)).isEqualTo(12);
    }

    @Test
    void comparisonsAndLogicMeanOnOrOff() {
        assertThat(Formula.parse("A + B >= 16").isOnOff()).isTrue();
        assertThat(Formula.parse("A and not B").isOnOff()).isTrue();
        assertThat(Formula.parse("A && !B || C").isOnOff()).isTrue();
        assertThat(Formula.parse("A ? B > 1 : C == 0").isOnOff()).isTrue();
        assertThat(Formula.parse("A + B").isOnOff()).isFalse();

        Formula carry = Formula.parse("A + B >= 16");
        assertThat(carry.outputFor(Map.of("A", 15, "B", 1))).isEqualTo(new SignalRange(1, 15));
        assertThat(carry.outputFor(Map.of("A", 3, "B", 1))).isEqualTo(new SignalRange(0, 0));
        assertThat(Formula.parse("(A + B) % 16").outputFor(Map.of("A", 9, "B", 9))).isEqualTo(new SignalRange(2, 2));
    }

    @Test
    void mistakesAreExplained() {
        assertThatThrownBy(() -> Formula.parse("")).hasMessageContaining("empty");
        assertThatThrownBy(() -> Formula.parse("A +")).hasMessageContaining("ends too early");
        assertThatThrownBy(() -> Formula.parse("(A + B")).hasMessageContaining("missing ')'");
        assertThatThrownBy(() -> Formula.parse("A $ B")).hasMessageContaining("character 3");
        assertThatThrownBy(() -> Formula.parse("min(A)")).hasMessageContaining("takes 2 values");
        assertThatThrownBy(() -> Formula.parse("A B")).hasMessageContaining("unexpected 'B'");
        assertThatThrownBy(() -> Formula.parse("A + B").outputFor(Map.of("A", 15, "B", 15)))
                .hasMessageContaining("gives 30");
        assertThatThrownBy(() -> eval("A / B", Map.of("A", 1, "B", 0))).hasMessageContaining("divides by zero");
    }

    @Test
    void renamingTouchesWholeNamesOnly() {
        assertThat(Formula.renameVariable("A + AB * A", "A", "X")).isEqualTo("X + AB * X");
        assertThat(Formula.parse("max(IN1, IN2) and IN3").variables()).containsExactly("IN1", "IN2", "IN3");
    }

    /** A hex adder: two 0..15 inputs, the sum's low digit and a carry. */
    private static CircuitDraft hexAdder() {
        CircuitDraft draft = new CircuitDraft();
        draft.setCorner1(new Cell(0, 0, 0));
        draft.setCorner2(new Cell(9, 3, 9));
        draft.putPort(Port.input("A", InputKind.COMPARATOR, new Cell(0, 1, 0), Dir.EAST));
        draft.putPort(Port.input("B", InputKind.COMPARATOR, new Cell(0, 1, 9), Dir.EAST));
        draft.putPort(Port.output("SUM", OutputKind.DUST, new Cell(9, 1, 2), Dir.NORTH));
        draft.putPort(Port.output("CARRY", OutputKind.DUST, new Cell(9, 1, 7), Dir.NORTH));
        draft.setFormulaMode(true);
        draft.setFormula("SUM", "(A + B) % 16");
        draft.setFormula("CARRY", "A + B >= 16");
        draft.setAnalogInput("A", true);
        draft.setAnalogInput("B", true);
        return draft;
    }

    @Test
    void formulasExpandIntoOneRowPerInputCombination() {
        CircuitDraft draft = hexAdder();
        List<TruthRow> rows = draft.formulaRows();

        assertThat(rows).hasSize(256);
        TruthRow row = rows.stream().filter(r -> r.inputs().get("A").equals(new SignalRange(9, 9))
                && r.inputs().get("B").equals(new SignalRange(8, 8))).findFirst().orElseThrow();
        assertThat(row.outputs().get("SUM")).isEqualTo(new SignalRange(1, 1));
        assertThat(row.outputs().get("CARRY")).isEqualTo(new SignalRange(1, 15));
        assertThat(draft.toSpecJson().getAsJsonArray("rows")).hasSize(256);
    }

    @Test
    void onOffInputsAreTriedAtZeroAndFifteen() {
        CircuitDraft draft = hexAdder();
        draft.setAnalogInput("B", false);
        draft.setFormula("SUM", "min(A + B, 15)");
        List<TruthRow> rows = draft.formulaRows();
        assertThat(rows).hasSize(32);
        assertThat(rows).allSatisfy(r -> assertThat(r.inputs().get("B").min()).isIn(0, 15));
    }

    @Test
    void formulaProblemsNameTheOutput() {
        CircuitDraft draft = hexAdder();
        draft.setFormula("SUM", "A + C");
        assertThatThrownBy(draft::formulaRows).hasMessage("SUM: there is no input named C");
        draft.setFormula("SUM", "");
        assertThatThrownBy(draft::formulaRows).hasMessage("SUM has no formula");
        draft.setFormula("SUM", "A + B");
        assertThatThrownBy(draft::formulaRows).hasMessageStartingWith("SUM gives 16");
    }

    @Test
    void formulasFollowRenamesAndSurviveSaving() {
        CircuitDraft draft = hexAdder();
        draft.renamePort("A", "X");
        draft.renamePort("SUM", "S");
        assertThat(draft.getFormula("S")).isEqualTo("(X + B) % 16");
        assertThat(draft.getFormula("CARRY")).isEqualTo("X + B >= 16");
        assertThat(draft.isAnalogInput("X")).isTrue();

        CircuitDraft loaded = CircuitDraft.fromJson(draft.toJson());
        assertThat(loaded.isFormulaMode()).isTrue();
        assertThat(loaded.getFormula("S")).isEqualTo("(X + B) % 16");
        assertThat(loaded.formulaRows()).hasSize(256);
    }
}
