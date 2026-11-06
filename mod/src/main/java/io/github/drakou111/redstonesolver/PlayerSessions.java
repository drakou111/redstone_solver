package io.github.drakou111.redstonesolver;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;

/** Each player's solved circuits: the one waiting to be placed and the last one. Server thread only. */
public final class PlayerSessions {
    /** A solved layout and the world box it was solved for. */
    public record PendingResult(Layout layout, Box box) {
    }

    /** The last solved circuit (kept after placing) so it can be shrunk: solver JSON and world origin. */
    public record LastResult(JsonObject layoutJson, JsonObject spec, Cell origin) {
    }

    /** One player's state. */
    public static final class Session {
        private PendingResult pending;
        private LastResult last;

        public Optional<PendingResult> getPending() {
            return Optional.ofNullable(this.pending);
        }

        public void setPending(Optional<PendingResult> pending) {
            this.pending = pending.orElse(null);
        }

        public Optional<LastResult> getLast() {
            return Optional.ofNullable(this.last);
        }

        public void setLast(Optional<LastResult> last) {
            this.last = last.orElse(null);
        }
    }

    private final Map<UUID, Session> sessions = new HashMap<>();

    public Session get(UUID player) {
        return this.sessions.computeIfAbsent(player, id -> new Session());
    }
}
