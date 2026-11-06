package io.github.drakou111.redstonesolver.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;

/** An integer block position, either in the world or relative to a box. */
public record Cell(int x, int y, int z) {
    public Cell plus(Cell other) {
        return new Cell(this.x + other.x, this.y + other.y, this.z + other.z);
    }

    public Cell minus(Cell other) {
        return new Cell(this.x - other.x, this.y - other.y, this.z - other.z);
    }

    public static Cell min(Cell a, Cell b) {
        return new Cell(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.min(a.z, b.z));
    }

    public static Cell max(Cell a, Cell b) {
        return new Cell(Math.max(a.x, b.x), Math.max(a.y, b.y), Math.max(a.z, b.z));
    }

    public JsonArray toJson() {
        JsonArray array = new JsonArray();
        array.add(this.x);
        array.add(this.y);
        array.add(this.z);
        return array;
    }

    public static Cell fromJson(JsonElement json) {
        JsonArray array = json.getAsJsonArray();
        if (array.size() != 3) throw new IllegalArgumentException("a position must be [x, y, z]: " + json);
        return new Cell(array.get(0).getAsInt(), array.get(1).getAsInt(), array.get(2).getAsInt());
    }

    @Override
    public String toString() {
        return "%d %d %d".formatted(this.x, this.y, this.z);
    }
}
