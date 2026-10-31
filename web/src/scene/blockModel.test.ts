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

  it('moves the repeater delay torch back with the delay and shows a lock bar', () => {
    const torchesOf = (delay: number, locked: boolean) =>
      blockParts({ type: 'repeater', facing: 'north', delay }, { position: [0, 0, 0], power: 0, on: false, locked, comparator: 0 }).filter(
        (p) => p.kind === 'box' && p.faces.top?.texture === 'redstone_torch_off',
      );
    const delayTorchZ = (delay: number) => {
      const torch = torchesOf(delay, false)[1];
      return torch?.kind === 'box' ? torch.min[2] : -1;
    };
    expect(delayTorchZ(3)).toBeGreaterThan(delayTorchZ(1));
    expect(torchesOf(2, true)).toHaveLength(1);
  });

  it('lights the comparator front torch in subtract mode', () => {
    const lit = (mode: 'compare' | 'subtract') =>
      blockParts({ type: 'comparator', facing: 'east', mode }).filter((p) => p.kind === 'box' && p.faces.top?.texture === 'redstone_torch').length;
    expect(lit('subtract')).toBe(1);
    expect(lit('compare')).toBe(0);
  });

  it('uses lit textures for powered components', () => {
    const lit = blockParts({ type: 'torch', facing: 'down' }, { position: [0, 0, 0], power: 0, on: true, locked: false, comparator: 0 });
    expect(lit.some((p) => p.kind === 'quad' && p.look.texture === 'redstone_torch')).toBe(true);
  });
});
