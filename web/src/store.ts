import * as THREE from 'three';
import { create } from 'zustand';

import { api } from './api';
import type { SimState, Session, SolveResult, Spec, World } from './types';

const CHECK_DELAY_MS = 250;
const POLL_INTERVAL_MS = 400;
export const DEFAULT_TIME_LIMIT = 60;

export interface Result extends SolveResult {
  layout: World;
  spec: Spec;
}

interface State {
  session: Session | null;
  spec: Spec | null;
  specPath: string;
  preview: World | null;
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
  sim: SimState | null;
  topLayer: number;

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
  clearResult: () => void;
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
      const { world } = await api.checkSpec(spec);
      if (get().spec === spec) set({ preview: world, specError: null });
    } catch (error) {
      if (get().spec === spec) set({ specError: errorText(error) });
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
      if (!result?.layout) {
        set({ solveError: result ? `No circuit: ${result.status}` : 'The solver stopped without a result.' });
        return;
      }
      const spec = get().spec;
      if (!spec) return;
      set({ result: { ...result, layout: result.layout, spec }, topLayer: result.layout.size[1] - 1 });
      const sim = await api.resetSim(result.layout, spec.inputs.map(() => 0));
      set({ sim });
    } catch (error) {
      clearInterval(pollTimer);
      set({ solving: false, solveError: errorText(error) });
    }
  }

  return {
    session: null,
    spec: null,
    specPath: '',
    preview: null,
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
    sim: null,
    topLayer: 64,

    init: async () => {
      const session = await api.session();
      set({ session });
      await get().loadSpec(session.defaultPath);
      if (session.jar) void get().loadTextures(session.jar);
    },

    loadSpec: async (path) => {
      try {
        const { spec } = await api.loadSpec(path);
        set({ spec, specPath: path, notice: `Loaded ${path}`, result: null, sim: null, topLayer: spec.size[1] - 1 });
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
      set({ spec });
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
    setSim: (sim) => set({ sim }),
    clearResult: () => set({ result: null, sim: null }),
  };
});
