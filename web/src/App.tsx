import { useEffect, useMemo, useState } from 'react';

import { HoverInfo } from './panels/HoverInfo';
import { SimBar } from './panels/SimBar';
import { SolveBar } from './panels/SolveBar';
import { SpecPanel } from './panels/SpecPanel';
import { PLACING_INFO, Toolbar } from './panels/Toolbar';
import { Scene } from './scene/Scene';
import { freePorts, portAt, portName, specWorld } from './specEdit';
import { PLACING, useStore } from './store';

function ViewControls() {
  const spec = useStore((s) => s.spec);
  const topLayer = useStore((s) => s.topLayer);
  const setTopLayer = useStore((s) => s.setTopLayer);
  const texturesJar = useStore((s) => s.texturesJar);
  const textureError = useStore((s) => s.textureError);
  const loadTextures = useStore((s) => s.loadTextures);
  const result = useStore((s) => s.result);
  const view = useStore((s) => s.view);
  const setView = useStore((s) => s.setView);
  const [jar, setJar] = useState<string | null>(null);
  if (!spec) return null;
  const top = spec.size[1] - 1;
  return (
    <div className="view-controls">
      {result && (
        <div className="segmented">
          <button className={view === 'spec' ? 'active' : ''} onClick={() => setView('spec')}>
            Spec
          </button>
          <button className={view === 'result' ? 'active' : ''} onClick={() => setView('result')}>
            Circuit
          </button>
        </div>
      )}
      <label className="field">
        <span>Layers 0-{Math.min(topLayer, top)}</span>
        <input type="range" min={0} max={top} value={Math.min(topLayer, top)} onChange={(e) => setTopLayer(Number(e.target.value))} />
      </label>
      <details className="textures">
        <summary>{texturesJar ? 'Minecraft textures' : 'No textures'}</summary>
        <div className="popover-body">
          <p className="muted">Textures are read from your own Minecraft client jar, never copied.</p>
          <input className="path" value={jar ?? texturesJar ?? ''} placeholder="path to client .jar" onChange={(e) => setJar(e.target.value)} />
          <button onClick={() => void loadTextures(jar ?? texturesJar ?? undefined)}>Load</button>
          {textureError && <div className="bad">{textureError}</div>}
        </div>
      </details>
    </div>
  );
}

function EditBadge() {
  const result = useStore((s) => s.result);
  const verification = useStore((s) => s.verification);
  const view = useStore((s) => s.view);
  if (view !== 'result' || !result?.edited || !verification) return null;
  if (verification.checking) return <div className="edit-badge checking">Edited - checking in the simulator...</div>;
  return verification.ok ? (
    <div className="edit-badge ok">Edited - still meets the spec ({result.blockCount} blocks)</div>
  ) : (
    <div className="edit-badge fail">Edited - fails: {verification.reason}</div>
  );
}

function isTyping(target: EventTarget | null): boolean {
  return target instanceof HTMLInputElement || target instanceof HTMLSelectElement || target instanceof HTMLTextAreaElement;
}

function useShortcuts() {
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (isTyping(event.target) || event.ctrlKey || event.altKey || event.metaKey) return;
      const state = useStore.getState();
      const k = event.key.toLowerCase();
      if (k === 'r') {
        state.rotate();
      } else {
        const placing = PLACING.find((p) => PLACING_INFO[p].key.toLowerCase() === k);
        if (!placing || state.view !== 'spec') return;
        state.setPlacing(placing);
      }
      event.preventDefault();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
}

function Viewport() {
  const spec = useStore((s) => s.spec);
  const result = useStore((s) => s.result);
  const view = useStore((s) => s.view);
  const sim = useStore((s) => s.sim);
  const textures = useStore((s) => s.textures);
  const topLayer = useStore((s) => s.topLayer);
  const hover = useStore((s) => s.hover);
  const setHover = useStore((s) => s.setHover);
  const place = useStore((s) => s.place);
  const remove = useStore((s) => s.remove);
  const placing = useStore((s) => s.placing);
  const regionPort = useStore((s) => s.regionPort);
  const showResult = view === 'result' && result !== null;
  const world = useMemo(() => (showResult ? result.layout : spec ? specWorld(spec) : null), [showResult, result, spec]);
  if (!spec || !world) return <div className="muted center">Loading...</div>;
  const free = freePorts(spec);
  const active = free.find((p) => regionPort && p.input === regionPort.input && p.index === regionPort.index) ?? free[0];
  const regions = showResult
    ? []
    : free.map((p) => ({
        name: portName(p),
        cells: portAt(spec, p)?.region ?? [],
        input: p.input,
        active: placing === 'region' && p === active,
      }));

  return (
    <Scene
      world={world}
      sim={showResult ? sim : null}
      textures={textures}
      topLayer={topLayer}
      forbidden={showResult ? [] : spec.forbidden}
      kept={showResult ? [] : spec.fixed.map((b) => b.position)}
      sketch={showResult ? [] : spec.hints.map((b) => b.position)}
      regions={regions}
      hover={hover}
      onHover={setHover}
      onPlace={place}
      onRemove={remove}
    />
  );
}

export function App() {
  const init = useStore((s) => s.init);
  const [fatal, setFatal] = useState<string | null>(null);
  useShortcuts();

  useEffect(() => {
    init().catch((e: unknown) => setFatal(e instanceof Error ? e.message : String(e)));
  }, [init]);

  if (fatal) return <div className="fatal">Could not start: {fatal}</div>;
  return (
    <div className="app">
      <SpecPanel />
      <main className="main">
        <SolveBar />
        <SimBar />
        <div className="viewport">
          <Viewport />
          <Toolbar />
          <HoverInfo />
          <EditBadge />
          <ViewControls />
        </div>
      </main>
    </div>
  );
}
