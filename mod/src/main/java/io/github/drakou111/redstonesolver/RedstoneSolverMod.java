package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.net.SolverPayloads;
import io.github.drakou111.redstonesolver.solver.SolverService;
import java.lang.System.Logger;
import java.lang.System.Logger.Level;
import java.nio.file.Path;
import java.util.Optional;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;

/** Server side: owns the solver process and answers the client's solve and place requests. */
public final class RedstoneSolverMod implements ModInitializer {
    public static final String MOD_ID = "redstone_solver";

    private static final Logger LOGGER = System.getLogger(RedstoneSolverMod.class.getName());

    @Override
    public void onInitialize() {
        FabricLoader loader = FabricLoader.getInstance();
        ModConfig config = ModConfig.load(loader.getConfigDir(), loader.getGameDir());
        LOGGER.log(Level.INFO, "Solver: {0}", config.solverExecutable());

        SolverPayloads.register();
        // Circuits found before are kept here and returned at once when asked for again.
        Path library = loader.getConfigDir().resolve(MOD_ID).resolve("library");
        SolverService solver = new SolverService(config.solverExecutable(), Optional.of(library));
        new SolverNetworking(new PlayerSessions(), solver).register();
        ServerLifecycleEvents.SERVER_STOPPING.register(server -> solver.close());
    }
}
