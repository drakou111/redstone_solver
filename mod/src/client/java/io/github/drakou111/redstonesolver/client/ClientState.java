package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/** Everything the client knows: the draft being edited, solve settings and the last solve's state. */
public final class ClientState {
    public static final double DEFAULT_TIME_LIMIT_SECONDS = 3600;
    public static final double DEFAULT_SKETCH_SECONDS = 3;
    /** Results kept (the server keeps as many). */
    public static final int MAX_RESULTS = 20;

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
    private Optional<Preview> preview = Optional.empty();

    /** The solver's circuit so far and the world cell its lowest corner sits at. */
    public record Preview(Cell origin, Layout layout) {
    }
    private final List<JsonObject> results = new ArrayList<>(); // newest last
    private int selected = -1;
    private Method method = Method.SOLVE;
    private double sketchSeconds = DEFAULT_SKETCH_SECONDS;
    private int version;
    private boolean autoShrink = true;
    private boolean inputsInOrder;
    private OutputOrder outputOrder = OutputOrder.ANY;
    private boolean portsAlongSide;
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

    /** Search the box may move each port along its side of the box. */
    public boolean isPortsAlongSide() {
        return this.portsAlongSide;
    }

    public void setPortsAlongSide(boolean portsAlongSide) {
        this.portsAlongSide = portsAlongSide;
    }

    /** Build from gates, then shrink the result right away. */
    public boolean isAutoShrink() {
        return this.autoShrink;
    }

    public void setAutoShrink(boolean autoShrink) {
        this.autoShrink = autoShrink;
    }

    /** Build from gates keeps the inputs down the west side in order, IN1 first. */
    public boolean isInputsInOrder() {
        return this.inputsInOrder;
    }

    public void setInputsInOrder(boolean inputsInOrder) {
        this.inputsInOrder = inputsInOrder;
    }

    /** How Build from gates orders the outputs down the east side. */
    public enum OutputOrder {
        ANY("any", "Outputs: any order"),
        IN_ORDER("inOrder", "Outputs: in order"),
        IN_ORDER_OR_REVERSED("inOrderOrReversed", "Outputs: in order or flipped");

        private final String wireName;
        private final String label;

        OutputOrder(String wireName, String label) {
            this.wireName = wireName;
            this.label = label;
        }

        /** The value the solver host reads. */
        public String getWireName() {
            return this.wireName;
        }

        public String getLabel() {
            return this.label;
        }

        public OutputOrder next() {
            OutputOrder[] all = values();
            return all[(this.ordinal() + 1) % all.length];
        }
    }

    public OutputOrder getOutputOrder() {
        return this.outputOrder;
    }

    public void setOutputOrder(OutputOrder outputOrder) {
        this.outputOrder = Objects.requireNonNull(outputOrder);
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
        this.preview = Optional.empty();
        if (!solving) this.progress = "";
    }

    /** What the running job has so far, drawn in the world until it ends. */
    public Optional<Preview> getPreview() {
        return this.preview;
    }

    public void setPreview(Preview preview) {
        this.preview = this.solving ? Optional.of(preview) : Optional.empty();
    }

    public String getProgress() {
        return this.progress;
    }

    public void setProgress(String progress) {
        this.progress = progress;
    }

    /** The result shown on the card (see SolverNetworking.finish for its fields), if any. */
    public Optional<JsonObject> getSelectedResult() {
        if (this.selected < 0 || this.selected >= this.results.size()) return Optional.empty();
        return Optional.of(this.results.get(this.selected));
    }

    /** Keeps a result from the server and shows it; the oldest go past the server's limit. */
    public void addResult(JsonObject result) {
        this.results.add(result);
        while (this.results.size() > MAX_RESULTS) this.results.removeFirst();
        this.selected = this.results.size() - 1;
        this.version++;
    }

    public void removeResult(int id) {
        this.results.removeIf(r -> r.has("id") && r.get("id").getAsInt() == id);
        this.selected = Math.min(this.selected, this.results.size() - 1);
        this.version++;
    }

    public void clearResults() {
        this.results.clear();
        this.selected = -1;
        this.version++;
    }

    public int getResultCount() {
        return this.results.size();
    }

    /** 0-based position of the shown result. */
    public int getSelectedIndex() {
        return this.selected;
    }

    /** Shows an older (negative) or newer (positive) result. */
    public void step(int by) {
        if (this.results.isEmpty()) return;
        this.selected = Math.clamp(this.selected + by, 0, this.results.size() - 1);
        this.version++;
    }

    /** The shown result's server id, if it is a circuit (failed solves have none). */
    public Optional<Integer> getSelectedId() {
        return this.getSelectedResult().filter(r -> r.has("id")).map(r -> r.get("id").getAsInt());
    }
}
