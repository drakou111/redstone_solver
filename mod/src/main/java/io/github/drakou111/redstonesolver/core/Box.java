package io.github.drakou111.redstonesolver.core;

/** An axis-aligned box of whole blocks, both corners included. */
public record Box(Cell min, Cell max) {
    public Box {
        if (min.x() > max.x() || min.y() > max.y() || min.z() > max.z())
            throw new IllegalArgumentException("box corners are out of order: " + min + " / " + max);
    }

    public static Box spanning(Cell a, Cell b) {
        return new Box(Cell.min(a, b), Cell.max(a, b));
    }

    public Cell size() {
        return new Cell(this.max.x() - this.min.x() + 1, this.max.y() - this.min.y() + 1,
                this.max.z() - this.min.z() + 1);
    }

    public int volume() {
        Cell size = this.size();
        return size.x() * size.y() * size.z();
    }

    public boolean contains(Cell cell) {
        return cell.x() >= this.min.x() && cell.x() <= this.max.x() && cell.y() >= this.min.y()
                && cell.y() <= this.max.y() && cell.z() >= this.min.z() && cell.z() <= this.max.z();
    }
}
