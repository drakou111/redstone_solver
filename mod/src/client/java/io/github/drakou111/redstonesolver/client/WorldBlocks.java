package io.github.drakou111.redstonesolver.client;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.PortBlock;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.tags.BlockTags;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.block.state.properties.ComparatorMode;

/** Reads world blocks as the solver's blocks, for kept cells and sketches. */
public final class WorldBlocks implements CircuitDraft.BlockSource {
    private final Level level;

    public WorldBlocks(Level level) {
        this.level = level;
    }

    @Override
    public Optional<JsonObject> blockAt(Cell cell) {
        BlockPos pos = new BlockPos(cell.x(), cell.y(), cell.z());
        BlockState state = this.level.getBlockState(pos);
        if (state.isAir()) return Optional.empty();
        return Optional.of(describe(state, pos));
    }

    /** What port the block in this cell can be, if it is a repeater, comparator, dust or barrel. */
    public Optional<PortBlock> portBlockAt(Cell cell) {
        BlockPos pos = new BlockPos(cell.x(), cell.y(), cell.z());
        BlockState state = this.level.getBlockState(pos);
        if (state.is(Blocks.BARREL)) return PortBlock.fromSolverBlock(type("barrel"));
        if (!state.is(Blocks.REPEATER) && !state.is(Blocks.COMPARATOR) && !state.is(Blocks.REDSTONE_WIRE))
            return Optional.empty();
        return PortBlock.fromSolverBlock(this.describe(state, pos));
    }

    /** A short name for messages: the solver type if known, else the block id. */
    public String nameAt(Cell cell) {
        BlockPos pos = new BlockPos(cell.x(), cell.y(), cell.z());
        BlockState state = this.level.getBlockState(pos);
        if (state.isAir()) return "air";
        try {
            return describe(state, pos).get("type").getAsString();
        } catch (IllegalArgumentException e) {
            return BuiltInRegistries.BLOCK.getKey(state.getBlock()).getPath();
        }
    }

    private JsonObject describe(BlockState state, BlockPos pos) {
        if (state.is(Blocks.REDSTONE_WIRE)) return type("dust");
        if (state.is(Blocks.REDSTONE_TORCH)) return facing(type("torch"), Dir.DOWN);
        // Java's wall torch faces away from its wall; the solver names the wall.
        if (state.is(Blocks.REDSTONE_WALL_TORCH))
            return facing(type("torch"), dir(state.getValue(BlockStateProperties.HORIZONTAL_FACING)).opposite());
        // Java's diode facing points at its input; the solver's points where the signal goes.
        if (state.is(Blocks.REPEATER)) {
            JsonObject json = facing(type("repeater"), dir(state.getValue(BlockStateProperties.HORIZONTAL_FACING)).opposite());
            json.addProperty("delay", state.getValue(BlockStateProperties.DELAY));
            return json;
        }
        if (state.is(Blocks.COMPARATOR)) {
            JsonObject json = facing(type("comparator"), dir(state.getValue(BlockStateProperties.HORIZONTAL_FACING)).opposite());
            json.addProperty("mode", state.getValue(BlockStateProperties.MODE_COMPARATOR) == ComparatorMode.SUBTRACT
                    ? "subtract" : "compare");
            return json;
        }
        if (state.is(Blocks.OBSERVER))
            throw new IllegalArgumentException("observers are not used; remove the one at " + pos.toShortString());
        if (state.is(Blocks.REDSTONE_BLOCK)) return type("redstone_block");
        if (state.is(Blocks.TARGET)) return type("target");
        if (state.is(Blocks.NOTE_BLOCK)) return type("note_block");
        if (state.is(Blocks.HOPPER) || state.is(Blocks.BARREL) || state.is(Blocks.CHEST))
            throw new IllegalArgumentException("containers in the box cannot be kept or sketched yet (at "
                    + pos.toShortString() + ")");
        if (state.is(BlockTags.IMPERMEABLE)) return type("glass");
        if (state.isRedstoneConductor(this.level, pos)) return type("solid");
        throw new IllegalArgumentException("the solver does not know " + BuiltInRegistries.BLOCK.getKey(state.getBlock())
                + " at " + pos.toShortString() + "; remove it or mark the cell empty");
    }

    private static JsonObject type(String name) {
        JsonObject json = new JsonObject();
        json.addProperty("type", name);
        return json;
    }

    private static JsonObject facing(JsonObject json, Dir dir) {
        json.addProperty("facing", dir.jsonName());
        return json;
    }

    private static Dir dir(Direction direction) {
        return ClientRsCommands.toDir(direction);
    }
}
