package io.github.drakou111.redstonesolver.client;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.QuadInstance;
import com.mojang.brigadier.exceptions.CommandSyntaxException;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.JavaBlockStates;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import net.fabricmc.fabric.api.client.rendering.v1.level.LevelRenderContext;
import net.fabricmc.fabric.api.client.rendering.v1.level.LevelRenderEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.rendertype.RenderTypes;
import net.minecraft.client.renderer.texture.OverlayTexture;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.commands.arguments.blocks.BlockStateParser;
import net.minecraft.core.Direction;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.util.LightCoordsUtil;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

/**
 * Draws the running job's circuit so far as see-through copies of the real blocks (like a schematic
 * hologram), at the cells it would be placed in.
 */
public final class GhostRenderer {
    private static final int ALPHA = 0x90;
    private static final int PLAIN_COLOR = 0xFFFFFF;
    // Dust's model is tinted by its power; a lit red reads as dust.
    private static final int DUST_COLOR = 0xE01010;
    private static final long MODEL_SEED = 42L;
    private static final Direction[] FACES_AND_INSIDE = {Direction.DOWN, Direction.UP, Direction.NORTH,
            Direction.SOUTH, Direction.WEST, Direction.EAST, null};

    private final ClientState state;
    private Optional<ClientState.Preview> shown = Optional.empty();
    private List<Ghost> ghosts = List.of();

    private record Ghost(Vec3 position, List<BlockStateModelPart> parts, boolean dust) {
    }

    public GhostRenderer(ClientState state) {
        this.state = state;
    }

    public void register() {
        LevelRenderEvents.COLLECT_SUBMITS.register(this::draw);
    }

    private void draw(LevelRenderContext context) {
        Optional<ClientState.Preview> preview = this.state.isShowOutlines() ? this.state.getPreview() : Optional.empty();
        if (preview.isEmpty()) return;
        if (!preview.equals(this.shown)) {
            this.ghosts = ghostsOf(preview.get());
            this.shown = preview;
        }

        Vec3 camera = Minecraft.getInstance().gameRenderer.mainCamera().position();
        PoseStack poseStack = context.poseStack();
        int color = (ALPHA << 24) | PLAIN_COLOR;
        int dust = (ALPHA << 24) | DUST_COLOR;
        for (Ghost ghost : this.ghosts) {
            poseStack.pushPose();
            Vec3 offset = ghost.position().subtract(camera);
            poseStack.translate(offset.x, offset.y, offset.z);
            context.submitNodeCollector().submitCustomGeometry(poseStack, RenderTypes.translucentMovingBlock(),
                    (pose, consumer) -> {
                        QuadInstance instance = new QuadInstance();
                        instance.setLightCoords(LightCoordsUtil.FULL_BRIGHT);
                        instance.setOverlayCoords(OverlayTexture.NO_OVERLAY);
                        for (BlockStateModelPart part : ghost.parts()) {
                            for (Direction face : FACES_AND_INSIDE) {
                                for (BakedQuad quad : part.getQuads(face)) {
                                    instance.setColor(ghost.dust() && quad.materialInfo().isTinted() ? dust : color);
                                    consumer.putBakedQuad(pose, quad, instance);
                                }
                            }
                        }
                    });
            poseStack.popPose();
        }
    }

    // Ports are the player's own blocks, already in the world: not drawn.
    private static List<Ghost> ghostsOf(ClientState.Preview preview) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.level == null) return List.of();
        HolderLookup<Block> blocks = minecraft.level.registryAccess().lookupOrThrow(Registries.BLOCK);
        RandomSource random = RandomSource.create(MODEL_SEED);
        List<Ghost> ghosts = new ArrayList<>();
        for (Layout.Block block : preview.layout().blocks()) {
            if (block.type().equals("input") || block.type().equals("output")) continue;
            Optional<String> text = JavaBlockStates.stateFor(block);
            if (text.isEmpty()) continue;
            BlockState blockState = parse(blocks, text.get());
            BlockStateModel model = minecraft.getModelManager().getBlockStateModelSet().get(blockState);
            List<BlockStateModelPart> parts = new ArrayList<>();
            random.setSeed(MODEL_SEED);
            model.collectParts(random, parts);
            Cell cell = preview.origin().plus(block.position());
            ghosts.add(new Ghost(new Vec3(cell.x(), cell.y(), cell.z()), parts, block.type().equals("dust")));
        }
        return ghosts;
    }

    private static BlockState parse(HolderLookup<Block> blocks, String text) {
        try {
            return BlockStateParser.parseForBlock(blocks, text, false).blockState();
        } catch (CommandSyntaxException e) {
            throw new IllegalStateException("bad block state '" + text + "': " + e.getMessage(), e);
        }
    }
}
