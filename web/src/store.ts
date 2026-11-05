import * as THREE from 'three';
import { create } from 'zustand';

import { api } from './api';
import {
  addPort,
  blockAt,
  editWorld,
  freePorts,
  inside,
  isFree,
  occupantAt,
  type PortRef,
  portAt,
  regionContains,
  removeHint,
  removePort,
  rotateBlock,
  rotatePort,
  setFixed,
  setPortFree as withPortFree,
  toggleForbidden,
  toggleRegionCell,
} from './specEdit';
import type { Block, InputKind, OutputKind, SimState, Session, SolveResult, Spec, Vec3, World } from './types';

const CHECK_DELAY_MS = 250;
const POLL_INTERVAL_MS = 400;
const FULL_SIGNAL = 15;
export const DEFAULT_TIME_LIMIT = 60;
export const DEFAULT_SKETCH_TIME = 3; // seconds per sketch repair step (solver default)

// What a right-click places in the spec view (the circuit view always
// places the block brush).
// 'region' adds cells where the solver may put the selected free port.
export type Placing = 'input' | 'output' | 'keep' | 'empty' | 'region';
export const PLACING: readonly Placing[] = ['input', 'output', 'keep', 'empty', 'region'];

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
  onePlacement: boolean;
  sketchTime: number;
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
  placing: Placing;
  newInputKind: InputKind;
  newOutputKind: OutputKind;
  brush: Block;
  regionPort: PortRef | null;
  hover: Hover | null;

  init: () => Promise<void>;
  loadSpec: (path: string) => Promise<void>;
  saveSpec: (path: string) => Promise<void>;
  updateSpec: (change: (spec: Spec) => Spec) => void;
  loadTextures: (jar?: string) => Promise<void>;
  setTimeLimit: (seconds: number) => void;
  setFirstOnly: (first: boolean) => void;
  setOnePlacement: (one: boolean) => void;
  setSketchTime: (seconds: number) => void;
  startSolve: () => Promise<void>;
  cancelSolve: () => Promise<void>;
  setTopLayer: (y: number) => void;
  setSim: (sim: SimState) => void;
  setSimError: (error: string | null) => void;
  setView: (view: 'spec' | 'result') => void;
  setPlacing: (placing: Placing) => void;
  setNewInputKind: (kind: InputKind) => void;
  setNewOutputKind: (kind: OutputKind) => void;
  setBrush: (block: Block) => void;
  // Selects the free port whose region the 'region' tool edits.
  drawRegion: (port: PortRef) => void;
  setPortFree: (port: PortRef, free: boolean) => void;
  setHover: (hover: Hover | null) => void;
  place: (hover: Hover) => void;
  remove: (hover: Hover) => void;
  rotate: () => void;
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
      const check = await api.verify(result.spec, layout, get().onePlacement);
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

  // Right-click: put the current item into the empty cell next to the face.
  function placeInSpec(spec: Spec, place: Vec3 | null) {
    const { placing, newInputKind, newOutputKind, brush, updateSpec } = get();
    if (!place || !inside(spec.size, place) || occupantAt(spec, place) !== null) return;
    if (placing === 'region') {
      const port = activeRegionPort(spec);
      if (port && !regionContains(spec, port, place)) updateSpec((s) => toggleRegionCell(s, port, place));
      return;
    }
    if (placing === 'input') updateSpec((s) => addPort(s, true, place, newInputKind, 'east'));
    else if (placing === 'output') updateSpec((s) => addPort(s, false, place, newOutputKind, 'east'));
    else if (placing === 'keep') updateSpec((s) => setFixed(s, place, brush));
    else updateSpec((s) => toggleForbidden(s, place));
  }

  // Left-click: remove whatever occupies the clicked cell.
  // The selected free port, or the first one if the selection is stale.
  function activeRegionPort(spec: Spec): PortRef | null {
    const selected = get().regionPort;
    if (selected) {
      const port = portAt(spec, selected);
      if (port && isFree(port)) return selected;
    }
    return freePorts(spec)[0] ?? null;
  }

  function removeInSpec(spec: Spec, hit: Vec3 | null) {
    const { updateSpec, placing } = get();
    if (hit && placing === 'region') {
      const port = activeRegionPort(spec);
      if (port && regionContains(spec, port, hit)) {
        updateSpec((s) => toggleRegionCell(s, port, hit));
        return;
      }
    }
    const occupant = hit ? occupantAt(spec, hit) : null;
    if (!hit || !occupant) return;
    if (occupant.kind === 'port') updateSpec((s) => removePort(s, occupant.port));
    else if (occupant.kind === 'fixed') updateSpec((s) => setFixed(s, hit, null));
    else if (occupant.kind === 'hint') updateSpec((s) => removeHint(s, hit));
    else updateSpec((s) => toggleForbidden(s, hit));
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
    onePlacement: false,
    sketchTime: DEFAULT_SKETCH_TIME,
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
    placing: 'keep',
    newInputKind: 'comparator',
    newOutputKind: 'dust',
    brush: { type: 'solid' },
    regionPort: null,
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
    setOnePlacement: (one) => set({ onePlacement: one }),
    setSketchTime: (seconds) => set({ sketchTime: seconds }),

    startSolve: async () => {
      const { spec, timeLimit, firstOnly, onePlacement, sketchTime } = get();
      if (!spec) return;
      try {
        await api.startSolve(spec, { timeLimit, first: firstOnly, onePlacement, sketchTime });
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
    setView: (view) => set({ view }),
    setPlacing: (placing) => set({ placing }),
    setNewInputKind: (kind) => set({ newInputKind: kind }),
    setNewOutputKind: (kind) => set({ newOutputKind: kind }),
    setBrush: (block) => set({ brush: block }),
    drawRegion: (port) => set({ regionPort: port, placing: 'region', view: 'spec' }),
    setPortFree: (port, free) => {
      get().updateSpec((s) => withPortFree(s, port, free));
      if (free) get().drawRegion(port);
    },
    setHover: (hover) => set({ hover }),
    place: (hover) => {
      const { view, spec, result, brush } = get();
      if (view === 'result' && result) {
        const p = hover.place;
        if (p && inside(result.layout.size, p) && !blockAt(result.layout, p)) void editResult(p, brush);
      } else if (spec) {
        placeInSpec(spec, hover.place);
      }
    },

    remove: (hover) => {
      const { view, spec, result } = get();
      if (view === 'result' && result) {
        const hit = hover.hit;
        const block = hit ? blockAt(result.layout, hit) : undefined;
        if (!hit || !block) return;
        // Inputs cannot be removed from a solved circuit; clicking switches them.
        if (block.type === 'input') void toggleInput(block.channel ?? 0);
        else if (block.type !== 'output') void editResult(hit, null);
      } else if (spec) {
        removeInSpec(spec, hover.hit);
      }
    },

    // R: rotate the port or block under the cursor, else the brush.
    rotate: () => {
      const { view, hover, result, spec, updateSpec } = get();
      const hit = hover?.hit;
      if (view === 'result' && result && hit) {
        const block = blockAt(result.layout, hit);
        if (block && block.type !== 'input' && block.type !== 'output') {
          void editResult(hit, rotateBlock(block));
          return;
        }
      }
      if (view === 'spec' && spec && hit) {
        const occupant = occupantAt(spec, hit);
        if (occupant?.kind === 'port') {
          updateSpec((s) => rotatePort(s, occupant.port));
          return;
        }
        const kept = occupant?.kind === 'fixed' ? spec.fixed[occupant.index] : undefined;
        if (kept) {
          updateSpec((s) => setFixed(s, hit, rotateBlock(kept)));
          return;
        }
      }
      set({ brush: rotateBlock(get().brush) });
    },
  };
});
