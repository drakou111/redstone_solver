// Pure spec edits used by the 3D tools; each returns a new spec.

import type { Block, Dir, InputKind, InputPort, OutputKind, OutputPort, PlacedBlock, SignalRange, Spec, Vec3, World } from './types';
import { HORIZONTAL, key } from './types';

const ANY: SignalRange = [0, 15];
const ALL_DIRS: readonly Dir[] = ['down', 'up', 'north', 'east', 'south', 'west'];
const TORCH_DIRS: readonly Dir[] = ['down', 'north', 'east', 'south', 'west'];

export interface PortRef {
  input: boolean;
  index: number;
}

export function inside(size: Vec3, p: Vec3): boolean {
  return p.every((v, i) => v >= 0 && v < (size[i] ?? 0));
}

// What occupies a cell in the spec, if anything.
export type Occupant =
  | { kind: 'port'; port: PortRef }
  | { kind: 'fixed'; index: number }
  | { kind: 'forbidden'; index: number };

export function isFree(port: { region: Vec3[] }): boolean {
  return port.region.length > 0;
}

export function portAt(spec: Spec, ref: PortRef): InputPort | OutputPort | undefined {
  return ref.input ? spec.inputs[ref.index] : spec.outputs[ref.index];
}

export function portName(ref: PortRef): string {
  return `${ref.input ? 'IN' : 'OUT'}${ref.index + 1}`;
}

// Ports the solver places, in IN1.., OUT1.. order.
export function freePorts(spec: Spec): PortRef[] {
  return [
    ...spec.inputs.flatMap((p, index) => (isFree(p) ? [{ input: true, index }] : [])),
    ...spec.outputs.flatMap((p, index) => (isFree(p) ? [{ input: false, index }] : [])),
  ];
}

export function occupantAt(spec: Spec, p: Vec3): Occupant | null {
  const k = key(p);
  const input = spec.inputs.findIndex((port) => !isFree(port) && key(port.position) === k);
  if (input >= 0) return { kind: 'port', port: { input: true, index: input } };
  const output = spec.outputs.findIndex((port) => !isFree(port) && key(port.position) === k);
  if (output >= 0) return { kind: 'port', port: { input: false, index: output } };
  const fixed = spec.fixed.findIndex((b) => key(b.position) === k);
  if (fixed >= 0) return { kind: 'fixed', index: fixed };
  const forbidden = (spec.forbidden).findIndex((c) => key(c) === k);
  if (forbidden >= 0) return { kind: 'forbidden', index: forbidden };
  return null;
}

export function addPort(spec: Spec, input: boolean, position: Vec3, kind: InputKind | OutputKind, facing: Dir): Spec {
  const rows = spec.rows.map((r) =>
    input ? { ...r, inputs: [...r.inputs, ANY] } : { ...r, outputs: [...r.outputs, ANY] },
  );
  return input
    ? { ...spec, rows, inputs: [...spec.inputs, { kind: kind as InputKind, position, facing, region: [] }] }
    : { ...spec, rows, outputs: [...spec.outputs, { kind: kind as OutputKind, position, facing, region: [] }] };
}

export function removePort(spec: Spec, port: PortRef): Spec {
  const keep = (_: unknown, i: number) => i !== port.index;
  const rows = spec.rows.map((r) =>
    port.input ? { ...r, inputs: r.inputs.filter(keep) } : { ...r, outputs: r.outputs.filter(keep) },
  );
  return port.input
    ? { ...spec, rows, inputs: spec.inputs.filter(keep) }
    : { ...spec, rows, outputs: spec.outputs.filter(keep) };
}

function updatePort(spec: Spec, port: PortRef, change: (p: InputPort | OutputPort) => Partial<InputPort | OutputPort>): Spec {
  return port.input
    ? { ...spec, inputs: spec.inputs.map((p, i) => (i === port.index ? ({ ...p, ...change(p) } as InputPort) : p)) }
    : { ...spec, outputs: spec.outputs.map((p, i) => (i === port.index ? ({ ...p, ...change(p) } as OutputPort) : p)) };
}

export function movePort(spec: Spec, port: PortRef, position: Vec3): Spec {
  return updatePort(spec, port, () => ({ position }));
}

// Switches between a fixed port and one the solver places. A new region
// starts with the port's current cell; a port made fixed again goes to the
// first cell of its region.
export function setPortFree(spec: Spec, port: PortRef, free: boolean): Spec {
  return updatePort(spec, port, (p) =>
    free ? { region: isFree(p) ? p.region : [p.position] } : { position: p.region[0] ?? p.position, region: [] },
  );
}

// Adds or removes one cell of a free port's region (a region never becomes
// empty, since that would make the port fixed).
export function toggleRegionCell(spec: Spec, port: PortRef, cell: Vec3): Spec {
  const k = key(cell);
  return updatePort(spec, port, (p) => {
    if (!p.region.some((c) => key(c) === k)) return { region: [...p.region, cell] };
    const region = p.region.filter((c) => key(c) !== k);
    return region.length > 0 ? { region } : {};
  });
}

export function regionContains(spec: Spec, port: PortRef, cell: Vec3): boolean {
  const k = key(cell);
  return portAt(spec, port)?.region.some((c) => key(c) === k) ?? false;
}

export function nextDir(current: Dir, choices: readonly Dir[]): Dir {
  const i = choices.indexOf(current);
  return choices[(i + 1) % choices.length] ?? choices[0] ?? 'north';
}

// Rotating a port only makes sense for repeater and comparator kinds.
export function rotatePort(spec: Spec, port: PortRef): Spec {
  const rotate = <T extends { facing: Dir }>(p: T): T => ({ ...p, facing: nextDir(p.facing, HORIZONTAL) });
  return port.input
    ? { ...spec, inputs: spec.inputs.map((p, i) => (i === port.index ? rotate(p) : p)) }
    : { ...spec, outputs: spec.outputs.map((p, i) => (i === port.index ? rotate(p) : p)) };
}

// Facings a block can take, in rotation order (empty if it has none).
export function facingChoices(block: Block): readonly Dir[] {
  switch (block.type) {
    case 'torch':
      return TORCH_DIRS;
    case 'observer':
      return ALL_DIRS;
    case 'repeater':
    case 'comparator':
      return HORIZONTAL;
    default:
      return [];
  }
}

export function rotateBlock(block: Block): Block {
  const choices = facingChoices(block);
  if (choices.length === 0) return block;
  return { ...block, facing: nextDir(block.facing ?? choices[0] ?? 'north', choices) };
}

export function setFixed(spec: Spec, position: Vec3, block: Block | null): Spec {
  const fixed = spec.fixed.filter((b) => key(b.position) !== key(position));
  if (block) fixed.push({ ...block, position });
  return { ...spec, fixed };
}

export function toggleForbidden(spec: Spec, position: Vec3): Spec {
  const forbidden = spec.forbidden;
  const k = key(position);
  return forbidden.some((c) => key(c) === k)
    ? { ...spec, forbidden: forbidden.filter((c) => key(c) !== k) }
    : { ...spec, forbidden: [...forbidden, position] };
}

// Replaces (or with null removes) the block at a position in a solved layout.
export function editWorld(world: World, position: Vec3, block: Block | null): World {
  const blocks: PlacedBlock[] = world.blocks.filter((b) => key(b.position) !== key(position));
  if (block && block.type !== 'air') blocks.push({ ...block, position });
  return { ...world, blocks };
}

export function blockAt(world: World, position: Vec3): PlacedBlock | undefined {
  const k = key(position);
  return world.blocks.find((b) => key(b.position) === k);
}

// The spec as blocks: ports and kept blocks (what the 3D editor shows).
// Ports the solver places have no block yet; their regions are drawn
// separately.
export function specWorld(spec: Spec): World {
  const blocks: PlacedBlock[] = [
    ...spec.inputs.flatMap((p, channel): PlacedBlock[] =>
      isFree(p) ? [] : [{ type: 'input', kind: p.kind, channel, facing: p.facing, position: p.position }],
    ),
    ...spec.outputs.flatMap((p, channel): PlacedBlock[] =>
      isFree(p) ? [] : [{ type: 'output', kind: p.kind, channel, facing: p.facing, position: p.position }],
    ),
    ...spec.fixed,
  ];
  return { size: spec.size, blocks };
}
