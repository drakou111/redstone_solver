package io.github.drakou111.redstonesolver.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.loader.api.FabricLoader;

/** Client side: holds the draft, the keys, the outlines and the /rs commands. */
public final class RedstoneSolverClient implements ClientModInitializer {
    @Override
    public void onInitializeClient() {
        ClientState state = new ClientState();
        DraftFiles files = new DraftFiles(FabricLoader.getInstance().getConfigDir());
        DraftAutosave autosave = new DraftAutosave(state, files);
        autosave.register();
        ServerLink server = new ServerLink(state, autosave);
        server.register();
        new SolverKeys(state, server, files).register();
        new DraftRenderer(state).register();
        ClientRsCommands commands = new ClientRsCommands(state, server, files);
        ClientCommandRegistrationCallback.EVENT.register((dispatcher, registryAccess) -> commands.register(dispatcher));
    }
}
