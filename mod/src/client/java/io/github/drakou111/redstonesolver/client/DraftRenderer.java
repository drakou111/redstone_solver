package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Dir;
import io.github.drakou111.redstonesolver.core.Layout;
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
    private static final int KEPT_COLOR = 0xFF4C8CFF;
    private static final int EMPTY_COLOR = 0xFFFF4040;
    private static final float MARK_LINE_WIDTH = 1.5f;
    private static final float BOX_LINE_WIDTH = 2.5f;
    private static final float PORT_LINE_WIDTH = 2.0f;
    private static final int PORT_FILL_ALPHA = 0x40000000;
    private static final float LABEL_SCALE = 0.6f;
    private static final double LABEL_HEIGHT = 1.25;
    private static final double ARROW_LENGTH = 0.8;
    private static final double CELL_INSET = 0.02;
    // The solver's circuit so far: see-through shapes in each block's form.
    private static final int PREVIEW_FILL_ALPHA = 0x50000000;
    private static final float PREVIEW_LINE_WIDTH = 1.0f;
    private static final int PREVIEW_BLOCK_COLOR = 0xFF40C8FF;
    private static final int PREVIEW_GLASS_COLOR = 0xFFB0E8FF;
    private static final int PREVIEW_SPECIAL_COLOR = 0xFFFFC040;
    private static final int PREVIEW_DUST_COLOR = 0xFFFF2020;
    private static final int PREVIEW_REPEATER_COLOR = 0xFFC0C0C0;
    private static final int PREVIEW_COMPARATOR_COLOR = 0xFFF0F0F0;
    private static final int PREVIEW_TORCH_COLOR = 0xFFFF8020;
    private static final double DUST_HEIGHT = 1.0 / 16;
    private static final double DIODE_HEIGHT = 2.0 / 16;
    private static final double TORCH_HALF_WIDTH = 1.0 / 16;
    private static final double TORCH_HEIGHT = 10.0 / 16;
    private static final double WALL_TORCH_SHIFT = 5.0 / 16;
    private static final double DIODE_ARROW_LENGTH = 0.6;

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
            for (Cell cell : draft.getKeptCells()) Gizmos.cuboid(cellBox(cell), GizmoStyle.stroke(KEPT_COLOR, MARK_LINE_WIDTH));
            for (Cell cell : draft.getEmptyCells()) drawEmpty(cell);
            this.state.getPreview().ifPresent(DraftRenderer::drawPreview);
        }
    }

    private static void drawPreview(ClientState.Preview preview) {
        for (Layout.Block block : preview.layout().blocks()) {
            Cell p = preview.origin().plus(block.position());
            double x = p.x();
            double y = p.y();
            double z = p.z();
            switch (block.type()) {
                case "air", "input", "output", "lever", "button", "lamp" -> {
                }
                case "dust" -> shape(new AABB(x, y, z, x + 1, y + DUST_HEIGHT, z + 1), PREVIEW_DUST_COLOR);
                case "repeater", "comparator" -> {
                    int color = block.type().equals("repeater") ? PREVIEW_REPEATER_COLOR : PREVIEW_COMPARATOR_COLOR;
                    shape(new AABB(x, y, z, x + 1, y + DIODE_HEIGHT, z + 1), color);
                    block.facing().ifPresent(facing -> {
                        Cell step = facing.offset();
                        Vec3 center = new Vec3(x + 0.5, y + DIODE_HEIGHT, z + 0.5);
                        Vec3 half = new Vec3(step.x(), step.y(), step.z()).scale(DIODE_ARROW_LENGTH / 2);
                        Gizmos.arrow(center.subtract(half), center.add(half), PREVIEW_DUST_COLOR);
                    });
                }
                case "torch" -> {
                    // A wall torch leans towards the block it hangs on.
                    Cell lean = block.facing().filter(f -> f.offset().y() == 0).map(Dir::offset).orElse(new Cell(0, 0, 0));
                    double cx = x + 0.5 + lean.x() * WALL_TORCH_SHIFT;
                    double cz = z + 0.5 + lean.z() * WALL_TORCH_SHIFT;
                    shape(new AABB(cx - TORCH_HALF_WIDTH, y, cz - TORCH_HALF_WIDTH, cx + TORCH_HALF_WIDTH,
                            y + TORCH_HEIGHT, cz + TORCH_HALF_WIDTH), PREVIEW_TORCH_COLOR);
                }
                case "solid" -> shape(cellBox(p), PREVIEW_BLOCK_COLOR);
                case "glass" -> shape(cellBox(p), PREVIEW_GLASS_COLOR);
                default -> shape(cellBox(p), PREVIEW_SPECIAL_COLOR);
            }
        }
    }

    private static void shape(AABB box, int color) {
        int fill = (color & 0x00FFFFFF) | PREVIEW_FILL_ALPHA;
        Gizmos.cuboid(box, GizmoStyle.strokeAndFill(color, PREVIEW_LINE_WIDTH, fill));
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

    // An X on the floor of the cell.
    private static void drawEmpty(Cell cell) {
        Gizmos.cuboid(cellBox(cell), GizmoStyle.stroke(EMPTY_COLOR, MARK_LINE_WIDTH));
        double y = cell.y() + CELL_INSET;
        Gizmos.line(new Vec3(cell.x(), y, cell.z()), new Vec3(cell.x() + 1, y, cell.z() + 1), EMPTY_COLOR, MARK_LINE_WIDTH);
        Gizmos.line(new Vec3(cell.x() + 1, y, cell.z()), new Vec3(cell.x(), y, cell.z() + 1), EMPTY_COLOR, MARK_LINE_WIDTH);
    }

    private static AABB cellBox(Cell cell) {
        BlockPos pos = new BlockPos(cell.x(), cell.y(), cell.z());
        return new AABB(pos).deflate(CELL_INSET);
    }
}
