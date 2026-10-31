import type { Block, CellState, Dir } from '../types';

// A block is drawn as boxes, flat textured quads and labels, all in
// block-local coordinates (0..1 on each axis; +x east, +y up, +z south).

export type V3 = [number, number, number];
export type UV = [number, number];
export type BoxFace = 'top' | 'bottom' | 'north' | 'south' | 'west' | 'east';

export interface Look {
  texture?: string; // Minecraft block texture name
  color: string; // tint when textured, fill otherwise
  transparent?: boolean;
}

export interface BoxPart {
  kind: 'box';
  min: V3;
  max: V3;
  faces: Partial<Record<BoxFace, Look>>; // missing faces are not drawn
}

export interface QuadPart {
  kind: 'quad';
  corners: [V3, V3, V3, V3];
  uvs: [UV, UV, UV, UV];
  look: Look;
}

export interface LabelPart {
  kind: 'label';
  at: V3;
  text: string;
}

export type Part = BoxPart | QuadPart | LabelPart;

const WHITE = '#ffffff';
const DIODE_HEIGHT = 2 / 16;
const DUST_HEIGHT = 1 / 64;
const MAX_SIGNAL = 15;

// Minecraft's redstone dust colour for a power level.
export function dustColor(power: number): string {
  const f = power / MAX_SIGNAL;
  const red = power === 0 ? 0.3 : f * 0.6 + 0.4;
  const green = Math.max(0, f * f * 0.7 - 0.5);
  const toHex = (v: number) =>
    Math.round(Math.min(1, v) * 255)
      .toString(16)
      .padStart(2, '0');
  return `#${toHex(red)}${toHex(green)}00`;
}

// Clockwise quarter turns from north to `d` (seen from above).
export function turnsTowards(d: Dir): number {
  return { north: 0, east: 1, south: 2, west: 3, up: 0, down: 0 }[d];
}

const OPPOSITE: Record<Dir, Dir> = {
  north: 'south',
  south: 'north',
  east: 'west',
  west: 'east',
  up: 'down',
  down: 'up',
};

const OFFSET: Record<Dir, V3> = {
  north: [0, 0, -1],
  south: [0, 0, 1],
  east: [1, 0, 0],
  west: [-1, 0, 0],
  up: [0, 1, 0],
  down: [0, -1, 0],
};

// UVs for a horizontal quad given as NW, NE, SE, SW corners, with the image's
// top edge turned `turns` quarter turns clockwise from north.
export function topUVs(turns: number): [UV, UV, UV, UV] {
  const base: UV[] = [
    [0, 1],
    [1, 1],
    [1, 0],
    [0, 0],
  ];
  const k = ((turns % 4) + 4) % 4;
  return [0, 1, 2, 3].map((i) => base[(i - k + 4) % 4] as UV) as [UV, UV, UV, UV];
}

function flatQuad(x0: number, z0: number, x1: number, z1: number, y: number): [V3, V3, V3, V3] {
  return [
    [x0, y, z0],
    [x1, y, z0],
    [x1, y, z1],
    [x0, y, z1],
  ];
}

function uniformBox(min: V3, max: V3, look: Look): BoxPart {
  return {
    kind: 'box',
    min,
    max,
    faces: { top: look, bottom: look, north: look, south: look, west: look, east: look },
  };
}

function cube(look: Look): BoxPart {
  return uniformBox([0, 0, 0], [1, 1, 1], look);
}

function sidedCube(side: Look, top: Look, bottom: Look = top): BoxPart {
  return {
    kind: 'box',
    min: [0, 0, 0],
    max: [1, 1, 1],
    faces: { top, bottom, north: side, south: side, west: side, east: side },
  };
}

function dustParts(power: number, points: readonly Dir[]): Part[] {
  const look: Look = { texture: 'redstone_dust_dot', color: dustColor(power), transparent: true };
  const line: Look = { texture: 'redstone_dust_line0', color: dustColor(power), transparent: true };
  const parts: Part[] = [{ kind: 'quad', corners: flatQuad(0, 0, 1, 1, DUST_HEIGHT), uvs: topUVs(0), look }];
  for (const d of points) {
    // Half of the block from the centre towards d; the line texture runs
    // north-south, so east/west halves turn it a quarter.
    const x0 = d === 'east' ? 0.5 : 0;
    const x1 = d === 'west' ? 0.5 : 1;
    const z0 = d === 'south' ? 0.5 : 0;
    const z1 = d === 'north' ? 0.5 : 1;
    const corners = flatQuad(x0, z0, x1, z1, DUST_HEIGHT * 2);
    const turn = d === 'east' || d === 'west' ? 1 : 0;
    const full = topUVs(turn);
    // Map the half's corners into the full-block UV square.
    const uvs = corners.map(([x, , z]) => {
      const nw = full[0];
      const ne = full[1];
      const sw = full[3];
      return [nw[0] + (ne[0] - nw[0]) * x + (sw[0] - nw[0]) * z, nw[1] + (ne[1] - nw[1]) * x + (sw[1] - nw[1]) * z] as UV;
    }) as [UV, UV, UV, UV];
    parts.push({ kind: 'quad', corners, uvs, look: line });
  }
  return parts;
}

function torchParts(attached: Dir, lit: boolean): Part[] {
  const look: Look = { texture: lit ? 'redstone_torch' : 'redstone_torch_off', color: WHITE, transparent: true };
  let base: V3 = [0.5, 0, 0.5];
  let lean: V3 = [0, 0, 0];
  if (attached !== 'down') {
    const o = OFFSET[attached];
    base = [0.5 + o[0] * 0.5, 0.2, 0.5 + o[2] * 0.5];
    lean = [-o[0] * 0.25, 0, -o[2] * 0.25];
  }
  const parts: Part[] = [];
  for (const alongX of [true, false]) {
    const h: V3 = alongX ? [0.5, 0, 0] : [0, 0, 0.5];
    parts.push({
      kind: 'quad',
      corners: [
        [base[0] - h[0], base[1], base[2] - h[2]],
        [base[0] + h[0], base[1], base[2] + h[2]],
        [base[0] + h[0] + lean[0], base[1] + 1, base[2] + h[2] + lean[2]],
        [base[0] - h[0] + lean[0], base[1] + 1, base[2] - h[2] + lean[2]],
      ],
      uvs: [
        [0, 0],
        [1, 0],
        [1, 1],
        [0, 1],
      ],
      look,
    });
  }
  return parts;
}

function diodeParts(texture: string, facing: Dir, label: string): Part[] {
  const stone: Look = { texture: 'smooth_stone', color: WHITE };
  return [
    {
      kind: 'box',
      min: [0, 0, 0],
      max: [1, DIODE_HEIGHT, 1],
      faces: { bottom: stone, north: stone, south: stone, west: stone, east: stone },
    },
    {
      kind: 'quad',
      corners: flatQuad(0, 0, 1, 1, DIODE_HEIGHT),
      uvs: topUVs(turnsTowards(facing)),
      look: { texture, color: WHITE, transparent: true },
    },
    { kind: 'label', at: [0.5, DIODE_HEIGHT + 0.05, 0.5], text: label },
  ];
}

function observerParts(watched: Dir, powered: boolean): Part[] {
  const side: Look = { texture: 'observer_side', color: WHITE };
  const faceOf = (face: BoxFace): Look => {
    const dir: Dir = face === 'top' ? 'up' : face === 'bottom' ? 'down' : face;
    if (dir === watched) return { texture: 'observer_front', color: WHITE };
    if (dir === OPPOSITE[watched])
      return { texture: powered ? 'observer_back_on' : 'observer_back', color: WHITE };
    return side;
  };
  const box: BoxPart = {
    kind: 'box',
    min: [0, 0, 0],
    max: [1, 1, 1],
    faces: {
      bottom: faceOf('bottom'),
      north: faceOf('north'),
      south: faceOf('south'),
      west: faceOf('west'),
      east: faceOf('east'),
    },
  };
  const parts: Part[] = [box];
  if (watched === 'up' || watched === 'down') {
    box.faces.top = faceOf('top');
  } else {
    // The arrow on top points towards the output.
    parts.push({
      kind: 'quad',
      corners: flatQuad(0, 0, 1, 1, 1),
      uvs: topUVs(turnsTowards(OPPOSITE[watched])),
      look: { texture: 'observer_top', color: WHITE },
    });
  }
  return parts;
}

// Parts for a block; `state` adds live power (lit torches, dust colour...).
export function blockParts(block: Block, state?: CellState, livePortValue?: number): Part[] {
  const facing = block.facing ?? 'north';
  const power = state?.power ?? 0;
  const on = state?.on ?? false;
  switch (block.type) {
    case 'air':
      return [];
    case 'solid':
      return [cube({ texture: 'cyan_wool', color: WHITE })];
    case 'glass':
      return [cube({ texture: 'cyan_stained_glass', color: WHITE, transparent: true })];
    case 'redstone_block':
      return [cube({ texture: 'redstone_block', color: WHITE })];
    case 'note_block':
      return [cube({ texture: 'note_block', color: WHITE })];
    case 'chest':
      return [uniformBox([1 / 16, 0, 1 / 16], [15 / 16, 14 / 16, 15 / 16], { texture: 'oak_planks', color: WHITE })];
    case 'target':
      return [sidedCube({ texture: 'target_side', color: WHITE }, { texture: 'target_top', color: WHITE })];
    case 'hopper':
      return [sidedCube({ texture: 'hopper_outside', color: WHITE }, { texture: 'hopper_top', color: WHITE }, { texture: 'hopper_outside', color: WHITE })];
    case 'barrel':
      return [
        sidedCube(
          { texture: 'barrel_side', color: WHITE },
          { texture: 'barrel_top', color: WHITE },
          { texture: 'barrel_bottom', color: WHITE },
        ),
      ];
    case 'dust':
      return dustParts(power, state?.points ?? ['north', 'east', 'south', 'west']);
    case 'torch':
      return torchParts(facing, state ? on : true);
    case 'repeater':
      return diodeParts(on ? 'repeater_on' : 'repeater', facing, String(block.delay ?? 1));
    case 'comparator':
      return diodeParts(
        (state?.comparator ?? 0) > 0 ? 'comparator_on' : 'comparator',
        facing,
        block.mode === 'subtract' ? '-' : '=',
      );
    case 'observer':
      return observerParts(facing, on);
    case 'input':
    case 'output':
      return portParts(block, state, livePortValue ?? 0);
  }
}

function portParts(block: Block, state: CellState | undefined, value: number): Part[] {
  const isInput = block.type === 'input';
  const name = `${isInput ? 'IN' : 'OUT'}${(block.channel ?? 0) + 1}`;
  const facing = block.facing ?? 'north';
  const label: LabelPart = { kind: 'label', at: [0.5, 0.6, 0.5], text: name };
  switch (block.kind) {
    case 'dust':
      return [...dustParts(state?.power ?? value, state?.points ?? ['north', 'east', 'south', 'west']), { ...label, at: [0.5, 0.15, 0.5] }];
    case 'repeater':
      return [...diodeParts(value > 0 ? 'repeater_on' : 'repeater', facing, ''), { ...label, at: [0.5, 0.3, 0.5] }];
    case 'comparator':
      return [...diodeParts(value > 0 ? 'comparator_on' : 'comparator', facing, ''), { ...label, at: [0.5, 0.3, 0.5] }];
    case 'container':
      return [
        sidedCube(
          { texture: 'barrel_side', color: '#b8ffb8' },
          { texture: 'barrel_top', color: '#b8ffb8' },
          { texture: 'barrel_bottom', color: '#b8ffb8' },
        ),
        { ...label, at: [0.5, 1.15, 0.5] },
      ];
    default:
      return [cube({ texture: 'stone', color: '#ff00ff' }), label];
  }
}
