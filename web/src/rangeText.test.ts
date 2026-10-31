import { describe, expect, it } from 'vitest';

import { formatRange, parseRange } from './rangeText';

describe('parseRange', () => {
  it('reads anything, single values and ranges', () => {
    expect(parseRange('*')).toEqual([0, 15]);
    expect(parseRange('')).toEqual([0, 15]);
    expect(parseRange(' 7 ')).toEqual([7, 7]);
    expect(parseRange('1..15')).toEqual([1, 15]);
    expect(parseRange('2-4')).toEqual([2, 4]);
  });

  it('rejects invalid text', () => {
    expect(parseRange('16')).toBeNull();
    expect(parseRange('5..2')).toBeNull();
    expect(parseRange('abc')).toBeNull();
  });
});

describe('formatRange', () => {
  it('round-trips through parseRange', () => {
    for (const text of ['*', '0', '1..15', '3..4']) {
      const range = parseRange(text);
      expect(range).not.toBeNull();
      if (range) expect(formatRange(range)).toBe(text);
    }
  });
});
