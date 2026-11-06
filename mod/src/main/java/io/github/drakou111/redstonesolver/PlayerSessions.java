package io.github.drakou111.redstonesolver;

import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;

/** Each player's solved circuit waiting to be placed. Server thread only. */
public final class PlayerSessions {
    /** A solved layout and the world box it was solved for. */
    public record PendingResult(Layout layout, Box box) {
    }

    /** One player's state. */
    public static final class Session {
        private PendingResult pending;

        public Optional<PendingResult> getPending() {
            return Optional.ofNullable(this.pending);
        }

        public void setPending(Optional<PendingResult> pending) {
            this.pending = pending.orElse(null);
        }
    }

    private final Map<UUID, Session> sessions = new HashMap<>();

    public Session get(UUID player) {
        return this.sessions.computeIfAbsent(player, id -> new Session());
    }
}
