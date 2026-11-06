package io.github.drakou111.redstonesolver;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import io.github.drakou111.redstonesolver.PlayerSessions.PendingResult;
import io.github.drakou111.redstonesolver.PlayerSessions.Session;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.Layout;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToClient;
import io.github.drakou111.redstonesolver.net.SolverPayloads.ToServer;
import io.github.drakou111.redstonesolver.solver.SolverException;
import io.github.drakou111.redstonesolver.solver.SolverService;
import io.github.drakou111.redstonesolver.solver.SolverService.SolveOutcome;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import java.util.UUID;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.commands.Commands;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;

/** Server side of the mod: answers client requests by running the solver and placing results. */
public final class SolverNetworking {
    public static final int MAX_BOX_VOLUME = 4096;
    private static final double MAX_TIME_LIMIT_SECONDS = 24 * 60 * 60;

    private final PlayerSessions sessions;
    private final SolverService solver;

    public SolverNetworking(PlayerSessions sessions, SolverService solver) {
        this.sessions = sessions;
        this.solver = solver;
    }

    public void register() {
        ServerPlayNetworking.registerGlobalReceiver(ToServer.TYPE, (payload, context) -> {
            ServerPlayer player = context.player();
            try {
                this.handle(context.server(), player, payload);
            } catch (IllegalArgumentException | IllegalStateException | SolverException | JsonParseException e) {
                sendError(player, e.getMessage());
            }
        });
    }

    private void handle(MinecraftServer server, ServerPlayer player, ToServer payload) {
        if (!Commands.LEVEL_GAMEMASTERS.check(player.permissions()))
            throw new IllegalStateException("The solver needs cheats or operator status (it builds blocks).");
        JsonObject body = payload.json().isEmpty() ? new JsonObject() : JsonParser.parseString(payload.json()).getAsJsonObject();
        switch (payload.action()) {
            case "solve" -> this.solve(server, player, body);
            case "cancel" -> this.solver.cancel();
            case "place" -> this.place(player);
            case "discard" -> {
                this.sessions.get(player.getUUID()).setPending(Optional.empty());
                send(player, "discarded", new JsonObject());
            }
            default -> throw new IllegalArgumentException("unknown action " + payload.action());
        }
    }

    /** Body: {spec, origin: [x, y, z], timeLimit}. */
    private void solve(MinecraftServer server, ServerPlayer player, JsonObject body) {
        JsonObject spec = body.getAsJsonObject("spec");
        if (spec == null) throw new IllegalArgumentException("no spec");
        Cell origin = Cell.fromJson(body.get("origin"));
        Cell size = Cell.fromJson(spec.get("size"));
        Box box = new Box(origin, origin.plus(size).minus(new Cell(1, 1, 1)));
        if (box.volume() > MAX_BOX_VOLUME)
            throw new IllegalArgumentException("The box has %d cells; keep it at %d or fewer.".formatted(box.volume(), MAX_BOX_VOLUME));
        double timeLimit = body.get("timeLimit").getAsDouble();
        if (timeLimit <= 0 || timeLimit > MAX_TIME_LIMIT_SECONDS)
            throw new IllegalArgumentException("The time limit must be between 1 s and 24 h.");

        UUID playerId = player.getUUID();
        this.sessions.get(playerId).setPending(Optional.empty());
        this.solver.startSolve(spec, timeLimit, new SolverService.Listener() {
            @Override
            public void progress(double seconds, String lastLogLine) {
                JsonObject json = new JsonObject();
                json.addProperty("seconds", seconds);
                json.addProperty("line", lastLogLine);
                server.execute(() -> sendTo(server, playerId, "progress", json));
            }

            @Override
            public void finished(SolveOutcome outcome) {
                server.execute(() -> SolverNetworking.this.finish(server, playerId, box, outcome));
            }
        });
        send(player, "started", new JsonObject());
    }

    private void finish(MinecraftServer server, UUID playerId, Box box, SolveOutcome outcome) {
        JsonObject json = new JsonObject();
        JsonArray log = new JsonArray();
        for (String line : outcome.log()) log.add(line);
        json.add("log", log);
        json.addProperty("status", outcome.status());
        json.addProperty("seconds", outcome.seconds());
        json.addProperty("optimal", outcome.optimal());
        outcome.error().ifPresent(error -> json.addProperty("error", error));

        if (outcome.layout().isPresent()) {
            Layout layout = outcome.layout().get();
            json.addProperty("blockCount", layout.countPlacedBlocks());
            json.add("size", layout.size().toJson());
            json.add("blocksByType", blocksByType(layout));
            this.sessions.get(playerId).setPending(Optional.of(new PendingResult(layout, box)));
            ServerPlayer player = server.getPlayerList().getPlayer(playerId);
            if (player != null) json.addProperty("boxOccupied", LayoutPlacer.hasBlocks(player.level(), box));
        }
        sendTo(server, playerId, "result", json);
    }

    private void place(ServerPlayer player) {
        Session session = this.sessions.get(player.getUUID());
        PendingResult pending = session.getPending()
                .orElseThrow(() -> new IllegalStateException("No solved circuit to place. Solve first."));
        LayoutPlacer.place(player.level(), pending.box(), pending.layout());
        session.setPending(Optional.empty());
        send(player, "placed", new JsonObject());
    }

    private static JsonObject blocksByType(Layout layout) {
        Map<String, Integer> counts = new TreeMap<>();
        for (Layout.Block block : layout.blocks()) {
            String name = block.type().equals("input") || block.type().equals("output")
                    ? block.type() + " (" + block.kind() + ")"
                    : block.type();
            counts.merge(name, 1, Integer::sum);
        }
        JsonObject json = new JsonObject();
        for (Map.Entry<String, Integer> entry : counts.entrySet()) json.addProperty(entry.getKey(), entry.getValue());
        return json;
    }

    private static void sendTo(MinecraftServer server, UUID playerId, String event, JsonElement json) {
        ServerPlayer player = server.getPlayerList().getPlayer(playerId);
        if (player != null) send(player, event, json);
    }

    private static void send(ServerPlayer player, String event, JsonElement json) {
        ServerPlayNetworking.send(player, new ToClient(event, json.toString()));
    }

    private static void sendError(ServerPlayer player, String message) {
        JsonObject json = new JsonObject();
        json.addProperty("message", message == null ? "unknown error" : message);
        send(player, "error", json);
    }
}
