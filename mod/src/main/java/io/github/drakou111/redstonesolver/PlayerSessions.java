package io.github.drakou111.redstonesolver;

import com.google.gson.JsonObject;
import io.github.drakou111.redstonesolver.core.Box;
import io.github.drakou111.redstonesolver.core.Cell;
import io.github.drakou111.redstonesolver.core.Layout;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;

/** Each player's solved circuits, newest last, so any of them can be placed, shrunk or exported. Server thread only. */
public final class PlayerSessions {
    /** How many results a player keeps; the oldest goes first. */
    public static final int MAX_RESULTS = 20;

    /**
     * A solved circuit: its layout (also as solver JSON for shrinking and export), its spec, where it was
     * solved (the lowest corner, the floor layer included when one is assumed), an older box to clear when
     * placed back there (after a shrink), and whether its bottom layer is an assumed floor never placed.
     */
    public record StoredResult(int id, Layout layout, JsonObject layoutJson, JsonObject spec, Cell origin,
            Optional<Box> alsoClear, boolean skipBottom) {
        public Box box() {
            return new Box(this.origin, this.origin.plus(this.layout.size()).minus(new Cell(1, 1, 1)));
        }
    }

    /** One player's results. */
    public static final class Session {
        private final List<StoredResult> results = new ArrayList<>();
        private int nextId = 1;

        public StoredResult add(Layout layout, JsonObject layoutJson, JsonObject spec, Cell origin,
                Optional<Box> alsoClear, boolean skipBottom) {
            StoredResult result = new StoredResult(this.nextId++, layout, layoutJson, spec, origin, alsoClear,
                    skipBottom);
            this.results.add(result);
            while (this.results.size() > MAX_RESULTS) this.results.removeFirst();
            return result;
        }

        /** The result with this id, or the newest when the id is absent. */
        public Optional<StoredResult> find(Optional<Integer> id) {
            if (id.isEmpty()) return this.results.isEmpty() ? Optional.empty() : Optional.of(this.results.getLast());
            for (StoredResult result : this.results) {
                if (result.id() == id.get()) return Optional.of(result);
            }
            return Optional.empty();
        }

        public boolean forget(int id) {
            return this.results.removeIf(result -> result.id() == id);
        }
    }

    private final Map<UUID, Session> sessions = new HashMap<>();

    public Session get(UUID player) {
        return this.sessions.computeIfAbsent(player, id -> new Session());
    }
}
