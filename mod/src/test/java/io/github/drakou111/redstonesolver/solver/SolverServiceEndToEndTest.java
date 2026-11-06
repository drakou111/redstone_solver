package io.github.drakou111.redstonesolver.solver;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assumptions.assumeThat;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.InputKind;
import io.github.drakou111.redstonesolver.core.JavaBlockStates;
import io.github.drakou111.redstonesolver.core.Layout;
import io.github.drakou111.redstonesolver.core.OutputKind;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.TruthRow;
import io.github.drakou111.redstonesolver.solver.SolverService.SolveOutcome;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/** Runs the real solver built by CMake; skipped when it has not been built. */
class SolverServiceEndToEndTest {
    private static final double TIME_LIMIT_SECONDS = 30;
    private static Path executable;

    @BeforeAll
    static void findSolver() {
        String path = System.getProperty("redstone.solver.exe", "");
        assumeThat(path).as("solver path").isNotEmpty();
        executable = Path.of(path);
        assumeThat(Files.isRegularFile(executable)).as("solver built at " + executable).isTrue();
    }

    private static CircuitDraft notGate() {
        CircuitDraft draft = new CircuitDraft();
        draft.setCorner1(new Cell(100, 60, 100));
        draft.setCorner2(new Cell(103, 61, 100));
        draft.putPort(Port.input("IN1", InputKind.COMPARATOR, new Cell(100, 61, 100), Dir.EAST));
        draft.putPort(Port.output("OUT1", OutputKind.DUST, new Cell(103, 61, 100), Dir.NORTH));
        draft.addRow(TruthRow.parse("IN1=1..15 -> OUT1=0"));
        draft.addRow(TruthRow.parse("IN1=0 -> OUT1=1..15"));
        return draft;
    }

    @Test
    void theSolverAcceptsTheDraftSpec() {
        try (SolverClient client = SolverClient.start(executable)) {
            JsonObject body = new JsonObject();
            body.add("spec", notGate().toSpecJson());
            assertThat(client.call("spec/check", body).has("world")).isTrue();
        }
    }

    @Test
    void solvesANotGateIntoPlaceableBlocks() throws Exception {
        CompletableFuture<SolveOutcome> done = new CompletableFuture<>();
        try (SolverService service = new SolverService(executable)) {
            JsonObject options = new JsonObject();
            options.addProperty("timeLimit", TIME_LIMIT_SECONDS);
            JsonObject request = new JsonObject();
            request.add("spec", notGate().toSpecJson());
            request.add("options", options);
            service.startJob("solve/start", request, new SolverService.Listener() {
                @Override
                public void progress(double seconds, String lastLogLine) {
                }

                @Override
                public void finished(SolveOutcome outcome) {
                    done.complete(outcome);
                }
            });
            SolveOutcome outcome = done.get((long) TIME_LIMIT_SECONDS * 2, TimeUnit.SECONDS);

            assertThat(outcome.error()).isEmpty();
            assertThat(outcome.layout()).isPresent();
            Layout layout = outcome.layout().get();
            assertThat(layout.size()).isEqualTo(new Cell(4, 2, 1));
            for (Layout.Block block : layout.blocks()) assertThat(JavaBlockStates.stateFor(block)).isPresent();
            assertThat(outcome.settleTicks()).hasSize(1);
            assertThat(outcome.settleTicks().getFirst()).isPositive();
            assertThat(outcome.analysisError()).isEmpty();
        }
    }
}
