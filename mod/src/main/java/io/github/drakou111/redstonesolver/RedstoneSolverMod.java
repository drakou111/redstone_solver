package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.solver.SolverService;
import java.lang.System.Logger;
import java.lang.System.Logger.Level;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.item.Item;

public final class RedstoneSolverMod implements ModInitializer {
    public static final String MOD_ID = "redstone_solver";

    private static final Logger LOGGER = System.getLogger(RedstoneSolverMod.class.getName());

    @Override
    public void onInitialize() {
        FabricLoader loader = FabricLoader.getInstance();
        ModConfig config = ModConfig.load(loader.getConfigDir(), loader.getGameDir());
        LOGGER.log(Level.INFO, "Solver: {0}", config.solverExecutable());

        Item wand = registerWand();
        PlayerSessions sessions = new PlayerSessions();
        SolverService solver = new SolverService(config.solverExecutable());
        RsCommands commands = new RsCommands(sessions, solver, config, wand);
        BoxOverlay overlay = new BoxOverlay(sessions, wand);

        new WandEvents(sessions, wand).register();
        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> commands.register(dispatcher));
        ServerTickEvents.END_SERVER_TICK.register(overlay::tick);
        ServerLifecycleEvents.SERVER_STOPPING.register(server -> solver.close());
    }

    private static Item registerWand() {
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, Identifier.fromNamespaceAndPath(MOD_ID, "wand"));
        return Registry.register(BuiltInRegistries.ITEM, key, new Item(new Item.Properties().setId(key).stacksTo(1)));
    }
}
