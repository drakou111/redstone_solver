package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import java.util.Optional;

/** Everything the client knows: the draft being edited, solve settings and the last solve's state. */
public final class ClientState {
    public static final double DEFAULT_TIME_LIMIT_SECONDS = 3600;
    public static final double DEFAULT_SKETCH_SECONDS = 3;

    /** How the circuit is built: one search of the whole box, or from gates. */
    public enum Method {
        SOLVE,
        COMPOSE
    }

    private CircuitDraft draft = new CircuitDraft();
    private double timeLimitSeconds = DEFAULT_TIME_LIMIT_SECONDS;
    private boolean showOutlines = true;
    private boolean solving;
    private String progress = "";
    private JsonObject lastResult;
    private boolean resultWaiting;
    private Method method = Method.SOLVE;
    private double sketchSeconds = DEFAULT_SKETCH_SECONDS;
    private int version;
    private boolean autoShrink = true;
    private boolean cancelling;

    /** Grows whenever the solve state changes, so open screens know to refresh their buttons. */
    public int getVersion() {
        return this.version;
    }

    public Method getMethod() {
        return this.method;
    }

    public void setMethod(Method method) {
        this.method = method;
    }

    public double getSketchSeconds() {
        return this.sketchSeconds;
    }

    public void setSketchSeconds(double seconds) {
        if (!(seconds > 0)) throw new IllegalArgumentException("the sketch step time must be positive");
        this.sketchSeconds = seconds;
    }

    /** Build from gates, then shrink the result right away. */
    public boolean isAutoShrink() {
        return this.autoShrink;
    }

    public void setAutoShrink(boolean autoShrink) {
        this.autoShrink = autoShrink;
    }

    /** True from asking to cancel until the job ends. */
    public boolean isCancelling() {
        return this.cancelling;
    }

    public void setCancelling(boolean cancelling) {
        this.cancelling = cancelling;
        this.version++;
    }

    public CircuitDraft getDraft() {
        return this.draft;
    }

    public void replaceDraft(CircuitDraft draft) {
        this.draft = draft;
        this.version++;
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
        this.cancelling = false;
        this.version++;
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
        this.version++;
    }

    /** True while a solved circuit waits on the server for Place or Discard. */
    public boolean isResultWaiting() {
        return this.resultWaiting;
    }

    public void setResultWaiting(boolean waiting) {
        this.resultWaiting = waiting;
        this.version++;
    }
}
