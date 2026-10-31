import { useEffect, useState } from 'react';

import { SimBar } from './panels/SimBar';
import { SolveBar } from './panels/SolveBar';
import { SpecPanel } from './panels/SpecPanel';
import { Scene } from './scene/Scene';
import { useStore } from './store';

function ViewControls() {
  const world = useStore((s) => s.result?.layout ?? s.preview);
  const topLayer = useStore((s) => s.topLayer);
  const setTopLayer = useStore((s) => s.setTopLayer);
  const texturesJar = useStore((s) => s.texturesJar);
  const textureError = useStore((s) => s.textureError);
  const loadTextures = useStore((s) => s.loadTextures);
  const result = useStore((s) => s.result);
  const clearResult = useStore((s) => s.clearResult);
  const [jar, setJar] = useState<string | null>(null);
  if (!world) return null;
  const top = world.size[1] - 1;
  return (
    <div className="view-controls">
      <label className="field">
        <span>Layers 0-{Math.min(topLayer, top)}</span>
        <input type="range" min={0} max={top} value={Math.min(topLayer, top)} onChange={(e) => setTopLayer(Number(e.target.value))} />
      </label>
      {result && (
        <button className="ghost" onClick={clearResult}>
          Back to ports
        </button>
      )}
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

export function App() {
  const init = useStore((s) => s.init);
  const world = useStore((s) => s.result?.layout ?? s.preview);
  const sim = useStore((s) => (s.result ? s.sim : null));
  const textures = useStore((s) => s.textures);
  const topLayer = useStore((s) => s.topLayer);
  const [fatal, setFatal] = useState<string | null>(null);

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
          {world ? <Scene world={world} sim={sim} textures={textures} topLayer={topLayer} /> : <div className="muted center">Loading...</div>}
          <ViewControls />
        </div>
      </main>
    </div>
  );
}
