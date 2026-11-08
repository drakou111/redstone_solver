package io.github.drakou111.redstonesolver.client;

import com.mojang.blaze3d.platform.InputConstants;
import io.github.drakou111.redstonesolver.Chat;
import io.github.drakou111.redstonesolver.RedstoneSolverMod;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import io.github.drakou111.redstonesolver.core.PortBlock;
import java.util.Optional;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.keymapping.v1.KeyMappingHelper;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.resources.Identifier;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;

/** The mod's keys (rebindable under Controls): box corners, I/O ports, point marks and panel. */
public final class SolverKeys {
    private final ClientState state;
    private final ServerLink server;
    private final DraftFiles files;
    private final KeyMapping corner1;
    private final KeyMapping corner2;
    private final KeyMapping inputPort;
    private final KeyMapping outputPort;
    private final KeyMapping cellMenu;
    private final KeyMapping panel;

    public SolverKeys(ClientState state, ServerLink server, DraftFiles files) {
        this.state = state;
        this.server = server;
        this.files = files;
        KeyMapping.Category category = KeyMapping.Category.register(
                Identifier.fromNamespaceAndPath(RedstoneSolverMod.MOD_ID, "main"));
        this.corner1 = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.corner1", InputConstants.KEY_J, category));
        this.corner2 = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.corner2", InputConstants.KEY_K, category));
        this.inputPort = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.input", InputConstants.KEY_I, category));
        this.outputPort = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.output", InputConstants.KEY_O, category));
        this.cellMenu = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.cell", InputConstants.KEY_P, category));
        this.panel = KeyMappingHelper.registerKeyMapping(
                new KeyMapping("key.redstone_solver.panel", InputConstants.KEY_R, category));
    }

    public void register() {
        ClientTickEvents.END_CLIENT_TICK.register(this::tick);
    }

    private void tick(Minecraft client) {
        while (this.corner1.consumeClick()) this.setCorner(client, true);
        while (this.corner2.consumeClick()) this.setCorner(client, false);
        while (this.inputPort.consumeClick()) this.setPort(client, true);
        while (this.outputPort.consumeClick()) this.setPort(client, false);
        while (this.cellMenu.consumeClick()) this.openCellMenu(client);
        while (this.panel.consumeClick()) {
            if (client.player != null) {
                client.setScreenAndShow(new SolverScreen(this.state, this.server, this.files, lookedAtPlacement(client),
                        ClientRsCommands.toDir(client.player.getDirection())));
            }
        }
    }

    private void setCorner(Minecraft client, boolean first) {
        LocalPlayer player = client.player;
        if (player == null) return;
        Optional<BlockPos> target = lookedAtBlock(client);
        if (target.isEmpty()) {
            player.sendOverlayMessage(Chat.hint("Look at a block to set a box corner."));
            return;
        }
        CircuitDraft draft = this.state.getDraft();
        Cell cell = new Cell(target.get().getX(), target.get().getY(), target.get().getZ());
        if (first) draft.setCorner1(cell);
        else draft.setCorner2(cell);

        Optional<Box> box = draft.getBox();
        if (box.isEmpty()) {
            player.sendOverlayMessage(Chat.hint("Corner %d set. Now press %s on the opposite corner.".formatted(
                    first ? 1 : 2, (first ? this.corner2 : this.corner1).getTranslatedKeyMessage().getString())));
            return;
        }
        Cell size = box.get().size();
        player.sendOverlayMessage(Chat.hint("Box: %d x %d x %d. The bottom layer is for supports."
                .formatted(size.x(), size.y(), size.z())));
    }

    private void openCellMenu(Minecraft client) {
        LocalPlayer player = client.player;
        if (player == null || client.level == null) return;
        Optional<Cell> placement = lookedAtPlacement(client);
        Optional<BlockPos> looked = lookedAtBlock(client);
        if (placement.isEmpty() || looked.isEmpty()) {
            player.sendOverlayMessage(Chat.hint("Look at a block, like the repeater or dust you placed for a port."));
            return;
        }
        Cell cell = new Cell(looked.get().getX(), looked.get().getY(), looked.get().getZ());
        WorldBlocks blocks = new WorldBlocks(client.level);
        client.setScreenAndShow(new CellMenuScreen(this.state, cell, placement.get(), blocks.nameAt(cell)));
    }

    private void setPort(Minecraft client, boolean input) {
        LocalPlayer player = client.player;
        if (player == null || client.level == null) return;
        Optional<BlockPos> target = lookedAtBlock(client);
        if (target.isEmpty()) {
            player.sendOverlayMessage(Chat.hint("Look at a repeater, comparator, dust or barrel to set a port."));
            return;
        }

        Cell cell = new Cell(target.get().getX(), target.get().getY(), target.get().getZ());
        CircuitDraft draft = this.state.getDraft();
        WorldBlocks blocks = new WorldBlocks(client.level);
        Optional<PortBlock> block = blocks.portBlockAt(cell);
        if (block.isEmpty()) {
            player.sendOverlayMessage(Chat.hint("A " + blocks.nameAt(cell) + " cannot be a port."));
            return;
        }

        Optional<Port> existing = draft.portAt(cell);
        // The same key on the same kind of port takes it away again.
        if (existing.isPresent() && existing.get().input() == input) {
            draft.removePort(existing.get().name());
            player.sendOverlayMessage(Chat.hint("Removed " + existing.get().name() + "."));
            return;
        }
        if (existing.isPresent() && existing.get().input() != input) {
            player.sendOverlayMessage(Chat.hint("That cell is already " + existing.get().name()
                    + "; clear it with P before changing its direction."));
            return;
        }
        String name = existing.map(Port::name).orElseGet(() -> draft.nextFreeName(input ? "IN" : "OUT"));
        try {
            Port port = block.get().toPort(name, input, cell);
            draft.putPort(port);
            player.sendOverlayMessage(Chat.hint((input ? "Input " : "Output ") + port.describe() + " set."));
        } catch (IllegalArgumentException | IllegalStateException e) {
            player.sendOverlayMessage(Chat.error(e.getMessage()));
        }
    }

    static Optional<BlockPos> lookedAtBlock(Minecraft client) {
        HitResult hit = client.hitResult;
        if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK) return Optional.empty();
        return Optional.of(blockHit.getBlockPos());
    }

    /** The cell where a block would be placed against the looked-at face. */
    static Optional<Cell> lookedAtPlacement(Minecraft client) {
        HitResult hit = client.hitResult;
        if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK) return Optional.empty();
        BlockPos pos = blockHit.getBlockPos().relative(blockHit.getDirection());
        return Optional.of(new Cell(pos.getX(), pos.getY(), pos.getZ()));
    }
}
