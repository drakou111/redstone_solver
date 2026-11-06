package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import java.util.Optional;
import net.fabricmc.fabric.api.event.player.AttackBlockCallback;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.item.Item;

/** The wand: left-click a block for corner 1, right-click for corner 2. Neither breaks nor places. */
public final class WandEvents {
    private final PlayerSessions sessions;
    private final Item wand;

    public WandEvents(PlayerSessions sessions, Item wand) {
        this.sessions = sessions;
        this.wand = wand;
    }

    public void register() {
        AttackBlockCallback.EVENT.register((player, level, hand, pos, direction) -> {
            if (player.getItemInHand(hand).getItem() != this.wand) return InteractionResult.PASS;
            if (player instanceof ServerPlayer serverPlayer) this.setCorner(serverPlayer, pos, true);
            return InteractionResult.SUCCESS;
        });
        UseBlockCallback.EVENT.register((player, level, hand, hit) -> {
            if (player.getItemInHand(hand).getItem() != this.wand) return InteractionResult.PASS;
            if (player instanceof ServerPlayer serverPlayer) this.setCorner(serverPlayer, hit.getBlockPos(), false);
            return InteractionResult.SUCCESS;
        });
    }

    private void setCorner(ServerPlayer player, BlockPos pos, boolean first) {
        CircuitDraft draft = this.sessions.get(player.getUUID()).getDraft();
        Cell cell = new Cell(pos.getX(), pos.getY(), pos.getZ());
        if (first) draft.setCorner1(cell);
        else draft.setCorner2(cell);

        Optional<Box> box = draft.getBox();
        String which = first ? "Corner 1" : "Corner 2";
        if (box.isEmpty()) {
            player.sendSystemMessage(Chat.info(which + " set at " + cell + ". Now "
                    + (first ? "right" : "left") + "-click the opposite corner."));
            return;
        }
        Cell size = box.get().size();
        player.sendSystemMessage(Chat.info("%s set. Box: %d x %d x %d (%d blocks). The bottom layer holds supports only."
                .formatted(which, size.x(), size.y(), size.z(), box.get().volume())));
    }
}
