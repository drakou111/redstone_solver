import type { SimState, Session, Spec, SolveStatus, World } from './types';

// The page is served by the desktop app with ?token=... in its URL; every
// API call must carry it.
const token = new URLSearchParams(window.location.search).get('token') ?? '';

export class ApiError extends Error {}

async function call<T>(route: string, body: unknown = {}): Promise<T> {
  const response = await fetch(`/api/${route}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Token': token },
    body: JSON.stringify(body),
  });
  const data: unknown = await response.json();
  if (!response.ok) {
    const message = (data as { error?: string }).error ?? response.statusText;
    throw new ApiError(message);
  }
  return data as T;
}

export const api = {
  session: () => call<Session>('session'),
  loadSpec: (path: string) => call<{ spec: Spec; path: string }>('spec/load', { path }),
  saveSpec: (path: string, spec: Spec) => call<{ path: string }>('spec/save', { path, spec }),
  checkSpec: (spec: Spec) => call<{ world: World }>('spec/check', { spec }),
  startSolve: (spec: Spec, options: { timeLimit: number; first: boolean }) =>
    call<object>('solve/start', { spec, options }),
  solveStatus: () => call<SolveStatus>('solve/status'),
  cancelSolve: () => call<object>('solve/cancel'),
  resetSim: (world: World, inputs: number[]) => call<SimState>('sim/reset', { world, inputs }),
  setInput: (channel: number, value: number, settle: boolean) =>
    call<SimState>('sim/input', { channel, value, settle }),
  tick: (count: number) => call<SimState>('sim/tick', { count }),
  settle: () => call<SimState>('sim/settle'),
  verify: (spec: Spec, world: World) =>
    call<{ ok: boolean; failure?: { from: number[] | null; to: number[]; reason: string } }>('verify', {
      spec,
      world,
    }),
  exportCommands: (world: World) => call<{ text: string }>('export', { world }),
  textures: (jar?: string) =>
    call<{ jar: string; textures: Record<string, string> }>('textures', jar ? { jar } : {}),
};
