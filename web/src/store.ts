import * as THREE from 'three';
import { create } from 'zustand';

import { api } from './api';
import {
  addPort,
  blockAt,
  editWorld,
  inside,
  movePort,
  occupantAt,
  type PortRef,
  removePort,
  rotateBlock,
  rotatePort,
  setFixed,
  toggleForbidden,
} from './specEdit';
import type { Block, InputKind, OutputKind, SimState, Session, SolveResult, Spec, Vec3, World } from './types';

const CHECK_DELAY_MS = 250;
const POLL_INTERVAL_MS = 400;
const FULL_SIGNAL = 15;
export const DEFAULT_TIME_LIMIT = 60;

export type Tool = 'select' | 'input' | 'output' | 'keep' | 'empty' | 'place' | 'remove';
export const SPEC_TOOLS: readonly Tool[] = ['select', 'input', 'output', 'keep', 'empty'];
export const RESULT_TOOLS: readonly Tool[] = ['select', 'place', 'remove'];

// Where the pointer is: the cell under it (if it hit a block or marker) and
// the empty cell a placement would fill.
export interface Hover {
  hit: Vec3 | null;
  place: Vec3 | null;
}

export interface Result extends SolveResult {
  layout: World;
  spec: Spec;
  edited: boolean;
}

export interface Verification {
  checking: boolean;
  ok?: boolean;
  reason?: string;
}

interface State {
  session: Session | null;
  spec: Spec | null;
  specPath: string;
  specError: string | null;
  notice: string | null;

  textures: ReadonlyMap<string, THREE.Texture>;
  texturesJar: string | null;
  textureError: string | null;

  timeLimit: number;
  firstOnly: boolean;
  solving: boolean;
  solveSeconds: number;
  log: string[];
  solveError: string | null;
  result: Result | null;
  verification: Verification | null;
  sim: SimState | null;
  simError: string | null;

  view: 'spec' | 'result';
  topLayer: number;
  tool: Tool;
  selected: PortRef | null;
  newInputKind: InputKind;
  newOutputKind: OutputKind;
  brush: Block;
  hover: Hover | null;

  init: () => Promise<void>;
  loadSpec: (path: string) => Promise<void>;
  saveSpec: (path: string) => Promise<void>;
  updateSpec: (change: (spec: Spec) => Spec) => void;
  loadTextures: (jar?: string) => Promise<void>;
  setTimeLimit: (seconds: number) => void;
  setFirstOnly: (first: boolean) => void;
  startSolve: () => Promise<void>;
  cancelSolve: () => Promise<void>;
  setTopLayer: (y: number) => void;
  setSim: (sim: SimState) => void;
  setSimError: (error: string | null) => void;
  setView: (view: 'spec' | 'result') => void;
  setTool: (tool: Tool) => void;
  setNewInputKind: (kind: InputKind) => void;
  setNewOutputKind: (kind: OutputKind) => void;
  setBrush: (block: Block) => void;
  setHover: (hover: Hover | null) => void;
  select: (port: PortRef | null) => void;
  click: (hover: Hover) => void;
  rotate: () => void;
  deleteSelected: () => void;
}

let checkTimer: ReturnType<typeof setTimeout> | undefined;
let pollTimer: ReturnType<typeof setInterval> | undefined;

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function makeTextures(urls: Record<string, string>): Map<string, THREE.Texture> {
  const loader = new THREE.TextureLoader();
  const textures = new Map<string, THREE.Texture>();
  for (const [name, url] of Object.entries(urls)) {
    const texture = loader.load(url);
    texture.magFilter = THREE.NearestFilter;
    texture.minFilter = THREE.NearestFilter;
    texture.generateMipmaps = false;
    texture.colorSpace = THREE.SRGBColorSpace;
    textures.set(name, texture);
  }
  return textures;
}

export const useStore = create<State>((set, get) => {
  async function checkSpec(spec: Spec) {
    try {
      await api.checkSpec(spec);
      if (get().spec === spec) set({ specError: null });
    } catch (error) {
      if (get().spec === spec) set({ specError: errorText(error) });
    }
  }

  async function loadIntoSimulator(layout: World, inputs: number[]) {
    try {
      set({ sim: await api.resetSim(layout, inputs), simError: null });
    } catch (error) {
      set({ sim: null, simError: errorText(error) });
    }
  }

  async function pollSolve() {
    try {
      const status = await api.solveStatus();
      set({ solving: status.running, solveSeconds: status.seconds ?? 0, log: status.log });
      if (status.running) return;
      clearInterval(pollTimer);
      if (status.error) {
        set({ solveError: status.error });
        return;
      }
      const result = status.result;
      const spec = get().spec;
      if (!result?.layout || !spec) {
        set({ solveError: result ? `No circuit: ${result.status}` : 'The solver stopped without a result.' });
        return;
      }
      set({
        result: { ...result, layout: result.layout, spec, edited: false },
        verification: { checking: false, ok: true },
        view: 'result',
        tool: 'select',
        selected: null,
        topLayer: result.layout.size[1] - 1,
      });
      await loadIntoSimulator(
        result.layout,
        spec.inputs.map(() => 0),
      );
    } catch (error) {
      clearInterval(pollTimer);
      set({ solving: false, solveError: errorText(error) });
    }
  }

  // Changes a solved layout, then re-checks it and reloads the simulator.
  async function editResult(position: Vec3, block: Block | null) {
    const result = get().result;
    if (!result) return;
    const layout = editWorld(result.layout, position, block);
    const blockCount = layout.blocks.filter((b) => b.type !== 'input' && b.type !== 'output').length;
    set({ result: { ...result, layout, blockCount, edited: true }, verification: { checking: true } });
    await loadIntoSimulator(layout, get().sim?.inputs ?? result.spec.inputs.map(() => 0));
    try {
      const check = await api.verify(result.spec, layout);
      if (get().result?.layout !== layout) return;
      set({ verification: { checking: false, ok: check.ok, reason: check.failure?.reason } });
    } catch (error) {
      set({ verification: { checking: false, ok: false, reason: errorText(error) } });
    }
  }

  async function toggleInput(channel: number) {
    const sim = get().sim;
    if (!sim) return;
    const value = (sim.inputs[channel] ?? 0) > 0 ? 0 : FULL_SIGNAL;
    try {
      set({ sim: await api.setInput(channel, value, true) });
    } catch (error) {
      set({ simError: errorText(error) });
    }
  }

  function clickSpec(spec: Spec, { hit, place }: Hover) {
    const { tool, selected, newInputKind, newOutputKind, brush, updateSpec } = get();
    const free = (p: Vec3 | null): p is Vec3 => p !== null && inside(spec.size, p) && occupantAt(spec, p) === null;
    const occupant = hit ? occupantAt(spec, hit) : null;
    switch (tool) {
      case 'select':
        if (occupant?.kind === 'port') set({ selected: occupant.port });
        else if (selected && free(place)) updateSpec((s) => movePort(s, selected, place));
        else set({ selected: null });
        return;
      case 'input':
      case 'output': {
        if (!free(place)) return;
        const input = tool === 'input';
        updateSpec((s) => addPort(s, input, place, input ? newInputKind : newOutputKind, 'east'));
        const count = input ? spec.inputs.length : spec.outputs.length;
        set({ selected: { input, index: count } });
        return;
      }
      case 'keep':
        if (hit && occupant?.kind === 'fixed') updateSpec((s) => setFixed(s, hit, null));
        else if (free(place)) updateSpec((s) => setFixed(s, place, brush));
        return;
      case 'empty':
        if (hit && occupant?.kind === 'forbidden') updateSpec((s) => toggleForbidden(s, hit));
        else if (free(place)) updateSpec((s) => toggleForbidden(s, place));
        return;
      default:
        return;
    }
  }

  function clickResult(result: Result, { hit, place }: Hover) {
    const { tool, brush } = get();
    const hitBlock = hit ? blockAt(result.layout, hit) : undefined;
    switch (tool) {
      case 'select':
        if (hitBlock?.type === 'input') void toggleInput(hitBlock.channel ?? 0);
        return;
      case 'place':
        if (place && inside(result.layout.size, place) && !blockAt(result.layout, place))
          void editResult(place, brush);
        return;
      case 'remove':
        if (hit && hitBlock && hitBlock.type !== 'input' && hitBlock.type !== 'output') void editResult(hit, null);
        return;
      default:
        return;
    }
  }

  return {
    session: null,
    spec: null,
    specPath: '',
    specError: null,
    notice: null,
    textures: new Map(),
    texturesJar: null,
    textureError: null,
    timeLimit: DEFAULT_TIME_LIMIT,
    firstOnly: false,
    solving: false,
    solveSeconds: 0,
    log: [],
    solveError: null,
    result: null,
    verification: null,
    sim: null,
    simError: null,
    view: 'spec',
    topLayer: 64,
    tool: 'select',
    selected: null,
    newInputKind: 'comparator',
    newOutputKind: 'dust',
    brush: { type: 'solid' },
    hover: null,

    init: async () => {
      const session = await api.session();
      set({ session });
      await get().loadSpec(session.defaultPath);
      if (session.jar) void get().loadTextures(session.jar);
    },

    loadSpec: async (path) => {
      try {
        const { spec } = await api.loadSpec(path);
        set({
          spec,
          specPath: path,
          notice: `Loaded ${path}`,
          result: null,
          sim: null,
          view: 'spec',
          selected: null,
          topLayer: spec.size[1] - 1,
        });
        await checkSpec(spec);
      } catch (error) {
        set({ notice: `Could not load: ${errorText(error)}` });
      }
    },

    saveSpec: async (path) => {
      const spec = get().spec;
      if (!spec) return;
      try {
        await api.saveSpec(path, spec);
        set({ specPath: path, notice: `Saved ${path}` });
      } catch (error) {
        set({ notice: `Could not save: ${errorText(error)}` });
      }
    },

    updateSpec: (change) => {
      const current = get().spec;
      if (!current) return;
      const spec = change(current);
      set({ spec, view: 'spec' });
      clearTimeout(checkTimer);
      checkTimer = setTimeout(() => void checkSpec(spec), CHECK_DELAY_MS);
    },

    loadTextures: async (jar) => {
      try {
        const { jar: used, textures } = await api.textures(jar);
        set({ textures: makeTextures(textures), texturesJar: used, textureError: null });
      } catch (error) {
        set({ textureError: errorText(error) });
      }
    },

    setTimeLimit: (seconds) => set({ timeLimit: seconds }),
    setFirstOnly: (first) => set({ firstOnly: first }),

    startSolve: async () => {
      const { spec, timeLimit, firstOnly } = get();
      if (!spec) return;
      try {
        await api.startSolve(spec, { timeLimit, first: firstOnly });
        set({ solving: true, solveError: null, log: [], solveSeconds: 0 });
        clearInterval(pollTimer);
        pollTimer = setInterval(() => void pollSolve(), POLL_INTERVAL_MS);
      } catch (error) {
        set({ solveError: errorText(error) });
      }
    },

    cancelSolve: async () => {
      await api.cancelSolve();
    },

    setTopLayer: (y) => set({ topLayer: y }),
    setSim: (sim) => set({ sim, simError: null }),
    setSimError: (error) => set({ simError: error }),
    setView: (view) => set({ view, tool: 'select', selected: null }),
    setTool: (tool) => set({ tool, selected: tool === 'select' ? get().selected : null }),
    setNewInputKind: (kind) => set({ newInputKind: kind }),
    setNewOutputKind: (kind) => set({ newOutputKind: kind }),
    setBrush: (block) => set({ brush: block }),
    setHover: (hover) => set({ hover }),
    select: (port) => set({ selected: port }),

    click: (hover) => {
      const { view, spec, result } = get();
      if (view === 'result' && result) clickResult(result, hover);
      else if (spec) clickSpec(spec, hover);
    },

    rotate: () => {
      const { selected, view, hover, result, updateSpec } = get();
      if (view === 'spec' && selected) {
        updateSpec((s) => rotatePort(s, selected));
        return;
      }
      if (view === 'result' && result && hover?.hit) {
        const block = blockAt(result.layout, hover.hit);
        if (block && block.type !== 'input' && block.type !== 'output') {
          void editResult(hover.hit, rotateBlock(block));
          return;
        }
      }
      set({ brush: rotateBlock(get().brush) });
    },

    deleteSelected: () => {
      const { selected, updateSpec } = get();
      if (!selected) return;
      updateSpec((s) => removePort(s, selected));
      set({ selected: null });
    },
  };
});
