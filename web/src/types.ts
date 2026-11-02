// Mirrors the JSON produced by src/host/json_io.cpp.

export type Dir = 'down' | 'up' | 'north' | 'south' | 'west' | 'east';
export type Vec3 = [number, number, number];
export type SignalRange = [number, number];

export const HORIZONTAL: readonly Dir[] = ['north', 'east', 'south', 'west'];

export type InputKind = 'comparator' | 'repeater' | 'dust' | 'container';
export type OutputKind = 'dust' | 'repeater' | 'comparator';

export const INPUT_KINDS: readonly InputKind[] = ['comparator', 'repeater', 'dust', 'container'];
export const OUTPUT_KINDS: readonly OutputKind[] = ['dust', 'repeater', 'comparator'];

// A port with a non-empty region is placed by the solver in one of those
// cells (position and facing are then ignored).
export interface InputPort {
  kind: InputKind;
  position: Vec3;
  facing: Dir;
  region: Vec3[];
}

export interface OutputPort {
  kind: OutputKind;
  position: Vec3;
  facing: Dir;
  region: Vec3[];
}

export interface TruthRow {
  inputs: SignalRange[];
  outputs: SignalRange[];
}

export interface Timing {
  earliest: number | null;
  latest: number | null;
  holdPrevious: boolean;
  settle: number;
  horizon: number;
}

export interface Spec {
  size: Vec3;
  inputs: InputPort[];
  outputs: OutputPort[];
  rows: TruthRow[];
  timing: Timing;
  palette: string[];
  feedback: boolean;
  supportLayer: boolean; // bottom layer holds only solid supports
  tiled: [boolean, boolean, boolean];
  fixed: PlacedBlock[]; // blocks the solver must keep
  forbidden: Vec3[]; // cells the solver must leave empty
}

export type BlockType =
  | 'air'
  | 'solid'
  | 'glass'
  | 'dust'
  | 'torch'
  | 'redstone_block'
  | 'repeater'
  | 'comparator'
  | 'target'
  | 'note_block'
  | 'hopper'
  | 'barrel'
  | 'chest'
  | 'observer'
  | 'input'
  | 'output';

export interface Block {
  type: BlockType;
  facing?: Dir;
  delay?: number;
  mode?: 'compare' | 'subtract';
  signal?: number;
  kind?: InputKind | OutputKind;
  channel?: number;
}

export interface PlacedBlock extends Block {
  position: Vec3;
}

export interface World {
  size: Vec3;
  blocks: PlacedBlock[];
}

export interface CellState {
  position: Vec3;
  power: number;
  on: boolean;
  locked: boolean;
  comparator: number;
  points?: Dir[];
}

export interface SimState {
  time: number;
  pending: boolean;
  inputs: number[];
  outputs: number[];
  cells: CellState[];
}

export interface SolveResult {
  status: string;
  optimal: boolean;
  seconds: number;
  refinements: number;
  blockCount?: number;
  layout?: World;
}

export interface SolveStatus {
  running: boolean;
  seconds?: number;
  log: string[];
  error?: string;
  result?: SolveResult;
}

export interface Session {
  examples: string[];
  defaultPath: string;
  jar: string | null;
  palette: string[];
}

export function key(p: Vec3): string {
  return `${p[0]},${p[1]},${p[2]}`;
}
