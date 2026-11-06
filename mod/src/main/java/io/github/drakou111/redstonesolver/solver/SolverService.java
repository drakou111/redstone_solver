package io.github.drakou111.redstonesolver.solver;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Layout;
import java.lang.System.Logger;
import java.lang.System.Logger.Level;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Owns the solver process (started on first use, restarted if it dies) and runs one solve at a
 * time, polling its progress on a virtual thread.
 */
public final class SolverService implements AutoCloseable {
    private static final Logger LOGGER = System.getLogger(SolverService.class.getName());
    private static final Duration POLL_INTERVAL = Duration.ofMillis(500);

    /** Receives progress and the end of a solve, on the polling thread. */
    public interface Listener {
        void progress(double seconds, String lastLogLine);

        void finished(SolveOutcome outcome);
    }

    /** How a solve ended: a verified layout, no circuit, or an error. */
    public record SolveOutcome(Optional<Layout> layout, String status, boolean optimal, double seconds,
            Optional<String> error, List<String> log) {
        public SolveOutcome {
            log = List.copyOf(log);
        }
    }

    private final Path executable;
    private final AtomicBoolean running = new AtomicBoolean();
    private SolverClient client;

    public SolverService(Path executable) {
        this.executable = Objects.requireNonNull(executable, "executable");
    }

    public Path getExecutable() {
        return this.executable;
    }

    public boolean isRunning() {
        return this.running.get();
    }

    /** Starts a solve of {@code spec} (solver JSON); fails at once if one is already running. */
    public void startSolve(JsonObject spec, double timeLimitSeconds, Listener listener) {
        Objects.requireNonNull(spec, "spec");
        Objects.requireNonNull(listener, "listener");
        if (timeLimitSeconds <= 0) throw new IllegalArgumentException("the time limit must be positive");
        if (!this.running.compareAndSet(false, true))
            throw new SolverException("A solve is already running; /rs cancel stops it.");

        Thread.ofVirtual().name("redstone-solve").start(() -> {
            try {
                SolverClient solver = this.client();
                JsonObject options = new JsonObject();
                options.addProperty("timeLimit", timeLimitSeconds);
                JsonObject body = new JsonObject();
                body.add("spec", spec);
                body.add("options", options);
                solver.call("solve/start", body);
                listener.finished(this.pollUntilDone(solver, listener));
            } catch (SolverException e) {
                listener.finished(failure(e.getMessage()));
            } catch (RuntimeException e) {
                LOGGER.log(Level.ERROR, "Solve failed", e);
                listener.finished(failure("Unexpected error: " + e));
            } finally {
                this.running.set(false);
            }
        });
    }

    public void cancel() {
        if (!this.running.get()) throw new SolverException("No solve is running.");
        this.client().call("solve/cancel", new JsonObject());
    }

    @Override
    public synchronized void close() {
        if (this.client != null) this.client.close();
        this.client = null;
    }

    private synchronized SolverClient client() {
        if (this.client == null || !this.client.isAlive()) {
            if (this.client != null) LOGGER.log(Level.WARNING, "The solver process ended; starting it again");
            this.client = SolverClient.start(this.executable);
        }
        return this.client;
    }

    private SolveOutcome pollUntilDone(SolverClient solver, Listener listener) {
        while (true) {
            try {
                Thread.sleep(POLL_INTERVAL);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new SolverException("Interrupted while waiting for the solver", e);
            }
            JsonObject status = solver.call("solve/status", new JsonObject());
            List<String> log = strings(status.getAsJsonArray("log"));
            if (status.get("running").getAsBoolean()) {
                double seconds = status.has("seconds") ? status.get("seconds").getAsDouble() : 0;
                listener.progress(seconds, log.isEmpty() ? "" : log.getLast());
                continue;
            }
            return outcomeOf(status, log);
        }
    }

    static SolveOutcome outcomeOf(JsonObject status, List<String> log) {
        if (status.has("error")) {
            String error = status.get("error").getAsString();
            return new SolveOutcome(Optional.empty(), "error", false, 0, Optional.of(error), log);
        }
        if (!status.has("result"))
            return new SolveOutcome(Optional.empty(), "error", false, 0, Optional.of("The solver sent no result"), log);
        JsonObject result = status.getAsJsonObject("result");
        Optional<Layout> layout = result.has("layout")
                ? Optional.of(Layout.fromJson(result.getAsJsonObject("layout")))
                : Optional.empty();
        return new SolveOutcome(layout, result.get("status").getAsString(), result.get("optimal").getAsBoolean(),
                result.get("seconds").getAsDouble(), Optional.empty(), log);
    }

    private static SolveOutcome failure(String message) {
        return new SolveOutcome(Optional.empty(), "error", false, 0, Optional.of(message), List.of());
    }

    private static List<String> strings(JsonArray array) {
        List<String> lines = new ArrayList<>();
        if (array == null) return lines;
        for (JsonElement element : array) lines.add(element.getAsString());
        return lines;
    }
}
