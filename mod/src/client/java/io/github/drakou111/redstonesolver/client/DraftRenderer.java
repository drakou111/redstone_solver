package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import java.util.Optional;
import net.fabricmc.fabric.api.client.rendering.v1.level.LevelRenderEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.core.BlockPos;
import net.minecraft.gizmos.GizmoStyle;
import net.minecraft.gizmos.Gizmos;
import net.minecraft.gizmos.TextGizmo;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Draws the box, its corners and the ports in the world with the game's gizmos, every frame. */
public final class DraftRenderer {
    private static final int BOX_COLOR = 0xFFFFFFFF;
    private static final int CORNER_COLOR = 0xFFFFD800;
    private static final int INPUT_COLOR = 0xFF3CDC3C;
    private static final int OUTPUT_COLOR = 0xFFE03C3C;
    private static final float BOX_LINE_WIDTH = 2.5f;
    private static final float PORT_LINE_WIDTH = 2.0f;
    private static final int PORT_FILL_ALPHA = 0x40000000;
    private static final float LABEL_SCALE = 0.6f;
    private static final double LABEL_HEIGHT = 1.25;
    private static final double ARROW_LENGTH = 0.8;
    private static final double CELL_INSET = 0.02;

    private final ClientState state;

    public DraftRenderer(ClientState state) {
        this.state = state;
    }

    public void register() {
        LevelRenderEvents.BEFORE_GIZMOS.register(context -> this.draw());
    }

    private void draw() {
        if (!this.state.isShowOutlines()) return;
        CircuitDraft draft = this.state.getDraft();
        Optional<Box> box = draft.getBox();
        if (box.isEmpty() && draft.getAnyCorner().isEmpty()) return;

        // The game drains this collector when it draws gizmos right after this event.
        try (Gizmos.TemporaryCollection collection = Minecraft.getInstance().levelRenderer.collectPerFrameRenderThreadGizmos()) {
            if (box.isEmpty()) {
                Gizmos.cuboid(cellBox(draft.getAnyCorner().get()), GizmoStyle.stroke(CORNER_COLOR, BOX_LINE_WIDTH));
                return;
            }
            Box b = box.get();
            AABB outline = new AABB(b.min().x(), b.min().y(), b.min().z(), b.max().x() + 1, b.max().y() + 1, b.max().z() + 1);
            Gizmos.cuboid(outline, GizmoStyle.stroke(BOX_COLOR, BOX_LINE_WIDTH));
            for (Port port : draft.getInputs()) drawPort(port, INPUT_COLOR);
            for (Port port : draft.getOutputs()) drawPort(port, OUTPUT_COLOR);
        }
    }

    private static void drawPort(Port port, int color) {
        Cell cell = port.position();
        int fill = (color & 0x00FFFFFF) | PORT_FILL_ALPHA;
        Gizmos.cuboid(cellBox(cell), GizmoStyle.strokeAndFill(color, PORT_LINE_WIDTH, fill));
        Vec3 center = new Vec3(cell.x() + 0.5, cell.y() + 0.5, cell.z() + 0.5);
        Gizmos.billboardText(port.name(), center.add(0, LABEL_HEIGHT - 0.5, 0),
                TextGizmo.Style.forColorAndCentered(color).withScale(LABEL_SCALE)).setAlwaysOnTop();
        if (port.hasFacing()) {
            Cell step = port.facing().offset();
            Vec3 half = new Vec3(step.x(), step.y(), step.z()).scale(ARROW_LENGTH / 2);
            Gizmos.arrow(center.subtract(half), center.add(half), color).setAlwaysOnTop();
        }
    }

    private static AABB cellBox(Cell cell) {
        BlockPos pos = new BlockPos(cell.x(), cell.y(), cell.z());
        return new AABB(pos).deflate(CELL_INSET);
    }
}
