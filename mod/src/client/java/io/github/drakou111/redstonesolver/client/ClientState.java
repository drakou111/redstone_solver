package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import java.util.Optional;

/** Everything the client knows: the draft being edited, solve settings and the last solve's state. */
public final class ClientState {
    public static final double DEFAULT_TIME_LIMIT_SECONDS = 60;

    private final CircuitDraft draft = new CircuitDraft();
    private double timeLimitSeconds = DEFAULT_TIME_LIMIT_SECONDS;
    private boolean showOutlines = true;
    private boolean solving;
    private String progress = "";
    private JsonObject lastResult;
    private boolean resultWaiting;

    public CircuitDraft getDraft() {
        return this.draft;
    }

    public double getTimeLimitSeconds() {
        return this.timeLimitSeconds;
    }

    public void setTimeLimitSeconds(double seconds) {
        if (!(seconds > 0)) throw new IllegalArgumentException("the time limit must be positive");
        this.timeLimitSeconds = seconds;
    }

    public boolean isShowOutlines() {
        return this.showOutlines;
    }

    public void setShowOutlines(boolean show) {
        this.showOutlines = show;
    }

    public boolean isSolving() {
        return this.solving;
    }

    public void setSolving(boolean solving) {
        this.solving = solving;
        if (!solving) this.progress = "";
    }

    public String getProgress() {
        return this.progress;
    }

    public void setProgress(String progress) {
        this.progress = progress;
    }

    /** The server's last "result" message, if any (see SolverNetworking.finish for its fields). */
    public Optional<JsonObject> getLastResult() {
        return Optional.ofNullable(this.lastResult);
    }

    public void setLastResult(Optional<JsonObject> result) {
        this.lastResult = result.orElse(null);
    }

    /** True while a solved circuit waits on the server for Place or Discard. */
    public boolean isResultWaiting() {
        return this.resultWaiting;
    }

    public void setResultWaiting(boolean waiting) {
        this.resultWaiting = waiting;
    }
}
