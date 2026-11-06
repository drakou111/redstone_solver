package io.github.drakou111.redstonesolver.core;

import java.util.Optional;

/**
 * Java Edition block states for the solver's blocks, matching the setblock export of the C++
 * report (src/redstone/report.cpp). Dust is given as a cross; its real shape comes from its
 * neighbors once placed.
 */
public final class JavaBlockStates {
    public static final String SUPPORT_BLOCK = "minecraft:cyan_wool";
    public static final String GLASS_BLOCK = "minecraft:cyan_stained_glass";
    public static final String DUST_CROSS = "minecraft:redstone_wire[north=side,south=side,east=side,west=side]";

    private JavaBlockStates() {
    }

    /** The state string to place, or empty for blocks that are not placed (air). */
    public static Optional<String> stateFor(Layout.Block block) {
        return Optional.ofNullable(switch (block.type()) {
            case "solid" -> SUPPORT_BLOCK;
            case "glass" -> GLASS_BLOCK;
            case "redstone_block" -> "minecraft:redstone_block";
            case "target" -> "minecraft:target";
            case "note_block" -> "minecraft:note_block";
            case "hopper" -> "minecraft:hopper[facing=down]";
            case "barrel" -> "minecraft:barrel[facing=up]";
            case "chest" -> "minecraft:chest[facing=north]";
            case "observer" -> "minecraft:observer[facing=%s]".formatted(block.requireFacing().jsonName());
            case "dust" -> DUST_CROSS;
            case "torch" -> torch(block.requireFacing());
            case "repeater" -> repeater(block.requireFacing(), block.delay());
            case "comparator" -> comparator(block.requireFacing(), block.mode());
            case "input" -> inputPort(block);
            case "output" -> outputPort(block);
            case "air" -> null;
            default -> throw new IllegalArgumentException("the mod cannot place solver block '" + block.type() + "'");
        });
    }

    /** Blocks that pop off without a block under or behind them; they are placed last. */
    public static boolean needsSupport(Layout.Block block) {
        return switch (block.type()) {
            case "dust", "torch", "repeater", "comparator", "input", "output" -> true;
            default -> false;
        };
    }

    /** Containers get filled to their comparator signal after placing. */
    public static boolean isContainer(Layout.Block block) {
        return switch (block.type()) {
            case "hopper", "barrel", "chest" -> true;
            case "input" -> block.kind().equals("container");
            default -> false;
        };
    }

    // The solver's torch facing points at the block it hangs on; Java's points away from it.
    private static String torch(Dir attachedTo) {
        if (attachedTo == Dir.DOWN) return "minecraft:redstone_torch";
        if (!attachedTo.isHorizontal()) throw new IllegalArgumentException("a torch cannot hang from a ceiling");
        return "minecraft:redstone_wall_torch[facing=%s]".formatted(attachedTo.opposite().jsonName());
    }

    // Java's diode facing points at its input, opposite to the signal.
    private static String repeater(Dir signal, int delay) {
        return "minecraft:repeater[facing=%s,delay=%d]".formatted(signal.opposite().jsonName(), delay);
    }

    private static String comparator(Dir signal, String mode) {
        return "minecraft:comparator[facing=%s,mode=%s]".formatted(signal.opposite().jsonName(), mode);
    }

    // Repeater and comparator inputs are placed too, so a lever behind them drives the circuit.
    private static String inputPort(Layout.Block block) {
        return switch (block.kind()) {
            case "dust" -> DUST_CROSS;
            case "container" -> "minecraft:barrel[facing=up]";
            case "repeater" -> repeater(block.requireFacing(), 1);
            case "comparator" -> comparator(block.requireFacing(), "compare");
            default -> throw new IllegalArgumentException("unknown input kind '" + block.kind() + "'");
        };
    }

    private static String outputPort(Layout.Block block) {
        return switch (block.kind()) {
            case "dust" -> DUST_CROSS;
            case "repeater" -> repeater(block.requireFacing(), 1);
            case "comparator" -> comparator(block.requireFacing(), "compare");
            default -> throw new IllegalArgumentException("unknown output kind '" + block.kind() + "'");
        };
    }
}
