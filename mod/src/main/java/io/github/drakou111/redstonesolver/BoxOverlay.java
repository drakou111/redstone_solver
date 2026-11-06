package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import io.github.drakou111.redstonesolver.core.Port;
import java.util.Optional;
import net.minecraft.core.particles.DustParticleOptions;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.Item;

/** Draws each wand holder's box and ports with particles that only they see. */
public final class BoxOverlay {
    private static final int INTERVAL_TICKS = 10;
    private static final double EDGE_STEP = 0.5;
    private static final int MAX_POINTS_PER_EDGE = 64;
    private static final float PARTICLE_SCALE = 0.8f;
    private static final DustParticleOptions BOX_EDGE = new DustParticleOptions(0xFFFFFF, PARTICLE_SCALE);
    private static final DustParticleOptions CORNER = new DustParticleOptions(0xFFD800, PARTICLE_SCALE);
    private static final DustParticleOptions INPUT = new DustParticleOptions(0x3CDC3C, 1.5f);
    private static final DustParticleOptions OUTPUT = new DustParticleOptions(0xE03C3C, 1.5f);

    private final PlayerSessions sessions;
    private final Item wand;

    public BoxOverlay(PlayerSessions sessions, Item wand) {
        this.sessions = sessions;
        this.wand = wand;
    }

    public void tick(MinecraftServer server) {
        if (server.getTickCount() % INTERVAL_TICKS != 0) return;
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            if (player.getMainHandItem().getItem() != this.wand && player.getOffhandItem().getItem() != this.wand)
                continue;
            this.sessions.find(player.getUUID()).ifPresent(session -> draw(player, session.getDraft()));
        }
    }

    private static void draw(ServerPlayer player, CircuitDraft draft) {
        ServerLevel level = player.level();
        Optional<Box> box = draft.getBox();
        if (box.isEmpty()) {
            draft.getAnyCorner().ifPresent(corner -> drawCellCorners(level, player, corner));
            return;
        }
        drawEdges(level, player, box.get());
        for (Port port : draft.getInputs()) dot(level, player, INPUT, port.position());
        for (Port port : draft.getOutputs()) dot(level, player, OUTPUT, port.position());
    }

    private static void drawEdges(ServerLevel level, ServerPlayer player, Box box) {
        double[] lo = {box.min().x(), box.min().y(), box.min().z()};
        double[] hi = {box.max().x() + 1, box.max().y() + 1, box.max().z() + 1};
        for (int axis = 0; axis < 3; axis++) {
            int a = (axis + 1) % 3;
            int b = (axis + 2) % 3;
            double length = hi[axis] - lo[axis];
            double step = Math.max(EDGE_STEP, length / MAX_POINTS_PER_EDGE);
            for (int corner = 0; corner < 4; corner++) {
                double[] point = new double[3];
                point[a] = (corner & 1) == 0 ? lo[a] : hi[a];
                point[b] = (corner & 2) == 0 ? lo[b] : hi[b];
                for (double t = 0; t <= length + 1e-6; t += step) {
                    point[axis] = lo[axis] + t;
                    send(level, player, BOX_EDGE, point[0], point[1], point[2]);
                }
            }
        }
    }

    private static void drawCellCorners(ServerLevel level, ServerPlayer player, Cell cell) {
        for (int corner = 0; corner < 8; corner++) {
            send(level, player, CORNER, cell.x() + (corner & 1), cell.y() + ((corner >> 1) & 1),
                    cell.z() + ((corner >> 2) & 1));
        }
    }

    private static void dot(ServerLevel level, ServerPlayer player, DustParticleOptions color, Cell cell) {
        send(level, player, color, cell.x() + 0.5, cell.y() + 0.5, cell.z() + 0.5);
    }

    private static void send(ServerLevel level, ServerPlayer player, DustParticleOptions options, double x, double y,
            double z) {
        level.sendParticles(player, options, true, false, x, y, z, 1, 0, 0, 0, 0);
    }
}
