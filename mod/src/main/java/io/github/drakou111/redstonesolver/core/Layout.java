package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/** A solved circuit: its size and every non-air block, positioned relative to the box. */
public record Layout(Cell size, List<Block> blocks) {
    public Layout {
        blocks = List.copyOf(blocks);
    }

    /**
     * One block as the solver describes it. Optional fields are present only for the block types
     * that use them (facing, repeater delay, comparator mode, container signal, port kind/channel).
     */
    public record Block(Cell position, String type, Optional<Dir> facing, int delay, String mode, int signal,
            String kind, int channel) {
        public Dir requireFacing() {
            return this.facing.orElseThrow(() -> new IllegalArgumentException(this.type + " without a facing"));
        }
    }

    public static Layout fromJson(JsonObject json) {
        Cell size = Cell.fromJson(json.get("size"));
        List<Block> blocks = new ArrayList<>();
        for (JsonElement element : json.getAsJsonArray("blocks")) {
            JsonObject entry = element.getAsJsonObject();
            Optional<Dir> facing = Optional.empty();
            if (entry.has("facing")) {
                String name = entry.get("facing").getAsString();
                facing = Optional.of(Dir.parse(name)
                        .orElseThrow(() -> new IllegalArgumentException("unknown facing " + name)));
            }
            blocks.add(new Block(Cell.fromJson(entry.get("position")), entry.get("type").getAsString(), facing,
                    intOr(entry, "delay", 1), stringOr(entry, "mode", "compare"), intOr(entry, "signal", 0),
                    stringOr(entry, "kind", ""), intOr(entry, "channel", 0)));
        }
        return new Layout(size, blocks);
    }

    public int countPlacedBlocks() {
        int count = 0;
        for (Block block : this.blocks) {
            if (JavaBlockStates.stateFor(block).isPresent()) count++;
        }
        return count;
    }

    private static int intOr(JsonObject json, String name, int fallback) {
        return json.has(name) ? json.get(name).getAsInt() : fallback;
    }

    private static String stringOr(JsonObject json, String name, String fallback) {
        return json.has(name) ? json.get(name).getAsString() : fallback;
    }
}
