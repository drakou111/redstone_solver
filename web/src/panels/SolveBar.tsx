import { useState } from 'react';

import { api, type ComposeModule } from '../api';
import { keepSketch } from '../specEdit';
import { DEFAULT_SKETCH_TIME, useStore } from '../store';

type Popover = 'log' | 'export' | 'options' | 'sketch' | null;

// Settings most solves never touch; the button shows a dot when any differs
// from its default so a changed setting is never forgotten.
function OptionsPopover({ onClose }: { onClose: () => void }) {
  const onePlacement = useStore((s) => s.onePlacement);
  const sketchTime = useStore((s) => s.sketchTime);
  const solving = useStore((s) => s.solving);
  const method = useStore((s) => s.method);
  const composeModule = useStore((s) => s.composeModule);
  const { setOnePlacement, setSketchTime, setComposeModule } = useStore.getState();
  return (
    <div className="popover options">
      <div className="popover-head">
        <span>Solver options</span>
        <button className="ghost" onClick={onClose}>
          Close
        </button>
      </div>
      <label className="check option-row">
        <input
          type="checkbox"
          checked={onePlacement}
          disabled={solving}
          onChange={(e) => setOnePlacement(e.target.checked)}
        />
        <span>
          Only as drawn
          <div className="muted small">
            Check the circuit facing this way, here only. Allows faster circuits, but they may fail when built rotated
            or elsewhere.
          </div>
        </span>
      </label>
      <label className="field option-row">
        <span>
          Sketch step time
          <div className="muted small">How long the solver tries to keep a sketch before freeing it.</div>
        </span>
        <input
          type="number"
          className="number"
          style={{ width: 64 }}
          min={1}
          value={sketchTime}
          disabled={solving}
          onChange={(e) => {
            const seconds = Number(e.target.value);
            if (Number.isFinite(seconds) && seconds > 0) setSketchTime(seconds);
          }}
        />
        <span className="muted">s</span>
      </label>
      {method === 'compose' && (
        <label className="field option-row">
          <span>
            Compose module
            <div className="muted small">The half adder both halves are built from.</div>
          </span>
          <select
            value={composeModule}
            disabled={solving}
            onChange={(e) => setComposeModule(e.target.value as ComposeModule)}
          >
            <option value="flat">Flat 5x2x4 (smallest)</option>
            <option value="tall">3 layers 5x3x3</option>
          </select>
        </label>
      )}
    </div>
  );
}

// Compose builds a full adder; it needs three inputs and two outputs.
function composeProblem(spec: { inputs: unknown[]; outputs: unknown[] } | null): string | null {
  if (!spec) return 'No spec loaded';
  if (spec.inputs.length !== 3 || spec.outputs.length !== 2)
    return 'Compose builds a full adder: it needs 3 inputs and 2 outputs (OUT1 sum, OUT2 carry)';
  return null;
}

// The sketch (hint blocks): check it as drawn, keep it, or clear it.
function SketchPopover({ onClose }: { onClose: () => void }) {
  const spec = useStore((s) => s.spec);
  const onePlacement = useStore((s) => s.onePlacement);
  const { updateSpec } = useStore.getState();
  const [check, setCheck] = useState<{ passes: boolean; report: string } | 'checking' | string | null>(null);
  if (!spec) return null;
  const runCheck = async () => {
    setCheck('checking');
    try {
      setCheck(await api.checkSketch(spec, onePlacement));
    } catch (error) {
      setCheck(error instanceof Error ? error.message : String(error));
    }
  };
  return (
    <div className="popover sketch">
      <div className="popover-head">
        <span>Sketch: {spec.hints.length} blocks the solver tries first</span>
        <button className="ghost" onClick={onClose}>
          Close
        </button>
      </div>
      <div className="button-row">
        <button onClick={() => void runCheck()} title="Simulate the sketch as drawn (other cells empty)">
          Check
        </button>
        <button
          onClick={() => updateSpec(keepSketch)}
          title="Turn every sketch block into a kept block the solver may not change"
        >
          Keep all
        </button>
        <button className="ghost" onClick={() => updateSpec((s) => ({ ...s, hints: [] }))}>
          Clear
        </button>
      </div>
      {check === 'checking' && <div className="muted">Checking...</div>}
      {typeof check === 'string' && check !== 'checking' && <div className="bad">{check}</div>}
      {check && typeof check === 'object' && (
        <>
          <div className={check.passes ? 'good' : 'bad'}>{check.passes ? 'The sketch works.' : 'The sketch fails:'}</div>
          <pre className="report">{check.report}</pre>
        </>
      )}
    </div>
  );
}

export function SolveBar() {
  const solving = useStore((s) => s.solving);
  const jobKind = useStore((s) => s.jobKind);
  const method = useStore((s) => s.method);
  const spec = useStore((s) => s.spec);
  const seconds = useStore((s) => s.solveSeconds);
  const log = useStore((s) => s.log);
  const solveError = useStore((s) => s.solveError);
  const result = useStore((s) => s.result);
  const timeLimit = useStore((s) => s.timeLimit);
  const firstOnly = useStore((s) => s.firstOnly);
  const onePlacement = useStore((s) => s.onePlacement);
  const sketchTime = useStore((s) => s.sketchTime);
  const sketchSize = useStore((s) => s.spec?.hints.length ?? 0);
  const specError = useStore((s) => s.specError);
  const { setTimeLimit, setFirstOnly, setMethod, startSolve, startShrink, startCompose, cancelSolve } =
    useStore.getState();
  const composeBlocked = composeProblem(spec);
  const running = { solve: 'Solving', shrink: 'Shrinking', compose: 'Composing' }[jobKind];
  const [open, setOpen] = useState<Popover>(null);
  const [exportText, setExportText] = useState<string>('');
  const toggle = (popover: Popover) => setOpen(open === popover ? null : popover);
  const optionsChanged = onePlacement || sketchTime !== DEFAULT_SKETCH_TIME;

  let status: React.ReactNode = <span className="muted">Set up the spec on the left, then solve.</span>;
  if (solving)
    status = (
      <span className="running">
        {running}... {seconds.toFixed(0)}s {log.length > 0 && `- ${log[log.length - 1]}`}
      </span>
    );
  else if (solveError) status = <span className="bad">{solveError}</span>;
  else if (result?.edited)
    status = <span className="muted">Edited by hand: {result.blockCount} blocks (see the check result in the view)</span>;
  else if (result?.shrunkFrom && result.shrunkFrom.join() === result.layout.size.join())
    status = (
      <span className="muted">
        No smaller box found: no column or row of {result.layout.size.join('x')} could go within the time limit
      </span>
    );
  else if (result?.shrunkFrom)
    status = (
      <span className="good">
        Shrunk {result.shrunkFrom.join('x')} to {result.layout.size.join('x')}: {result.blockCount} blocks in{' '}
        {result.seconds.toFixed(0)}s
      </span>
    );
  else if (result && jobKind === 'compose')
    status = (
      <span className="good">
        Full adder: {result.blockCount} blocks, {result.layout.size.join('x')}
      </span>
    );
  else if (result)
    status = (
      <span className="good">
        {result.optimal ? 'Optimal' : 'Verified'}: {result.blockCount} blocks in {result.seconds.toFixed(1)}s
        {!result.optimal && <span className="muted"> (not proven smallest)</span>}
      </span>
    );

  return (
    <div className="solvebar">
      <label className="field">
        <span>Time limit</span>
        <input
          type="number"
          className="number"
          style={{ width: 72 }}
          min={1}
          value={timeLimit}
          disabled={solving}
          onChange={(e) => {
            const seconds = Number(e.target.value);
            if (Number.isFinite(seconds) && seconds > 0) setTimeLimit(seconds);
          }}
        />
        <span className="muted">s</span>
      </label>
      {method === 'solve' && (
        <label className="check" title="Stop at the first circuit that passes the simulator, then remove unneeded blocks">
          <input
            type="checkbox"
            checked={firstOnly}
            disabled={solving}
            onChange={(e) => setFirstOnly(e.target.checked)}
          />
          first circuit only
        </label>
      )}
      <select
        className="method"
        value={method}
        disabled={solving}
        onChange={(e) => setMethod(e.target.value as 'solve' | 'compose')}
        title="Solve searches the whole box; Compose builds a full adder from two half adders (much faster, places the ports itself)"
      >
        <option value="solve">Solve</option>
        <option value="compose">Compose full adder</option>
      </select>
      {solving ? (
        <button className="danger" onClick={() => void cancelSolve()}>
          Cancel
        </button>
      ) : method === 'compose' ? (
        <button
          className="primary"
          disabled={!!specError || !!composeBlocked}
          title={composeBlocked ?? 'Time limit applies to each of its three steps'}
          onClick={() => void startCompose()}
        >
          Compose
        </button>
      ) : (
        <button className="primary" disabled={!!specError} onClick={() => void startSolve()}>
          Solve
        </button>
      )}
      <button
        className={`ghost${optionsChanged ? ' changed' : ''}`}
        onClick={() => toggle('options')}
        title={optionsChanged ? 'Some options differ from their defaults' : 'Solver options'}
      >
        Options
      </button>
      {sketchSize > 0 && (
        <button className="ghost" onClick={() => toggle('sketch')} title="Check, keep or clear the sketch">
          Sketch ({sketchSize})
        </button>
      )}
      <div className="status">{status}</div>
      <button className="ghost" onClick={() => toggle('log')}>
        Log
      </button>
      {result && !solving && !result.edited && (
        <button
          className="ghost"
          onClick={() => void startShrink()}
          title="Remove a column or row at a time while the circuit still works (uses the time limit)"
        >
          Shrink
        </button>
      )}
      {result && (
        <button
          className="ghost"
          onClick={() =>
            void api.exportCommands(result.layout).then((r) => {
              setExportText(r.text);
              setOpen('export');
            })
          }
        >
          Export
        </button>
      )}
      {open === 'options' && <OptionsPopover onClose={() => setOpen(null)} />}
      {open === 'sketch' && sketchSize > 0 && <SketchPopover onClose={() => setOpen(null)} />}
      {open === 'log' && (
        <div className="popover log">
          {log.length === 0 ? <div className="muted">No log yet.</div> : log.map((line, i) => <div key={i}>{line}</div>)}
        </div>
      )}
      {open === 'export' && (
        <div className="popover export">
          <div className="popover-head">
            <span>/setblock commands (relative to the lowest corner)</span>
            <button onClick={() => void navigator.clipboard.writeText(exportText)}>Copy</button>
            <button className="ghost" onClick={() => setOpen(null)}>
              Close
            </button>
          </div>
          <textarea readOnly value={exportText} />
        </div>
      )}
    </div>
  );
}
