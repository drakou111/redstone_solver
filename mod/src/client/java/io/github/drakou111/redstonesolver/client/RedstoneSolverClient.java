package io.github.drakou111.redstonesolver.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;

/** Client side: holds the draft, the keys, the outlines and the /rs commands. */
public final class RedstoneSolverClient implements ClientModInitializer {
    @Override
    public void onInitializeClient() {
        ClientState state = new ClientState();
        ServerLink server = new ServerLink(state);
        server.register();
        new SolverKeys(state, server).register();
        new DraftRenderer(state).register();
        ClientRsCommands commands = new ClientRsCommands(state, server);
        ClientCommandRegistrationCallback.EVENT.register((dispatcher, registryAccess) -> commands.register(dispatcher));
    }
}
