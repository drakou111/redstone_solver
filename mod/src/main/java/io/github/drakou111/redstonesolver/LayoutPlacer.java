package io.github.drakou111.redstonesolver;

import com.mojang.brigadier.exceptions.CommandSyntaxException;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.ContainerFill;
import io.github.drakou111.redstonesolver.core.JavaBlockStates;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.commands.arguments.blocks.BlockStateParser;
import net.minecraft.core.BlockPos;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.Container;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;

/** Builds a solved layout into the world, replacing everything in its box. */
public final class LayoutPlacer {
    private LayoutPlacer() {
    }

    /** True when any cell of the box holds something other than air. */
    public static boolean hasBlocks(ServerLevel level, Box box) {
        for (BlockPos pos : positions(box)) {
            if (!level.getBlockState(pos).isAir()) return true;
        }
        return false;
    }

    /** Clears the box, then places supports before the blocks that need them, like the setblock export. */
    public static void place(ServerLevel level, Box box, Layout layout) {
        if (!layout.size().equals(box.size()))
            throw new IllegalArgumentException("the circuit is %s but the box is %s".formatted(layout.size(), box.size()));
        for (BlockPos pos : positions(box)) {
            if (!level.isLoaded(pos)) throw new IllegalStateException("part of the box is not loaded");
        }

        HolderLookup<Block> blocks = level.registryAccess().lookupOrThrow(Registries.BLOCK);
        List<Placed> placed = new ArrayList<>();
        for (Layout.Block block : layout.blocks()) {
            JavaBlockStates.stateFor(block).ifPresent(text -> placed.add(new Placed(block, worldPos(box, block.position()),
                    parse(blocks, text))));
        }

        // Clearing drops nothing: neither the old blocks nor what containers held.
        for (BlockPos pos : positions(box)) {
            level.setBlock(pos, Blocks.AIR.defaultBlockState(),
                    Block.UPDATE_ALL | Block.UPDATE_SUPPRESS_DROPS | Block.UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS);
        }
        for (boolean supportedPass : new boolean[] {false, true}) {
            for (Placed entry : placed) {
                if (JavaBlockStates.needsSupport(entry.block()) == supportedPass)
                    level.setBlock(entry.pos(), entry.state(), Block.UPDATE_ALL);
            }
        }
        // Dust was placed as a cross; give it the shape its final neighbors call for.
        for (Placed entry : placed) {
            BlockState current = level.getBlockState(entry.pos());
            if (current.is(Blocks.REDSTONE_WIRE))
                level.setBlock(entry.pos(), Block.updateFromNeighbourShapes(current, level, entry.pos()), Block.UPDATE_ALL);
        }
        for (Placed entry : placed) {
            if (JavaBlockStates.isContainer(entry.block())) fill(level, entry.pos(), entry.block().signal());
        }
    }

    private record Placed(Layout.Block block, BlockPos pos, BlockState state) {
    }

    private static void fill(ServerLevel level, BlockPos pos, int signal) {
        BlockEntity entity = level.getBlockEntity(pos);
        if (!(entity instanceof Container container))
            throw new IllegalStateException("expected a container at " + pos.toShortString());
        List<Integer> stacks = ContainerFill.stacksForSignal(signal, container.getContainerSize());
        for (int slot = 0; slot < stacks.size(); slot++) container.setItem(slot, new ItemStack(Items.STONE, stacks.get(slot)));
        container.setChanged();
    }

    private static BlockState parse(HolderLookup<Block> blocks, String text) {
        try {
            return BlockStateParser.parseForBlock(blocks, text, false).blockState();
        } catch (CommandSyntaxException e) {
            throw new IllegalStateException("bad block state '" + text + "': " + e.getMessage(), e);
        }
    }

    private static BlockPos worldPos(Box box, Cell relative) {
        Cell cell = box.min().plus(relative);
        return new BlockPos(cell.x(), cell.y(), cell.z());
    }

    private static List<BlockPos> positions(Box box) {
        List<BlockPos> positions = new ArrayList<>(box.volume());
        for (int x = box.min().x(); x <= box.max().x(); x++) {
            for (int y = box.min().y(); y <= box.max().y(); y++) {
                for (int z = box.min().z(); z <= box.max().z(); z++) positions.add(new BlockPos(x, y, z));
            }
        }
        return positions;
    }
}
