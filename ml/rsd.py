"""Reading and writing .rsd sample files (see src/datagen/main.cpp)."""

from dataclasses import dataclass
from pathlib import Path

import numpy as np

MAGIC = b"RSD1"


@dataclass(frozen=True)
class Header:
    size: tuple[int, int, int]  # x, y, z
    inputs: int
    outputs: int
    vectors: int
    classes: int

    @property
    def volume(self) -> int:
        return self.size[0] * self.size[1] * self.size[2]

    @property
    def table_bytes(self) -> int:
        return self.vectors * self.outputs

    def to_bytes(self) -> bytes:
        return MAGIC + bytes([*self.size, self.inputs, self.outputs,
                              self.vectors, self.classes])


@dataclass
class Samples:
    header: Header
    tables: np.ndarray  # (n, vectors * outputs) uint8 output levels, 255 = broken
    cells: np.ndarray   # (n, y, z, x) uint8 classes

    def __len__(self) -> int:
        return self.tables.shape[0]


def read(path: Path) -> Samples:
    raw = path.read_bytes()
    if raw[:4] != MAGIC:
        raise ValueError(f"{path} is not an .rsd file")
    sx, sy, sz, n_in, n_out, n_vec, n_cls = raw[4:11]
    h = Header((sx, sy, sz), n_in, n_out, n_vec, n_cls)
    record = h.table_bytes + h.volume
    data = np.frombuffer(raw[11:], dtype=np.uint8)
    n = data.size // record
    data = data[: n * record].reshape(n, record)
    return Samples(h, data[:, : h.table_bytes].copy(),
                   data[:, h.table_bytes:].reshape(n, sy, sz, sx).copy())


def write(path: Path, samples: Samples) -> None:
    n = len(samples)
    body = np.concatenate([samples.tables.reshape(n, -1),
                           samples.cells.reshape(n, -1)], axis=1)
    path.write_bytes(samples.header.to_bytes() + body.astype(np.uint8).tobytes())
