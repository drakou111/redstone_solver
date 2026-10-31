import { useState } from 'react';

import { api } from '../api';
import { useStore } from '../store';

const MAX_SIGNAL = 15;

// Simulator controls for the solved circuit: input strengths, stepping and
// live outputs.
export function SimBar() {
  const sim = useStore((s) => s.sim);
  const result = useStore((s) => s.result);
  const view = useStore((s) => s.view);
  const error = useStore((s) => s.simError);
  const setSim = useStore((s) => s.setSim);
  const setError = useStore((s) => s.setSimError);
  const [settle, setSettle] = useState(true);
  if (view !== 'result' || !result) return null;
  if (!sim)
    return (
      <div className="simbar">
        <span className="sim-title">Simulator</span>
        <span className="bad">{error ?? 'Loading...'}</span>
      </div>
    );

  const run = (request: Promise<typeof sim>) =>
    request.then(setSim).catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)));

  return (
    <div className="simbar">
      <span className="sim-title">Simulator</span>
      {sim.inputs.map((value, channel) => (
        <label className="field input-slider" key={channel}>
          <span className="port-name in">IN{channel + 1}</span>
          <input
            type="range"
            min={0}
            max={MAX_SIGNAL}
            value={value}
            onChange={(e) => void run(api.setInput(channel, Number(e.target.value), settle))}
          />
          <span className="value">{value}</span>
        </label>
      ))}
      {sim.outputs.map((value, channel) => (
        <span className={`output-value ${value > 0 ? 'lit' : ''}`} key={channel}>
          OUT{channel + 1} = {value}
        </span>
      ))}
      <span className="muted">
        t = {sim.time}
        {sim.pending ? ' (busy)' : ''}
      </span>
      <button onClick={() => void run(api.tick(1))}>Tick</button>
      <button onClick={() => void run(api.tick(10))}>x10</button>
      <button onClick={() => void run(api.settle())}>Settle</button>
      <button onClick={() => void run(api.resetSim(result.layout, sim.inputs))}>Reset</button>
      <label className="check">
        <input type="checkbox" checked={settle} onChange={(e) => setSettle(e.target.checked)} />
        settle on change
      </label>
      {error && <span className="bad">{error}</span>}
    </div>
  );
}
