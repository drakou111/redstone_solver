package io.github.drakou111.redstonesolver.client;

import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.arguments.DoubleArgumentType;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.context.CommandContext;
import com.mojang.brigadier.suggestion.SuggestionProvider;
import io.github.drakou111.redstonesolver.Chat;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.InputKind;
import io.github.drakou111.redstonesolver.core.OutputKind;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.TruthRow;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.command.v2.FabricClientCommandSource;
import net.minecraft.commands.SharedSuggestionProvider;
import net.minecraft.core.Direction;
import net.minecraft.network.chat.Component;

/** The client-side /rs commands: a typed alternative to the keys and the Solver panel. */
public final class ClientRsCommands {
    private static final double MAX_TIME_LIMIT_SECONDS = 24 * 60 * 60;
    private static final String HELP = """
            Redstone Solver: describe a circuit, the solver builds it.
             J / K: set the box corners at the block you look at (bottom layer = supports).
             /rs input IN1 comparator, /rs output OUT1 dust: a port where a block would be placed,
               facing the way you look.
             /rs row IN1=1..15 -> OUT1=0   (ranges: 0, 15, 1..15, *)
             /rs timing 4 | any,  /rs solve [seconds],  /rs cancel,  /rs place,  /rs discard,  /rs shrink
             /rs show, /rs remove <port>, /rs rows remove <n>, /rs rows clear, /rs outlines""";

    private final ClientState state;
    private final ServerLink server;

    public ClientRsCommands(ClientState state, ServerLink server) {
        this.state = state;
        this.server = server;
    }

    public void register(CommandDispatcher<FabricClientCommandSource> dispatcher) {
        SuggestionProvider<FabricClientCommandSource> inputKinds = (context, builder) -> SharedSuggestionProvider.suggest(
                kindNames(InputKind.values()), builder);
        SuggestionProvider<FabricClientCommandSource> outputKinds = (context, builder) -> SharedSuggestionProvider.suggest(
                kindNames(OutputKind.values()), builder);
        SuggestionProvider<FabricClientCommandSource> portNames = (context, builder) -> {
            List<String> names = new ArrayList<>();
            for (Port port : this.state.getDraft().getInputs()) names.add(port.name());
            for (Port port : this.state.getDraft().getOutputs()) names.add(port.name());
            return SharedSuggestionProvider.suggest(names, builder);
        };

        dispatcher.register(ClientCommands.literal("rs")
                .executes(this::help)
                .then(ClientCommands.literal("help").executes(this::help))
                .then(ClientCommands.literal("input")
                        .then(ClientCommands.argument("name", StringArgumentType.word())
                                .then(ClientCommands.argument("kind", StringArgumentType.word()).suggests(inputKinds)
                                        .executes(context -> this.addPort(context, true)))))
                .then(ClientCommands.literal("output")
                        .then(ClientCommands.argument("name", StringArgumentType.word())
                                .then(ClientCommands.argument("kind", StringArgumentType.word()).suggests(outputKinds)
                                        .executes(context -> this.addPort(context, false)))))
                .then(ClientCommands.literal("remove")
                        .then(ClientCommands.argument("name", StringArgumentType.word()).suggests(portNames)
                                .executes(this::removePort)))
                .then(ClientCommands.literal("row")
                        .then(ClientCommands.argument("row", StringArgumentType.greedyString()).executes(this::addRow)))
                .then(ClientCommands.literal("rows")
                        .executes(this::show)
                        .then(ClientCommands.literal("clear").executes(this::clearRows))
                        .then(ClientCommands.literal("remove")
                                .then(ClientCommands.argument("number", IntegerArgumentType.integer(1))
                                        .executes(this::removeRow))))
                .then(ClientCommands.literal("timing")
                        .then(ClientCommands.literal("any").executes(context -> this.setTiming(context, false)))
                        .then(ClientCommands.argument("ticks", IntegerArgumentType.integer(0))
                                .executes(context -> this.setTiming(context, true))))
                .then(ClientCommands.literal("show").executes(this::show))
                .then(ClientCommands.literal("outlines").executes(this::toggleOutlines))
                .then(ClientCommands.literal("solve")
                        .executes(context -> this.solve(context, Optional.empty()))
                        .then(ClientCommands.argument("seconds", DoubleArgumentType.doubleArg(1, MAX_TIME_LIMIT_SECONDS))
                                .executes(context -> this.solve(context,
                                        Optional.of(DoubleArgumentType.getDouble(context, "seconds"))))))
                .then(ClientCommands.literal("shrink").executes(context -> this.run(context, this.server::shrink,
                        "Shrinking the last circuit.")))
                .then(ClientCommands.literal("cancel").executes(context -> this.run(context, this.server::cancel,
                        "Cancelling; the best circuit so far (if any) still comes back.")))
                .then(ClientCommands.literal("place").executes(context -> this.run(context, this.server::place, "")))
                .then(ClientCommands.literal("discard").executes(context -> this.run(context, this.server::discard,
                        "Discarded the circuit."))));
    }

    private int help(CommandContext<FabricClientCommandSource> context) {
        context.getSource().sendFeedback(Component.literal(HELP));
        return 1;
    }

    private int addPort(CommandContext<FabricClientCommandSource> context, boolean input) {
        FabricClientCommandSource source = context.getSource();
        String name = StringArgumentType.getString(context, "name");
        String kindName = StringArgumentType.getString(context, "kind");
        Optional<Cell> target = SolverKeys.lookedAtPlacement(source.getClient());
        if (target.isEmpty()) return fail(context, "Look at a block: the port goes where a block would be placed.");
        Dir facing = toDir(source.getPlayer().getDirection());
        try {
            Port port;
            if (input) {
                InputKind kind = InputKind.parse(kindName).orElseThrow(() -> new IllegalArgumentException(
                        "Input kinds: " + String.join(", ", kindNames(InputKind.values()))));
                port = Port.input(name, kind, target.get(), facing);
            } else {
                OutputKind kind = OutputKind.parse(kindName).orElseThrow(() -> new IllegalArgumentException(
                        "Output kinds: " + String.join(", ", kindNames(OutputKind.values()))));
                port = Port.output(name, kind, target.get(), facing);
            }
            this.state.getDraft().putPort(port);
            return ok(context, (input ? "Input " : "Output ") + port.describe());
        } catch (IllegalArgumentException | IllegalStateException e) {
            return fail(context, e.getMessage());
        }
    }

    private int removePort(CommandContext<FabricClientCommandSource> context) {
        String name = StringArgumentType.getString(context, "name");
        if (!this.state.getDraft().removePort(name)) return fail(context, "There is no port named " + name);
        return ok(context, "Removed " + name + ".");
    }

    private int addRow(CommandContext<FabricClientCommandSource> context) {
        try {
            TruthRow row = TruthRow.parse(StringArgumentType.getString(context, "row"));
            this.state.getDraft().addRow(row);
            return ok(context, "Row %d: %s".formatted(this.state.getDraft().getRows().size(), row));
        } catch (IllegalArgumentException e) {
            return fail(context, e.getMessage());
        }
    }

    private int removeRow(CommandContext<FabricClientCommandSource> context) {
        int number = IntegerArgumentType.getInteger(context, "number");
        if (!this.state.getDraft().removeRow(number - 1)) return fail(context, "There is no row " + number);
        return ok(context, "Removed row " + number + ".");
    }

    private int clearRows(CommandContext<FabricClientCommandSource> context) {
        this.state.getDraft().clearRows();
        return ok(context, "Cleared the truth table.");
    }

    private int setTiming(CommandContext<FabricClientCommandSource> context, boolean hasTicks) {
        Optional<Integer> ticks = hasTicks ? Optional.of(IntegerArgumentType.getInteger(context, "ticks")) : Optional.empty();
        this.state.getDraft().setLatestTicks(ticks);
        return ok(context, ticks.map(t -> "Outputs must be right within " + t + " game ticks.")
                .orElse("Outputs may take any time to settle."));
    }

    private int toggleOutlines(CommandContext<FabricClientCommandSource> context) {
        this.state.setShowOutlines(!this.state.isShowOutlines());
        return ok(context, this.state.isShowOutlines() ? "Outlines on." : "Outlines off.");
    }

    private int show(CommandContext<FabricClientCommandSource> context) {
        CircuitDraft draft = this.state.getDraft();
        FabricClientCommandSource source = context.getSource();
        Optional<Box> box = draft.getBox();
        source.sendFeedback(Chat.info(box.map(b -> "Box: %s to %s (%dx%dx%d)".formatted(b.min(), b.max(), b.size().x(),
                b.size().y(), b.size().z())).orElse("No box yet: press J and K on two corners.")));
        for (Port port : draft.getInputs()) source.sendFeedback(Chat.hint("  input  " + port.describe()));
        for (Port port : draft.getOutputs()) source.sendFeedback(Chat.hint("  output " + port.describe()));
        List<TruthRow> rows = draft.getRows();
        if (rows.isEmpty()) source.sendFeedback(Chat.hint("  no rows yet (/rs row IN1=1..15 -> OUT1=0)"));
        for (int i = 0; i < rows.size(); i++) source.sendFeedback(Chat.hint("  row %d: %s".formatted(i + 1, rows.get(i))));
        source.sendFeedback(Chat.hint("  timing: " + draft.getLatestTicks().map(t -> "within " + t + " game ticks").orElse("any")));
        return 1;
    }

    private int solve(CommandContext<FabricClientCommandSource> context, Optional<Double> seconds) {
        try {
            seconds.ifPresent(this.state::setTimeLimitSeconds);
            this.server.solve();
        } catch (IllegalArgumentException | IllegalStateException e) {
            return fail(context, e.getMessage());
        }
        return ok(context, "Solving (up to %.0f s). /rs cancel stops it.".formatted(this.state.getTimeLimitSeconds()));
    }

    private int run(CommandContext<FabricClientCommandSource> context, Runnable action, String message) {
        try {
            action.run();
        } catch (IllegalStateException e) {
            return fail(context, e.getMessage());
        }
        if (!message.isEmpty()) ok(context, message);
        return 1;
    }

    private static int ok(CommandContext<FabricClientCommandSource> context, String message) {
        context.getSource().sendFeedback(Chat.info(message));
        return 1;
    }

    private static int fail(CommandContext<FabricClientCommandSource> context, String message) {
        context.getSource().sendError(Chat.error(message));
        return 0;
    }

    static Dir toDir(Direction direction) {
        return Dir.parse(direction.getSerializedName())
                .orElseThrow(() -> new IllegalStateException("unknown direction " + direction));
    }

    private static List<String> kindNames(Enum<?>[] kinds) {
        List<String> names = new ArrayList<>();
        for (Enum<?> kind : kinds) names.add(kind.name().toLowerCase(Locale.ROOT));
        return names;
    }
}
