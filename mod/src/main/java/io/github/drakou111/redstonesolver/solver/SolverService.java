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
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Owns the solver process (started on first use, restarted if it dies) and runs one job at a
 * time (solve, compose or shrink), polling its progress on a virtual thread.
 */
public final class SolverService implements AutoCloseable {
    public static final Set<String> JOB_ROUTES = Set.of("solve/start", "compose/start", "shrink/start");

    private static final Logger LOGGER = System.getLogger(SolverService.class.getName());
    private static final Duration POLL_INTERVAL = Duration.ofMillis(500);

    /** Receives progress and the end of a job, on the polling thread. */
    public interface Listener {
        void progress(double seconds, String lastLogLine);

        /** The circuit so far (a partial build, a shrink step, the best found yet), as the solver's layout JSON. */
        default void preview(JsonObject layout) {
        }

        void finished(SolveOutcome outcome);
    }

    /**
     * How a job ended. With a layout, {@code spec} is the spec it solves (a shrink returns a smaller
     * one), {@code settleTicks} the latest tick each output changed after an input switch, and
     * {@code analysis} says why that is missing when it is.
     */
    public record SolveOutcome(Optional<Layout> layout, Optional<JsonObject> layoutJson, Optional<JsonObject> spec,
            String status, boolean optimal, double seconds, Optional<String> error, List<String> log,
            List<Integer> settleTicks, Optional<String> analysisError) {
        public SolveOutcome {
            log = List.copyOf(log);
            settleTicks = List.copyOf(settleTicks);
        }
    }

    private final Path executable;
    private final Optional<Path> library;
    private final AtomicBoolean running = new AtomicBoolean();
    private SolverClient client;

    public SolverService(Path executable) {
        this(executable, Optional.empty());
    }

    /** With {@code library}, the solver keeps verified circuits there and answers repeats from it. */
    public SolverService(Path executable, Optional<Path> library) {
        this.executable = Objects.requireNonNull(executable, "executable");
        this.library = Objects.requireNonNull(library, "library");
    }

    public boolean isRunning() {
        return this.running.get();
    }

    /**
     * Starts a job: {@code route} is one of {@link #JOB_ROUTES} and {@code body} its request
     * ({spec, options, ...}). Fails at once if a job is already running.
     */
    public void startJob(String route, JsonObject body, Listener listener) {
        Objects.requireNonNull(body, "body");
        Objects.requireNonNull(listener, "listener");
        if (!JOB_ROUTES.contains(route)) throw new IllegalArgumentException("not a job route: " + route);
        if (!body.has("spec")) throw new IllegalArgumentException("a job needs a spec");
        if (!this.running.compareAndSet(false, true))
            throw new SolverException("The solver is busy with another job; cancel it first.");

        Thread.ofVirtual().name("redstone-" + route).start(() -> {
            SolveOutcome outcome;
            try {
                SolverClient solver = this.client();
                solver.call(route, body);
                outcome = this.pollUntilDone(solver, body, listener);
            } catch (SolverException e) {
                outcome = failure(e.getMessage());
            } catch (RuntimeException e) {
                LOGGER.log(Level.ERROR, "Solver job failed", e);
                outcome = failure("Unexpected error: " + e);
            } finally {
                this.running.set(false);
            }
            // The solver is free again here, so the listener may start the next job.
            listener.finished(outcome);
        });
    }

    /** A quick request that is not a job (such as export); fails if the solver cannot start. */
    public JsonObject call(String route, JsonObject body) {
        if (JOB_ROUTES.contains(route)) throw new IllegalArgumentException("use startJob for " + route);
        return this.client().call(route, body);
    }

    public void cancel() {
        if (!this.running.get()) throw new SolverException("Nothing is running.");
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
            this.client = SolverClient.start(this.executable, this.library);
        }
        return this.client;
    }

    private SolveOutcome pollUntilDone(SolverClient solver, JsonObject request, Listener listener) {
        int previewSeen = 0;
        while (true) {
            try {
                Thread.sleep(POLL_INTERVAL);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new SolverException("Interrupted while waiting for the solver", e);
            }
            JsonObject query = new JsonObject();
            query.addProperty("previewSince", previewSeen);
            JsonObject status = solver.call("solve/status", query);
            if (status.has("preview")) {
                previewSeen = status.get("previewVersion").getAsInt();
                if (status.get("running").getAsBoolean()) listener.preview(status.getAsJsonObject("preview"));
            }
            List<String> log = strings(status.getAsJsonArray("log"));
            if (status.get("running").getAsBoolean()) {
                double seconds = status.has("seconds") ? status.get("seconds").getAsDouble() : 0;
                listener.progress(seconds, log.isEmpty() ? "" : log.getLast());
                continue;
            }
            return analyzed(solver, request, status, log);
        }
    }

    private static SolveOutcome analyzed(SolverClient solver, JsonObject request, JsonObject status, List<String> log) {
        if (status.has("error")) return failure(status.get("error").getAsString(), log);
        if (!status.has("result")) return failure("The solver sent no result", log);
        JsonObject result = status.getAsJsonObject("result");
        String state = result.get("status").getAsString();
        boolean optimal = result.get("optimal").getAsBoolean();
        double seconds = result.get("seconds").getAsDouble();
        if (!result.has("layout")) {
            return new SolveOutcome(Optional.empty(), Optional.empty(), Optional.empty(), state, optimal, seconds,
                    Optional.empty(), log, List.of(), Optional.empty());
        }

        JsonObject layoutJson = result.getAsJsonObject("layout");
        JsonObject spec = status.has("spec") ? status.getAsJsonObject("spec") : request.getAsJsonObject("spec");
        List<Integer> settle = new ArrayList<>();
        Optional<String> analysisError = Optional.empty();
        try {
            JsonObject body = new JsonObject();
            body.add("spec", spec);
            body.add("world", layoutJson);
            JsonObject options = request.getAsJsonObject("options");
            boolean onePlacement = options != null && options.has("onePlacement") && options.get("onePlacement").getAsBoolean();
            body.addProperty("onePlacement", onePlacement);
            for (JsonElement tick : solver.call("result/analyze", body).getAsJsonArray("settleTicks"))
                settle.add(tick.getAsInt());
        } catch (SolverException e) {
            LOGGER.log(Level.WARNING, "Timing analysis failed: " + e.getMessage());
            analysisError = Optional.of(e.getMessage());
        }
        return new SolveOutcome(Optional.of(Layout.fromJson(layoutJson)), Optional.of(layoutJson), Optional.of(spec),
                state, optimal, seconds, Optional.empty(), log, settle, analysisError);
    }

    private static SolveOutcome failure(String message) {
        return failure(message, List.of());
    }

    private static SolveOutcome failure(String message, List<String> log) {
        return new SolveOutcome(Optional.empty(), Optional.empty(), Optional.empty(), "error", false, 0,
                Optional.of(message), log, List.of(), Optional.empty());
    }

    private static List<String> strings(JsonArray array) {
        List<String> lines = new ArrayList<>();
        if (array == null) return lines;
        for (JsonElement element : array) lines.add(element.getAsString());
        return lines;
    }
}
