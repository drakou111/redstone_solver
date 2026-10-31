import { describe, expect, it } from 'vitest';

import { blockParts, dustColor, topUVs, turnsTowards } from './blockModel';
import { partsToQuads } from './geometry';

describe('topUVs', () => {
  it('maps image-up to north without turns', () => {
    // NW corner shows the image's top-left.
    expect(topUVs(0)[0]).toEqual([0, 1]);
  });

  it('a quarter turn puts the image top-left at the north-east corner', () => {
    expect(topUVs(1)[1]).toEqual([0, 1]);
    expect(topUVs(turnsTowards('east'))).toEqual(topUVs(1));
  });
});

describe('dustColor', () => {
  it('is dark when unpowered and bright red at full power', () => {
    expect(dustColor(0)).toBe('#4d0000');
    expect(dustColor(15)).toBe('#ff3300');
  });
});

describe('blockParts', () => {
  it('draws a full cube as six faces', () => {
    expect(partsToQuads(blockParts({ type: 'solid' }))).toHaveLength(6);
  });

  it('draws only the dust arms that are connected', () => {
    const parts = blockParts({ type: 'dust' }, { position: [0, 0, 0], power: 15, on: false, locked: false, comparator: 0, points: ['east', 'west'] });
    expect(parts.filter((p) => p.kind === 'quad')).toHaveLength(3);
  });

  it('labels ports with their channel', () => {
    const parts = blockParts({ type: 'input', kind: 'comparator', channel: 1, facing: 'east' });
    expect(parts.some((p) => p.kind === 'label' && p.text === 'IN2')).toBe(true);
  });

  it('uses lit textures for powered components', () => {
    const lit = blockParts({ type: 'torch', facing: 'down' }, { position: [0, 0, 0], power: 0, on: true, locked: false, comparator: 0 });
    expect(lit.some((p) => p.kind === 'quad' && p.look.texture === 'redstone_torch')).toBe(true);
  });
});
