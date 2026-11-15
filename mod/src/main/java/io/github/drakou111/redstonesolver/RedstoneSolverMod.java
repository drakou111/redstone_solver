package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.net.SolverPayloads;
import io.github.drakou111.redstonesolver.solver.SolverService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import java.nio.file.Path;
import java.util.Optional;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;

/** Server side: owns the solver process and answers the client's solve and place requests. */
public final class RedstoneSolverMod implements ModInitializer {
    public static final String MOD_ID = "redstone_solver";

    private static final Logger LOGGER = LoggerFactory.getLogger(RedstoneSolverMod.class);

    @Override
    public void onInitialize() {
        FabricLoader loader = FabricLoader.getInstance();
        ModConfig config = ModConfig.load(loader.getConfigDir(), loader.getGameDir());
        LOGGER.info("Solver: {}", config.solverExecutable());

        SolverPayloads.register();
        // Circuits found before are kept here and returned at once when asked for again.
        Path library = loader.getConfigDir().resolve(MOD_ID).resolve("library");
        // Build from gates needs the gate catalog; a copy comes with the mod.
        try {
            NativeSolver.installCatalog(library);
        } catch (java.io.UncheckedIOException e) {
            LOGGER.warn("Could not copy the gate catalog: " + e.getMessage(), e);
        }
        SolverService solver = new SolverService(config.solverExecutable(), Optional.of(library));
        new SolverNetworking(new PlayerSessions(), solver).register();
        ServerLifecycleEvents.SERVER_STOPPING.register(server -> solver.close());
    }
}
