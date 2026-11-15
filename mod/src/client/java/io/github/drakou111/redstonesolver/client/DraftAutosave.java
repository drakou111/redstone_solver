package io.github.drakou111.redstonesolver.client;

import io.github.drakou111.redstonesolver.Chat;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import java.io.UncheckedIOException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import java.util.Optional;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents;
import net.minecraft.client.Minecraft;

/** Restores the draft of a world or server on joining it and saves it on leaving. */
public final class DraftAutosave {
    private static final Logger LOGGER = LoggerFactory.getLogger(DraftAutosave.class);

    private final ClientState state;
    private final DraftFiles files;
    private String currentName;

    public DraftAutosave(ClientState state, DraftFiles files) {
        this.state = state;
        this.files = files;
    }

    public void register() {
        ClientPlayConnectionEvents.JOIN.register((handler, sender, client) -> this.join(client));
        ClientPlayConnectionEvents.DISCONNECT.register((handler, client) -> this.saveNow());
    }

    /** Saves the current world's draft now (also done on leaving and before each solve). */
    public void saveNow() {
        if (this.currentName == null) return;
        try {
            this.files.save(this.currentName, this.state, true);
        } catch (UncheckedIOException e) {
            LOGGER.warn("Could not save the draft: " + e.getMessage());
        }
    }

    private void join(Minecraft client) {
        this.currentName = worldId(client).map(DraftFiles::autosaveName).orElse(null);
        this.state.replaceDraft(new CircuitDraft());
        this.state.clearResults();
        this.state.setSolving(false);
        if (this.currentName == null) return;
        try {
            if (this.files.load(this.currentName, this.state) && client.player != null)
                client.player.sendSystemMessage(Chat.hint("[Solver] Restored your draft for this world."));
        } catch (IllegalArgumentException | UncheckedIOException e) {
            LOGGER.warn("Could not restore the draft: " + e.getMessage());
            if (client.player != null)
                client.player.sendSystemMessage(Chat.error("Could not restore your draft: " + e.getMessage()));
        }
    }

    private static Optional<String> worldId(Minecraft client) {
        if (client.getSingleplayerServer() != null)
            return Optional.of("world-" + client.getSingleplayerServer().getWorldData().getLevelName());
        if (client.getCurrentServer() != null) return Optional.of("server-" + client.getCurrentServer().ip);
        return Optional.empty();
    }
}
