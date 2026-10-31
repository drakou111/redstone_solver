import type { SignalRange } from './types';

export const MAX_SIGNAL = 15;
export const ANY: SignalRange = [0, MAX_SIGNAL];

// "*" or "" -> anything, "5" -> 5..5, "1..15" or "1-15" -> 1..15.
// Returns null for text that is not a valid range.
export function parseRange(text: string): SignalRange | null {
  const t = text.trim();
  if (t === '' || t === '*') return ANY;
  const match = /^(\d+)\s*(?:(?:\.\.|-)\s*(\d+))?$/.exec(t);
  if (!match) return null;
  const min = Number(match[1]);
  const max = match[2] === undefined ? min : Number(match[2]);
  if (min > max || max > MAX_SIGNAL) return null;
  return [min, max];
}

export function formatRange([min, max]: SignalRange): string {
  if (min === 0 && max === MAX_SIGNAL) return '*';
  return min === max ? String(min) : `${min}..${max}`;
}
