package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.github.drakou111.redstonesolver.Chat;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.Layout;
import io.github.drakou111.redstonesolver.core.ProgressLine;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToClient;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToServer;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.MutableComponent;

/** Sends the client's requests to the server and applies its answers to the client state. */
public final class ServerLink {
    private static final double MAX_CUT_SECONDS = 120;
    private static final double FIRST_COMPOSE_STEP_SECONDS = 120;
    // Circuits are checked as built, facing this way only: much faster, and
    // rotated or moved copies behave the same in practice.
    private static final boolean ONE_PLACEMENT = true;

    private final ClientState state;
    private final DraftAutosave autosave;
    private JsonObject lastSolve;

    public ServerLink(ClientState state, DraftAutosave autosave) {
        this.state = state;
        this.autosave = autosave;
    }

    public void register() {
        ClientPlayNetworking.registerGlobalReceiver(ToClient.TYPE, (payload, context) -> {
            JsonObject json = JsonParser.parseString(payload.json()).getAsJsonObject();
            this.receive(context.player(), payload.event(), json);
        });
    }

    public boolean isAvailable() {
        return ClientPlayNetworking.canSend(ToServer.TYPE);
    }

    /** Sends the draft to be solved; throws with a message for the player when the draft is incomplete. */
    public void solve() {
        if (this.state.isSolving()) throw new IllegalStateException("A solve is already running.");
        Box box = this.state.getDraft().requireBox();
        JsonObject body = new JsonObject();
        Minecraft client = Minecraft.getInstance();
        if (client.level == null) throw new IllegalStateException("Not in a world.");
        WorldBlocks blocks = new WorldBlocks(client.level);
        List<String> stale = this.state.getDraft().refreshPorts(blocks::portBlockAt);
        if (!stale.isEmpty() && client.player != null) {
            client.player.sendSystemMessage(Chat.hint("No port block at " + String.join(", ", stale)
                    + " any more; using its last kind and facing."));
        }
        JsonObject spec = this.state.getDraft().toSpecJson(blocks);
        if (this.state.getMethod() == ClientState.Method.SOLVE && this.state.isPortsAlongSide())
            CircuitDraft.freePortsAlongTheirSide(spec);
        body.add("spec", spec);
        body.add("origin", this.state.getDraft().specOrigin().toJson());
        body.addProperty("floorAssumed", this.state.getDraft().assumesFloor());
        body.addProperty("method", this.state.getMethod().name().toLowerCase(Locale.ROOT));
        JsonObject options = new JsonObject();
        boolean compose = this.state.getMethod() == ClientState.Method.COMPOSE;
        // Build from gates: the time limit is the whole budget; the solver starts
        // with short steps and lengthens them until the budget is used.
        options.addProperty("timeLimit", compose ? FIRST_COMPOSE_STEP_SECONDS : this.state.getTimeLimitSeconds());
        options.addProperty("onePlacement", ONE_PLACEMENT);
        options.addProperty("sketchTime", this.state.getSketchSeconds());
        if (compose) {
            options.addProperty("maxHeight", this.state.getDraft().specSize().y());
            options.addProperty("totalTime", this.state.getTimeLimitSeconds());
            // Then shrink: every variant is built, shrunk until the deadline, the smallest kept.
            options.addProperty("shrink", this.state.isAutoShrink());
            options.addProperty("keepInputOrder", this.state.isInputsInOrder());
            options.addProperty("outputOrder", this.state.getOutputOrder().getWireName());
            options.addProperty("cutTime", Math.min(MAX_CUT_SECONDS, this.state.getTimeLimitSeconds()));
        }
        body.add("options", options);
        this.lastSolve = body.deepCopy();
        this.send("solve", body);
        this.autosave.saveNow();
        this.state.setSolving(true);
        this.state.setProgress("Starting the solver...");
    }

    /** Shrinks the shown result: removes columns, rows and layers while it still works. */
    public void shrink() {
        if (this.state.isSolving()) throw new IllegalStateException("A solve is already running.");
        JsonObject body = this.selectedBody();
        body.add("options", this.shrinkOptions());
        this.send("shrink", body);
        this.state.setSolving(true);
        this.state.setProgress("Starting the shrink...");
    }

    /** Asks for the shown result as /setblock commands; they arrive as an "export" message. */
    public void export() {
        this.send("export", this.selectedBody());
    }

    /** Shrinks the circuit built in the box; the server checks that it works first. */
    public void shrinkBuild() {
        if (this.state.isSolving()) throw new IllegalStateException("A solve is already running.");
        Minecraft client = Minecraft.getInstance();
        if (client.level == null) throw new IllegalStateException("Not in a world.");
        WorldBlocks blocks = new WorldBlocks(client.level);
        CircuitDraft draft = this.state.getDraft();
        draft.refreshPorts(blocks::portBlockAt);
        JsonObject body = new JsonObject();
        body.add("spec", draft.toSpecJson(blocks));
        body.add("world", draft.buildLayoutJson(blocks));
        body.add("origin", draft.specOrigin().toJson());
        body.addProperty("floorAssumed", draft.assumesFloor());
        body.add("options", this.shrinkOptions());
        this.send("shrink", body);
        this.autosave.saveNow();
        this.state.setSolving(true);
        this.state.setProgress("Checking your build...");
    }

    /** The last solve again, without the circuit library's answer (to look for a better circuit). */
    public void searchAgain() {
        if (this.state.isSolving()) throw new IllegalStateException("A solve is already running.");
        if (this.lastSolve == null) throw new IllegalStateException("Nothing to search again.");
        JsonObject body = this.lastSolve.deepCopy();
        body.getAsJsonObject("options").addProperty("library", false);
        this.send("solve", body);
        this.state.setSolving(true);
        this.state.setProgress("Searching again...");
    }

    public void cancel() {
        if (!this.state.isSolving()) throw new IllegalStateException("Nothing is running.");
        this.send("cancel", new JsonObject());
        this.state.setCancelling(true);
    }

    /** Places the shown result back where it was solved. */
    public void place() {
        this.send("place", this.selectedBody());
    }

    /**
     * Places the shown result at a cell, turned to face {@code facing}: its input side towards
     * where the player looked, the circuit going forward and to the right from the cell.
     */
    public void placeAt(Cell target, Dir facing) {
        JsonObject result = this.state.getSelectedResult()
                .orElseThrow(() -> new IllegalStateException("No circuit to place."));
        int turns = switch (facing) {
            case SOUTH -> 1;
            case WEST -> 2;
            case NORTH -> 3;
            default -> 0;
        };
        Cell size = Cell.fromJson(result.get("size"));
        int sizeX = turns % 2 == 0 ? size.x() : size.z();
        int sizeZ = turns % 2 == 0 ? size.z() : size.x();
        Cell back = switch (turns) {
            case 1 -> new Cell(sizeX - 1, 0, 0);
            case 2 -> new Cell(sizeX - 1, 0, sizeZ - 1);
            case 3 -> new Cell(0, 0, sizeZ - 1);
            default -> new Cell(0, 0, 0);
        };
        boolean floor = result.has("floorAssumed") && result.get("floorAssumed").getAsBoolean();
        Cell origin = target.minus(back).minus(new Cell(0, floor ? 1 : 0, 0));
        JsonObject body = this.selectedBody();
        body.add("at", origin.toJson());
        body.addProperty("turns", turns);
        this.send("place", body);
    }

    public void discard() {
        this.send("discard", this.selectedBody());
    }

    // {id} of the shown result, or {} (the server then takes the newest).
    private JsonObject selectedBody() {
        JsonObject body = new JsonObject();
        this.state.getSelectedId().ifPresent(id -> body.addProperty("id", id));
        return body;
    }

    private JsonObject shrinkOptions() {
        JsonObject options = new JsonObject();
        options.addProperty("timeLimit", this.state.getTimeLimitSeconds());
        options.addProperty("cutTime", Math.min(MAX_CUT_SECONDS, this.state.getTimeLimitSeconds()));
        options.addProperty("onePlacement", ONE_PLACEMENT);
        options.addProperty("sketchTime", this.state.getSketchSeconds());
        return options;
    }

    private void send(String action, JsonObject body) {
        if (!this.isAvailable())
            throw new IllegalStateException("This server does not have the Redstone Solver mod.");
        ClientPlayNetworking.send(new ToServer(action, body.toString()));
    }

    private void receive(LocalPlayer player, String event, JsonObject json) {
        switch (event) {
            case "started" -> this.state.setSolving(true);
            case "cancelling" -> this.state.setCancelling(true);
            case "progress" -> {
                String text = "Solving... %.0f s  %s".formatted(json.get("seconds").getAsDouble(),
                        ProgressLine.of(json.get("line").getAsString()));
                this.state.setProgress(text);
                player.sendOverlayMessage(Chat.hint(text));
            }
            case "preview" -> this.state.setPreview(new ClientState.Preview(Cell.fromJson(json.get("origin")),
                    Layout.fromJson(json.getAsJsonObject("layout"))));
            case "result" -> this.onResult(player, json);
            case "placed" -> {
                player.sendSystemMessage(Chat.good("Placed. Drive the inputs with levers to test it."));
                if (json.get("inPlace").getAsBoolean()) this.adoptPlacedResult(player, json.get("id").getAsInt());
            }
            case "discarded" -> this.state.removeResult(json.get("id").getAsInt());
            case "export" -> {
                Minecraft.getInstance().keyboardHandler.setClipboard(json.get("text").getAsString());
                player.sendSystemMessage(Chat.good("Copied the /setblock commands to the clipboard (run them from the box's lowest corner)."));
            }
            case "error" -> {
                this.state.setSolving(false);
                player.sendSystemMessage(Chat.error(json.get("message").getAsString()));
            }
            default -> player.sendSystemMessage(Chat.error("Unknown message from the server: " + event));
        }
    }

    private void adoptPlacedResult(LocalPlayer player, int id) {
        Optional<JsonObject> result = this.state.getSelectedResult()
                .filter(r -> r.has("id") && r.get("id").getAsInt() == id);
        if (result.isEmpty() || !result.get().has("reshaped") || !result.get().get("reshaped").getAsBoolean()) return;
        try {
            boolean floor = result.get().has("floorAssumed") && result.get().get("floorAssumed").getAsBoolean();
            this.state.getDraft().adoptSpec(result.get().getAsJsonObject("spec"), Cell.fromJson(result.get().get("origin")),
                    floor);
            player.sendSystemMessage(Chat.hint("The box and ports now match the placed circuit."));
        } catch (IllegalArgumentException e) {
            player.sendSystemMessage(Chat.error("Could not update the box and ports: " + e.getMessage()));
        }
    }

    private void onResult(LocalPlayer player, JsonObject json) {
        this.state.setSolving(false);
        this.state.addResult(json);
        if (json.has("error")) {
            player.sendSystemMessage(Chat.error("Solve failed: " + json.get("error").getAsString()));
            return;
        }
        if (!json.has("blockCount")) {
            player.sendSystemMessage(Chat.error("No circuit found (%s after %.0f s). Try a bigger box, looser timing or more time."
                    .formatted(json.get("status").getAsString(), json.get("seconds").getAsDouble())));
            return;
        }
        player.sendSystemMessage(resultMessage(json));
    }

    static MutableComponent resultMessage(JsonObject json) {
        boolean optimal = json.get("optimal").getAsBoolean();
        boolean occupied = json.has("boxOccupied") && json.get("boxOccupied").getAsBoolean();
        StringBuilder counts = new StringBuilder();
        for (Map.Entry<String, JsonElement> entry : json.getAsJsonObject("blocksByType").entrySet()) {
            if (!counts.isEmpty()) counts.append(", ");
            counts.append(entry.getValue().getAsInt()).append(' ').append(entry.getKey());
        }
        String summary = "%s: %d blocks in %.1f s%s ".formatted(optimal ? "Smallest circuit" : "Circuit found",
                json.get("blockCount").getAsInt(), json.get("seconds").getAsDouble(),
                optimal ? "" : " (maybe not the smallest)");
        return Chat.good(summary)
                .append(Chat.button(occupied ? "Replace box" : "Place", "/rs place",
                        occupied ? "Clears the box (your build in it is lost) and builds the circuit"
                                : "Builds the circuit in the box"))
                .append(Component.literal(" "))
                .append(Chat.button("Discard", "/rs discard", "Forget this circuit"))
                .append(Component.literal("\n"))
                .append(Chat.hint(counts.toString()));
    }

}
