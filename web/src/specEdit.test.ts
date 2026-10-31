import { describe, expect, it } from 'vitest';

import {
  addPort,
  editWorld,
  movePort,
  occupantAt,
  removePort,
  rotateBlock,
  rotatePort,
  setFixed,
  toggleForbidden,
} from './specEdit';
import type { Spec } from './types';

function spec(): Spec {
  return {
    size: [4, 2, 2],
    inputs: [{ kind: 'comparator', position: [0, 1, 0], facing: 'east' }],
    outputs: [{ kind: 'dust', position: [3, 1, 0], facing: 'north' }],
    rows: [{ inputs: [[1, 15]], outputs: [[0, 0]] }],
    timing: { earliest: null, latest: null, holdPrevious: false, settle: 4, horizon: 8 },
    palette: ['solid'],
    feedback: false,
    tiled: [false, false, false],
    fixed: [],
    forbidden: [],
  };
}

describe('ports', () => {
  it('adding an input widens every truth table row', () => {
    const s = addPort(spec(), true, [0, 1, 1], 'repeater', 'east');
    expect(s.inputs).toHaveLength(2);
    expect(s.rows[0]?.inputs).toEqual([
      [1, 15],
      [0, 15],
    ]);
  });

  it('removing a port drops its truth table column', () => {
    const s = removePort(addPort(spec(), true, [0, 1, 1], 'dust', 'east'), { input: true, index: 0 });
    expect(s.inputs[0]?.kind).toBe('dust');
    expect(s.rows[0]?.inputs).toEqual([[0, 15]]);
  });

  it('moves and rotates ports', () => {
    let s = movePort(spec(), { input: true, index: 0 }, [0, 1, 1]);
    s = rotatePort(s, { input: true, index: 0 });
    expect(s.inputs[0]).toMatchObject({ position: [0, 1, 1], facing: 'south' });
  });
});

describe('cells', () => {
  it('finds what occupies a cell', () => {
    let s = setFixed(spec(), [1, 0, 0], { type: 'solid' });
    s = toggleForbidden(s, [2, 0, 0]);
    expect(occupantAt(s, [0, 1, 0])).toEqual({ kind: 'port', port: { input: true, index: 0 } });
    expect(occupantAt(s, [1, 0, 0])?.kind).toBe('fixed');
    expect(occupantAt(s, [2, 0, 0])?.kind).toBe('forbidden');
    expect(occupantAt(s, [2, 1, 1])).toBeNull();
  });

  it('toggling a forbidden cell twice clears it', () => {
    const s = toggleForbidden(toggleForbidden(spec(), [1, 1, 1]), [1, 1, 1]);
    expect(s.forbidden).toEqual([]);
  });

  it('replacing a kept block keeps one entry per cell', () => {
    let s = setFixed(spec(), [1, 0, 0], { type: 'solid' });
    s = setFixed(s, [1, 0, 0], { type: 'glass' });
    expect(s.fixed).toEqual([{ type: 'glass', position: [1, 0, 0] }]);
    expect(setFixed(s, [1, 0, 0], null).fixed).toEqual([]);
  });
});

describe('blocks', () => {
  it('rotates through the facings a block can take', () => {
    expect(rotateBlock({ type: 'torch', facing: 'west' }).facing).toBe('down');
    expect(rotateBlock({ type: 'repeater', facing: 'west' }).facing).toBe('north');
    expect(rotateBlock({ type: 'solid' })).toEqual({ type: 'solid' });
  });

  it('edits a solved layout', () => {
    const world = { size: [2, 1, 1] as [number, number, number], blocks: [{ type: 'solid' as const, position: [0, 0, 0] as [number, number, number] }] };
    expect(editWorld(world, [0, 0, 0], null).blocks).toEqual([]);
    expect(editWorld(world, [1, 0, 0], { type: 'glass' }).blocks).toHaveLength(2);
  });
});
