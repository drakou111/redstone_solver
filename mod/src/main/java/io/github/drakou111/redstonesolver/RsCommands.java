package io.github.drakou111.redstonesolver;

import com.google.gson.JsonObject;
import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.builder.LiteralArgumentBuilder;
import com.mojang.brigadier.context.CommandContext;
import com.mojang.brigadier.exceptions.CommandSyntaxException;
import com.mojang.brigadier.suggestion.SuggestionProvider;
import io.github.drakou111.redstonesolver.PlayerSessions.PendingResult;
import io.github.drakou111.redstonesolver.PlayerSessions.Session;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.InputKind;
import io.github.drakou111.redstonesolver.core.Layout;
import io.github.drakou111.redstonesolver.core.OutputKind;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.TruthRow;
import io.github.drakou111.redstonesolver.solver.SolverException;
import io.github.drakou111.redstonesolver.solver.SolverService;
import io.github.drakou111.redstonesolver.solver.SolverService.SolveOutcome;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import java.util.UUID;
import java.util.function.Consumer;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.commands.SharedSuggestionProvider;
import net.minecraft.commands.arguments.coordinates.BlockPosArgument;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.MutableComponent;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;

/** The /rs command tree. Every handler runs on the server thread. */
public final class RsCommands {
    private static final double PICK_RANGE = 20;
    private static final double MAX_TIME_LIMIT_SECONDS = 24 * 60 * 60;
    private static final int MAX_BOX_VOLUME = 4096;

    private static final String HELP = """
            Redstone Solver: describe a circuit, the solver builds it.
             1. /rs wand, then left/right-click two corners of the box. The bottom layer is for supports.
             2. Look where a port goes (like placing a block) and run
                /rs input IN1 comparator   or   /rs output OUT1 dust
                Repeater and comparator ports point the way you look.
             3. Add truth table rows: /rs row IN1=1..15 -> OUT1=0   (ranges: 0, 15, 1..15, *)
             4. Optional: /rs timing 4 (outputs right within 4 game ticks)
             5. /rs solve [seconds], then click Place.
            /rs show lists everything; /rs remove <name>, /rs rows remove <n>, /rs rows clear edit it.""";

    private final PlayerSessions sessions;
    private final SolverService solver;
    private final ModConfig config;
    private final Item wand;

    public RsCommands(PlayerSessions sessions, SolverService solver, ModConfig config, Item wand) {
        this.sessions = sessions;
        this.solver = solver;
        this.config = config;
        this.wand = wand;
    }

    public void register(CommandDispatcher<CommandSourceStack> dispatcher) {
        SuggestionProvider<CommandSourceStack> inputKinds = (context, builder) -> SharedSuggestionProvider.suggest(
                kindNames(InputKind.values()), builder);
        SuggestionProvider<CommandSourceStack> outputKinds = (context, builder) -> SharedSuggestionProvider.suggest(
                kindNames(OutputKind.values()), builder);
        SuggestionProvider<CommandSourceStack> portNames = (context, builder) -> {
            CircuitDraft draft = this.draftOf(context);
            List<String> names = new ArrayList<>();
            for (Port port : draft.getInputs()) names.add(port.name());
            for (Port port : draft.getOutputs()) names.add(port.name());
            return SharedSuggestionProvider.suggest(names, builder);
        };

        LiteralArgumentBuilder<CommandSourceStack> root = Commands.literal("rs")
                .requires(Commands.hasPermission(Commands.LEVEL_GAMEMASTERS))
                .executes(context -> this.help(context))
                .then(Commands.literal("help").executes(context -> this.help(context)))
                .then(Commands.literal("wand").executes(context -> this.giveWand(context)))
                .then(Commands.literal("box")
                        .then(Commands.argument("from", BlockPosArgument.blockPos())
                                .then(Commands.argument("to", BlockPosArgument.blockPos())
                                        .executes(context -> this.setBox(context)))))
                .then(Commands.literal("input")
                        .then(Commands.argument("name", StringArgumentType.word())
                                .then(Commands.argument("kind", StringArgumentType.word()).suggests(inputKinds)
                                        .executes(context -> this.addPort(context, true, false))
                                        .then(Commands.argument("at", BlockPosArgument.blockPos())
                                                .executes(context -> this.addPort(context, true, true))))))
                .then(Commands.literal("output")
                        .then(Commands.argument("name", StringArgumentType.word())
                                .then(Commands.argument("kind", StringArgumentType.word()).suggests(outputKinds)
                                        .executes(context -> this.addPort(context, false, false))
                                        .then(Commands.argument("at", BlockPosArgument.blockPos())
                                                .executes(context -> this.addPort(context, false, true))))))
                .then(Commands.literal("remove")
                        .then(Commands.argument("name", StringArgumentType.word()).suggests(portNames)
                                .executes(context -> this.removePort(context))))
                .then(Commands.literal("row")
                        .then(Commands.argument("row", StringArgumentType.greedyString())
                                .executes(context -> this.addRow(context))))
                .then(Commands.literal("rows")
                        .executes(context -> this.show(context))
                        .then(Commands.literal("clear").executes(context -> this.clearRows(context)))
                        .then(Commands.literal("remove")
                                .then(Commands.argument("number", IntegerArgumentType.integer(1))
                                        .executes(context -> this.removeRow(context)))))
                .then(Commands.literal("timing")
                        .then(Commands.literal("any").executes(context -> this.setTiming(context, false)))
                        .then(Commands.argument("ticks", IntegerArgumentType.integer(0))
                                .executes(context -> this.setTiming(context, true))))
                .then(Commands.literal("show").executes(context -> this.show(context)))
                .then(Commands.literal("solve")
                        .executes(context -> this.solve(context, this.config.defaultTimeLimitSeconds()))
                        .then(Commands.argument("seconds", DoubleArgumentType.doubleArg(1, MAX_TIME_LIMIT_SECONDS))
                                .executes(context -> this.solve(context,
                                        DoubleArgumentType.getDouble(context, "seconds")))))
                .then(Commands.literal("cancel").executes(context -> this.cancel(context)))
                .then(Commands.literal("place").executes(context -> this.place(context)))
                .then(Commands.literal("discard").executes(context -> this.discard(context)));
        dispatcher.register(root);
    }

    private int help(CommandContext<CommandSourceStack> context) {
        context.getSource().sendSystemMessage(Component.literal(HELP));
        return 1;
    }

    private int giveWand(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        player.getInventory().add(new ItemStack(this.wand));
        player.sendSystemMessage(Chat.info("Left-click one corner of the box, right-click the opposite corner."));
        return 1;
    }

    private int setBox(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        CircuitDraft draft = this.draftOf(context);
        draft.setCorner1(cell(BlockPosArgument.getBlockPos(context, "from")));
        draft.setCorner2(cell(BlockPosArgument.getBlockPos(context, "to")));
        Box box = draft.requireBox();
        Cell size = box.size();
        return this.ok(context, "Box: %d x %d x %d.".formatted(size.x(), size.y(), size.z()));
    }

    private int addPort(CommandContext<CommandSourceStack> context, boolean input, boolean explicitPosition)
            throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        String name = StringArgumentType.getString(context, "name");
        String kindName = StringArgumentType.getString(context, "kind");
        Optional<Cell> target = explicitPosition
                ? Optional.of(cell(BlockPosArgument.getBlockPos(context, "at")))
                : lookedAtPlacement(player);
        if (target.isEmpty()) return this.fail(context, "Look at a block within reach: the port goes where a block would be placed.");
        Dir facing = toDir(player.getDirection());

        Port port;
        if (input) {
            Optional<InputKind> kind = InputKind.parse(kindName);
            if (kind.isEmpty()) return this.fail(context, "Input kinds: " + String.join(", ", kindNames(InputKind.values())));
            port = Port.input(name, kind.get(), target.get(), facing);
        } else {
            Optional<OutputKind> kind = OutputKind.parse(kindName);
            if (kind.isEmpty()) return this.fail(context, "Output kinds: " + String.join(", ", kindNames(OutputKind.values())));
            port = Port.output(name, kind.get(), target.get(), facing);
        }
        try {
            this.draftOf(context).putPort(port);
        } catch (IllegalArgumentException | IllegalStateException e) {
            return this.fail(context, e.getMessage());
        }
        return this.ok(context, (input ? "Input " : "Output ") + port.describe());
    }

    private int removePort(CommandContext<CommandSourceStack> context) {
        String name = StringArgumentType.getString(context, "name");
        if (!this.draftOf(context).removePort(name)) return this.fail(context, "There is no port named " + name);
        return this.ok(context, "Removed " + name + ". Rows that name it no longer match; check /rs show.");
    }

    private int addRow(CommandContext<CommandSourceStack> context) {
        try {
            TruthRow row = TruthRow.parse(StringArgumentType.getString(context, "row"));
            CircuitDraft draft = this.draftOf(context);
            draft.addRow(row);
            return this.ok(context, "Row %d: %s".formatted(draft.getRows().size(), row));
        } catch (IllegalArgumentException e) {
            return this.fail(context, e.getMessage());
        }
    }

    private int removeRow(CommandContext<CommandSourceStack> context) {
        int number = IntegerArgumentType.getInteger(context, "number");
        if (!this.draftOf(context).removeRow(number - 1)) return this.fail(context, "There is no row " + number);
        return this.ok(context, "Removed row " + number + ".");
    }

    private int clearRows(CommandContext<CommandSourceStack> context) {
        this.draftOf(context).clearRows();
        return this.ok(context, "Cleared the truth table.");
    }

    private int setTiming(CommandContext<CommandSourceStack> context, boolean hasTicks) {
        Optional<Integer> ticks = hasTicks
                ? Optional.of(IntegerArgumentType.getInteger(context, "ticks"))
                : Optional.empty();
        this.draftOf(context).setLatestTicks(ticks);
        return this.ok(context, ticks.map(t -> "Outputs must be right within " + t + " game ticks.")
                .orElse("Outputs may take any time to settle."));
    }

    private int show(CommandContext<CommandSourceStack> context) {
        CircuitDraft draft = this.draftOf(context);
        CommandSourceStack source = context.getSource();
        Optional<Box> box = draft.getBox();
        source.sendSystemMessage(Chat.info(box.map(b -> "Box: %s to %s (%s)".formatted(b.min(), b.max(),
                b.size().x() + "x" + b.size().y() + "x" + b.size().z())).orElse("No box yet (/rs wand).")));
        for (Port port : draft.getInputs()) source.sendSystemMessage(Chat.hint("  input  " + port.describe()));
        for (Port port : draft.getOutputs()) source.sendSystemMessage(Chat.hint("  output " + port.describe()));
        List<TruthRow> rows = draft.getRows();
        if (rows.isEmpty()) source.sendSystemMessage(Chat.hint("  no rows yet (/rs row IN1=1..15 -> OUT1=0)"));
        for (int i = 0; i < rows.size(); i++)
            source.sendSystemMessage(Chat.hint("  row %d: %s".formatted(i + 1, rows.get(i))));
        source.sendSystemMessage(Chat.hint("  timing: " + draft.getLatestTicks().map(t -> "within " + t + " game ticks")
                .orElse("any")));
        return 1;
    }

    private int solve(CommandContext<CommandSourceStack> context, double seconds) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        CircuitDraft draft = this.draftOf(context);
        JsonObject spec;
        Box box;
        try {
            box = draft.requireBox();
            if (box.volume() > MAX_BOX_VOLUME)
                return this.fail(context, "The box has %d cells; keep it at %d or fewer.".formatted(box.volume(), MAX_BOX_VOLUME));
            spec = draft.toSpecJson();
        } catch (IllegalArgumentException | IllegalStateException e) {
            return this.fail(context, e.getMessage());
        }

        MinecraftServer server = context.getSource().getServer();
        UUID playerId = player.getUUID();
        SolverService.Listener listener = new SolverService.Listener() {
            @Override
            public void progress(double elapsed, String lastLogLine) {
                server.execute(() -> withPlayer(server, playerId, p -> p.sendOverlayMessage(
                        Chat.hint("Solving... %.0f s  %s".formatted(elapsed, lastLogLine)))));
            }

            @Override
            public void finished(SolveOutcome outcome) {
                server.execute(() -> RsCommands.this.finishSolve(server, playerId, box, outcome));
            }
        };
        try {
            this.solver.startSolve(spec, seconds, listener);
        } catch (SolverException e) {
            return this.fail(context, e.getMessage());
        }
        this.sessions.get(playerId).setPending(Optional.empty());
        return this.ok(context, "Solving (up to %.0f s). /rs cancel stops it.".formatted(seconds));
    }

    private void finishSolve(MinecraftServer server, UUID playerId, Box box, SolveOutcome outcome) {
        Optional<ServerPlayer> player = Optional.ofNullable(server.getPlayerList().getPlayer(playerId));
        if (outcome.error().isPresent()) {
            player.ifPresent(p -> p.sendSystemMessage(Chat.error("Solve failed: " + outcome.error().get())));
            return;
        }
        if (outcome.layout().isEmpty()) {
            player.ifPresent(p -> p.sendSystemMessage(Chat.error(
                    "No circuit found (%s after %.0f s). Try a bigger box, looser timing or more time."
                            .formatted(outcome.status(), outcome.seconds()))));
            return;
        }
        Layout layout = outcome.layout().get();
        String summary = "%s: %d blocks in %.1f s%s".formatted(outcome.optimal() ? "Smallest circuit" : "Circuit found",
                layout.countPlacedBlocks(), outcome.seconds(), outcome.optimal() ? "" : " (maybe not the smallest)");
        Session session = this.sessions.get(playerId);
        session.setPending(Optional.of(new PendingResult(layout, box, summary)));
        player.ifPresent(p -> {
            boolean occupied = LayoutPlacer.hasBlocks(p.level(), box);
            MutableComponent message = Chat.good(summary + " ").append(Chat.button(occupied ? "Replace box" : "Place",
                    "/rs place", occupied ? "Clears the box (your build in it is lost) and builds the circuit"
                            : "Builds the circuit in the box"))
                    .append(Component.literal(" ")).append(Chat.button("Discard", "/rs discard", "Forget this circuit"));
            p.sendSystemMessage(message);
        });
    }

    private int cancel(CommandContext<CommandSourceStack> context) {
        try {
            this.solver.cancel();
        } catch (SolverException e) {
            return this.fail(context, e.getMessage());
        }
        return this.ok(context, "Cancelling; the best circuit so far (if any) still comes back.");
    }

    private int place(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        Session session = this.sessions.get(player.getUUID());
        Optional<PendingResult> pending = session.getPending();
        if (pending.isEmpty()) return this.fail(context, "No solved circuit to place. Run /rs solve first.");
        try {
            LayoutPlacer.place(player.level(), pending.get().box(), pending.get().layout());
        } catch (IllegalArgumentException | IllegalStateException e) {
            return this.fail(context, "Cannot place: " + e.getMessage());
        }
        session.setPending(Optional.empty());
        return this.ok(context, "Placed. Drive the inputs with levers to test it.");
    }

    private int discard(CommandContext<CommandSourceStack> context) throws CommandSyntaxException {
        ServerPlayer player = context.getSource().getPlayerOrException();
        this.sessions.get(player.getUUID()).setPending(Optional.empty());
        return this.ok(context, "Discarded the circuit.");
    }

    private CircuitDraft draftOf(CommandContext<CommandSourceStack> context) {
        ServerPlayer player = context.getSource().getPlayer();
        if (player == null) throw new IllegalStateException("only players can use /rs");
        return this.sessions.get(player.getUUID()).getDraft();
    }

    private int ok(CommandContext<CommandSourceStack> context, String message) {
        context.getSource().sendSystemMessage(Chat.info(message));
        return 1;
    }

    private int fail(CommandContext<CommandSourceStack> context, String message) {
        context.getSource().sendSystemMessage(Chat.error(message));
        return 0;
    }

    private static void withPlayer(MinecraftServer server, UUID id, Consumer<ServerPlayer> action) {
        ServerPlayer player = server.getPlayerList().getPlayer(id);
        if (player != null) action.accept(player);
    }

    private static Optional<Cell> lookedAtPlacement(ServerPlayer player) {
        HitResult hit = player.pick(PICK_RANGE, 0, false);
        if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK) return Optional.empty();
        return Optional.of(cell(blockHit.getBlockPos().relative(blockHit.getDirection())));
    }

    private static Cell cell(BlockPos pos) {
        return new Cell(pos.getX(), pos.getY(), pos.getZ());
    }

    private static Dir toDir(Direction direction) {
        return Dir.parse(direction.getSerializedName())
                .orElseThrow(() -> new IllegalStateException("unknown direction " + direction));
    }

    private static List<String> kindNames(Enum<?>[] kinds) {
        List<String> names = new ArrayList<>();
        for (Enum<?> kind : kinds) names.add(kind.name().toLowerCase(Locale.ROOT));
        return names;
    }
}
