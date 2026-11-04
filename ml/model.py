"""A 3D conv net that fills in hidden cells of a circuit, steered by the truth
table the circuit should compute (masked prediction, MaskGIT style)."""

import math

import torch
import torch.nn as nn
import torch.nn.functional as F


class FilmBlock(nn.Module):
    """Residual 3D conv block whose features are scaled and shifted by the
    truth-table embedding."""

    def __init__(self, width: int, cond: int):
        super().__init__()
        self.norm1 = nn.GroupNorm(8, width)
        self.conv1 = nn.Conv3d(width, width, 3, padding=1)
        self.norm2 = nn.GroupNorm(8, width)
        self.conv2 = nn.Conv3d(width, width, 3, padding=1)
        self.film = nn.Linear(cond, 2 * width)

    def forward(self, x: torch.Tensor, c: torch.Tensor) -> torch.Tensor:
        scale, shift = self.film(c)[:, :, None, None, None].chunk(2, dim=1)
        h = self.conv1(F.silu(self.norm1(x)))
        h = self.norm2(h) * (1 + scale) + shift
        h = self.conv2(F.silu(h))
        return x + h


class Sketcher(nn.Module):
    def __init__(self, classes: int, static_channels: int, table_bits: int,
                 width: int = 128, depth: int = 10, cond: int = 256):
        super().__init__()
        self.classes = classes
        # visible class one-hot + hidden flag + ports/coordinates
        self.stem = nn.Conv3d(classes + 1 + static_channels, width, 3, padding=1)
        # each table bit: (cares, wants on)
        self.table = nn.Sequential(nn.Linear(2 * table_bits, cond), nn.SiLU(),
                                   nn.Linear(cond, cond), nn.SiLU())
        self.blocks = nn.ModuleList(FilmBlock(width, cond) for _ in range(depth))
        self.head = nn.Sequential(nn.GroupNorm(8, width), nn.SiLU(),
                                  nn.Conv3d(width, classes, 1))

    def forward(self, cells: torch.Tensor, hidden: torch.Tensor,
                static: torch.Tensor, table: torch.Tensor) -> torch.Tensor:
        """cells (b, y, z, x) long, hidden (b, y, z, x) bool, static
        (s, y, z, x) float, table (b, 2 * bits) float -> logits
        (b, classes, y, z, x)."""
        b = cells.shape[0]
        onehot = F.one_hot(cells, self.classes).permute(0, 4, 1, 2, 3).float()
        onehot = onehot * (~hidden)[:, None].float()
        x = torch.cat([onehot, hidden[:, None].float(),
                       static[None].expand(b, -1, -1, -1, -1)], dim=1)
        x = self.stem(x)
        c = self.table(table)
        for block in self.blocks:
            x = block(x, c)
        return self.head(x)


def table_features(bits: torch.Tensor, cares: torch.Tensor) -> torch.Tensor:
    """bits/cares (b, n) bool -> (b, 2n) float."""
    return torch.cat([cares.float(), (bits & cares).float()], dim=1)


def mask_ratio(u: torch.Tensor) -> torch.Tensor:
    """Cosine schedule: share of cells hidden for uniform u in [0, 1)."""
    return torch.cos(u * math.pi / 2)


@torch.no_grad()
def sample(model: Sketcher, static: torch.Tensor, table: torch.Tensor,
           fixed: torch.Tensor, fixed_cells: torch.Tensor, steps: int = 10,
           temperature: float = 1.0) -> torch.Tensor:
    """Fills every cell not in `fixed` (y, z, x bool; their classes in
    fixed_cells) over `steps` rounds, keeping the most confident picks each
    round. Returns (b, y, z, x) long."""
    b = table.shape[0]
    shape = (b, *fixed.shape)
    cells = fixed_cells[None].expand(shape).clone()
    hidden = (~fixed)[None].expand(shape).clone()
    free = int((~fixed).sum())
    for step in range(steps):
        logits = model(cells, hidden, static, table) / temperature
        probs = logits.softmax(dim=1).permute(0, 2, 3, 4, 1)  # b y z x c
        picked = torch.distributions.Categorical(probs=probs).sample()
        conf = probs.gather(-1, picked[..., None])[..., 0]
        conf = torch.where(hidden, conf, torch.full_like(conf, -1.0))
        # cells still hidden after this round
        keep_hidden = int(free * mask_ratio(torch.tensor((step + 1) / steps)))
        reveal = hidden.flatten(1).sum(1) - keep_hidden
        order = conf.flatten(1).argsort(dim=1, descending=True)
        rank = torch.empty_like(order)
        rank.scatter_(1, order, torch.arange(order.shape[1], device=order.device)
                      .expand_as(order))
        newly = (rank < reveal[:, None]).view(shape) & hidden
        cells = torch.where(newly, picked, cells)
        hidden = hidden & ~newly
    return cells
