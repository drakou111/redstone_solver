package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import io.github.drakou111.redstonesolver.Chat;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToClient;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToServer;
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
    private final ClientState state;

    public ServerLink(ClientState state) {
        this.state = state;
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
        body.add("spec", this.state.getDraft().toSpecJson(new WorldBlocks(client.level)));
        body.add("origin", box.min().toJson());
        body.addProperty("method", this.state.getMethod().name().toLowerCase(Locale.ROOT));
        JsonObject options = new JsonObject();
        options.addProperty("timeLimit", this.state.getTimeLimitSeconds());
        options.addProperty("onePlacement", this.state.isOnePlacement());
        options.addProperty("sketchTime", this.state.getSketchSeconds());
        body.add("options", options);
        this.send("solve", body);
        this.state.setSolving(true);
        this.state.setProgress("Starting the solver...");
    }

    public void cancel() {
        this.send("cancel", new JsonObject());
    }

    public void place() {
        this.send("place", new JsonObject());
    }

    public void discard() {
        this.send("discard", new JsonObject());
    }

    private void send(String action, JsonObject body) {
        if (!this.isAvailable())
            throw new IllegalStateException("This server does not have the Redstone Solver mod.");
        ClientPlayNetworking.send(new ToServer(action, body.toString()));
    }

    private void receive(LocalPlayer player, String event, JsonObject json) {
        switch (event) {
            case "started" -> this.state.setSolving(true);
            case "progress" -> {
                String text = "Solving... %.0f s  %s".formatted(json.get("seconds").getAsDouble(),
                        json.get("line").getAsString());
                this.state.setProgress(text);
                player.sendOverlayMessage(Chat.hint(text));
            }
            case "result" -> this.onResult(player, json);
            case "placed" -> {
                this.state.setResultWaiting(false);
                player.sendSystemMessage(Chat.good("Placed. Drive the inputs with levers to test it."));
            }
            case "discarded" -> this.state.setResultWaiting(false);
            case "error" -> {
                this.state.setSolving(false);
                player.sendSystemMessage(Chat.error(json.get("message").getAsString()));
            }
            default -> player.sendSystemMessage(Chat.error("Unknown message from the server: " + event));
        }
    }

    private void onResult(LocalPlayer player, JsonObject json) {
        this.state.setSolving(false);
        this.state.setLastResult(Optional.of(json));
        if (json.has("error")) {
            this.state.setResultWaiting(false);
            player.sendSystemMessage(Chat.error("Solve failed: " + json.get("error").getAsString()));
            return;
        }
        if (!json.has("blockCount")) {
            this.state.setResultWaiting(false);
            player.sendSystemMessage(Chat.error("No circuit found (%s after %.0f s). Try a bigger box, looser timing or more time."
                    .formatted(json.get("status").getAsString(), json.get("seconds").getAsDouble())));
            return;
        }
        this.state.setResultWaiting(true);
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
