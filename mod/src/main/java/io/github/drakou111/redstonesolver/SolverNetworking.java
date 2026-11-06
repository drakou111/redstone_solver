package io.github.drakou111.redstonesolver;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import io.github.drakou111.redstonesolver.PlayerSessions.LastResult;
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
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
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
    // One job runs at a time; set when the player asks to stop it.
    private final AtomicBoolean cancelRequested = new AtomicBoolean();

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
            case "cancel" -> {
                this.cancelRequested.set(true);
                this.solver.cancel();
                send(player, "cancelling", new JsonObject());
            }
            case "place" -> this.place(player);
            case "shrink" -> this.shrink(server, player, body);
            case "export" -> this.export(player);
            case "discard" -> {
                this.sessions.get(player.getUUID()).setPending(Optional.empty());
                send(player, "discarded", new JsonObject());
            }
            default -> throw new IllegalArgumentException("unknown action " + payload.action());
        }
    }

    /**
     * Body: {spec, origin: [x, y, z], method: "solve" | "compose", options: {timeLimit, onePlacement,
     * sketchTime, maxHeight, shrinkTimeLimit, cutTime}}. The options go to the solver unchanged.
     */
    private void solve(MinecraftServer server, ServerPlayer player, JsonObject body) {
        JsonObject spec = body.getAsJsonObject("spec");
        JsonObject options = body.getAsJsonObject("options");
        if (spec == null || options == null) throw new IllegalArgumentException("a solve needs a spec and options");
        Cell origin = Cell.fromJson(body.get("origin"));
        requireSmallEnough(Cell.fromJson(spec.get("size")));
        requireTimeLimit(options);
        String method = body.has("method") ? body.get("method").getAsString() : "solve";
        String route = switch (method) {
            case "solve" -> "solve/start";
            case "compose" -> "compose/start";
            default -> throw new IllegalArgumentException("unknown method " + method);
        };

        JsonObject request = new JsonObject();
        request.add("spec", spec);
        request.add("options", options);
        this.startJob(server, player, route, request, origin, Optional.empty(), floorAssumed(body));
    }

    /**
     * Body: {options: {timeLimit, cutTime, onePlacement, sketchTime}} shrinks the last result. With
     * {spec, world, origin} as well it shrinks the circuit built in the world, once it is checked to
     * work. Placing the result clears the old, bigger box.
     */
    private void shrink(MinecraftServer server, ServerPlayer player, JsonObject body) {
        JsonObject options = body.getAsJsonObject("options");
        if (options == null) throw new IllegalArgumentException("a shrink needs options");
        requireTimeLimit(options);
        JsonObject spec;
        JsonObject world;
        Cell origin;
        boolean floor;
        if (body.has("world")) {
            floor = floorAssumed(body);
            spec = body.getAsJsonObject("spec");
            world = body.getAsJsonObject("world");
            origin = Cell.fromJson(body.get("origin"));
            requireSmallEnough(Cell.fromJson(spec.get("size")));
            JsonObject check = new JsonObject();
            check.add("spec", spec);
            check.add("world", world);
            check.addProperty("onePlacement", options.has("onePlacement") && options.get("onePlacement").getAsBoolean());
            JsonObject verdict = this.solver.call("verify", check);
            if (!verdict.get("ok").getAsBoolean())
                throw new IllegalStateException("Your build does not work yet, so it cannot be shrunk: "
                        + describeFailure(verdict.getAsJsonObject("failure")));
        } else {
            LastResult last = this.sessions.get(player.getUUID()).getLast()
                    .orElseThrow(() -> new IllegalStateException("Solve something first; Shrink works on the last result."));
            spec = last.spec();
            world = last.layoutJson();
            origin = last.origin();
            floor = last.floorAssumed();
        }
        JsonObject request = new JsonObject();
        request.add("spec", spec);
        request.add("world", world);
        request.add("options", options);
        Box oldBox = boxAt(origin, Cell.fromJson(spec.get("size")));
        this.startJob(server, player, "shrink/start", request, origin, Optional.of(oldBox), floor);
    }

    private void startJob(MinecraftServer server, ServerPlayer player, String route, JsonObject request, Cell origin,
            Optional<Box> alsoClear, boolean floorAssumed) {
        UUID playerId = player.getUUID();
        this.sessions.get(playerId).setPending(Optional.empty());
        this.solver.startJob(route, request, new SolverService.Listener() {
            @Override
            public void progress(double seconds, String lastLogLine) {
                JsonObject json = new JsonObject();
                json.addProperty("seconds", seconds);
                json.addProperty("line", lastLogLine);
                server.execute(() -> sendTo(server, playerId, "progress", json));
            }

            @Override
            public void finished(SolveOutcome outcome) {
                server.execute(() -> SolverNetworking.this.finish(server, playerId, route, request, origin, alsoClear,
                        floorAssumed, outcome));
            }
        });
        this.cancelRequested.set(false);
        send(player, "started", new JsonObject());
    }

    // The result box starts at the origin; Compose and Shrink may return a different size.
    private void finish(MinecraftServer server, UUID playerId, String route, JsonObject request, Cell origin,
            Optional<Box> alsoClear, boolean floorAssumed, SolveOutcome outcome) {
        boolean cancelled = this.cancelRequested.get();
        JsonObject json = new JsonObject();
        JsonArray log = new JsonArray();
        for (String line : outcome.log()) log.add(line);
        json.add("log", log);
        json.addProperty("status", outcome.status());
        json.addProperty("seconds", outcome.seconds());
        json.addProperty("optimal", outcome.optimal());
        json.addProperty("cancelled", cancelled);
        json.addProperty("shrunk", route.equals("shrink/start"));
        outcome.error().ifPresent(error -> json.addProperty("error", error));

        ServerPlayer player = server.getPlayerList().getPlayer(playerId);
        if (outcome.layout().isPresent()) {
            JsonObject layoutJson = outcome.layoutJson().orElseThrow();
            boolean skipBottom = floorAssumed && outcome.layout().get().size().y() >= 2;
            // Build from gates draws its own bottom layer; it may stand on the floor only if it
            // still works with that layer full.
            if (skipBottom && route.equals("compose/start")) {
                JsonObject onFloor = withFullBottomLayer(layoutJson);
                if (this.worksAs(outcome.spec().orElseThrow(), onFloor, request)) {
                    layoutJson = onFloor;
                } else {
                    skipBottom = false;
                    json.addProperty("note", "This circuit needs its own bottom layer, so Place also builds the layer under your box.");
                }
            }
            Layout layout = Layout.fromJson(layoutJson);
            Box box = boxAt(origin, layout.size());
            Layout shown = skipBottom ? withoutBottomLayer(layout) : layout;
            json.addProperty("blockCount", shown.countPlacedBlocks());
            Cell shownSize = skipBottom ? layout.size().minus(new Cell(0, 1, 0)) : layout.size();
            json.add("size", shownSize.toJson());
            json.add("origin", origin.toJson());
            json.addProperty("floorAssumed", skipBottom);
            json.add("blocksByType", blocksByType(shown));
            JsonArray settle = new JsonArray();
            for (int ticks : outcome.settleTicks()) settle.add(ticks);
            json.add("settleTicks", settle);
            outcome.analysisError().ifPresent(error -> json.addProperty("analysisError", error));
            outcome.spec().ifPresent(spec -> json.add("spec", spec));
            // Compose and Shrink change the box or the ports; the client adopts them once placed.
            json.addProperty("reshaped", !route.equals("solve/start"));
            Session session = this.sessions.get(playerId);
            boolean skip = skipBottom;
            session.setPending(Optional.of(new PendingResult(layout, box, alsoClear, skip)));
            session.setLast(Optional.of(new LastResult(layoutJson, outcome.spec().orElseThrow(), origin, skip)));
            if (player != null) {
                boolean occupied = LayoutPlacer.hasBlocks(player.level(), LayoutPlacer.placedPart(box, skip))
                        || alsoClear.map(old -> LayoutPlacer.hasBlocks(player.level(), LayoutPlacer.placedPart(old, skip)))
                                .orElse(false);
                json.addProperty("boxOccupied", occupied);
            }
        }
        sendTo(server, playerId, "result", json);
    }

    private void export(ServerPlayer player) {
        LastResult last = this.sessions.get(player.getUUID()).getLast()
                .orElseThrow(() -> new IllegalStateException("Solve something first; Export works on the last result."));
        JsonObject body = new JsonObject();
        body.add("world", last.layoutJson());
        send(player, "export", this.solver.call("export", body));
    }

    private void place(ServerPlayer player) {
        Session session = this.sessions.get(player.getUUID());
        PendingResult pending = session.getPending()
                .orElseThrow(() -> new IllegalStateException("No solved circuit to place. Solve first."));
        pending.alsoClear().ifPresent(old -> LayoutPlacer.clear(player.level(), LayoutPlacer.placedPart(old, pending.skipBottom())));
        LayoutPlacer.place(player.level(), pending.box(), pending.layout(), pending.skipBottom());
        session.setPending(Optional.empty());
        send(player, "placed", new JsonObject());
    }

    private boolean worksAs(JsonObject spec, JsonObject world, JsonObject request) {
        JsonObject options = request.getAsJsonObject("options");
        JsonObject check = new JsonObject();
        check.add("spec", spec);
        check.add("world", world);
        check.addProperty("onePlacement", options.has("onePlacement") && options.get("onePlacement").getAsBoolean());
        return this.solver.call("verify", check).get("ok").getAsBoolean();
    }

    private static boolean floorAssumed(JsonObject body) {
        return body.has("floorAssumed") && body.get("floorAssumed").getAsBoolean();
    }

    // The layout with every empty cell of its bottom layer made solid, like the assumed floor.
    private static JsonObject withFullBottomLayer(JsonObject layoutJson) {
        Cell size = Cell.fromJson(layoutJson.get("size"));
        Set<Cell> taken = new HashSet<>();
        JsonArray blocks = layoutJson.getAsJsonArray("blocks").deepCopy();
        for (JsonElement block : blocks) taken.add(Cell.fromJson(block.getAsJsonObject().get("position")));
        for (int x = 0; x < size.x(); x++) {
            for (int z = 0; z < size.z(); z++) {
                Cell cell = new Cell(x, 0, z);
                if (taken.contains(cell)) continue;
                JsonObject solid = new JsonObject();
                solid.addProperty("type", "solid");
                solid.add("position", cell.toJson());
                blocks.add(solid);
            }
        }
        JsonObject result = new JsonObject();
        result.add("size", size.toJson());
        result.add("blocks", blocks);
        return result;
    }

    private static Layout withoutBottomLayer(Layout layout) {
        List<Layout.Block> blocks = new ArrayList<>();
        for (Layout.Block block : layout.blocks()) {
            if (block.position().y() > 0) blocks.add(block);
        }
        return new Layout(layout.size(), blocks);
    }

    private static void requireSmallEnough(Cell size) {
        int volume = size.x() * size.y() * size.z();
        if (volume > MAX_BOX_VOLUME)
            throw new IllegalArgumentException("The box has %d cells; keep it at %d or fewer.".formatted(volume, MAX_BOX_VOLUME));
    }

    private static void requireTimeLimit(JsonObject options) {
        double timeLimit = options.get("timeLimit").getAsDouble();
        if (timeLimit <= 0 || timeLimit > MAX_TIME_LIMIT_SECONDS)
            throw new IllegalArgumentException("The time limit must be between 1 s and 24 h.");
    }

    private static Box boxAt(Cell origin, Cell size) {
        return new Box(origin, origin.plus(size).minus(new Cell(1, 1, 1)));
    }

    // The verifier's failure: which input switch broke and why.
    private static String describeFailure(JsonObject failure) {
        if (failure == null) return "the check failed";
        String reason = failure.get("reason").getAsString();
        String to = failure.get("to").toString();
        if (failure.get("from").isJsonNull()) return "with inputs " + to + ": " + reason;
        return "switching inputs " + failure.get("from") + " -> " + to + ": " + reason;
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
