import { useState } from 'react';

import { api } from '../api';
import { useStore } from '../store';

export function SolveBar() {
  const solving = useStore((s) => s.solving);
  const seconds = useStore((s) => s.solveSeconds);
  const log = useStore((s) => s.log);
  const solveError = useStore((s) => s.solveError);
  const result = useStore((s) => s.result);
  const timeLimit = useStore((s) => s.timeLimit);
  const firstOnly = useStore((s) => s.firstOnly);
  const specError = useStore((s) => s.specError);
  const { setTimeLimit, setFirstOnly, startSolve, cancelSolve } = useStore.getState();
  const [showLog, setShowLog] = useState(false);
  const [exportText, setExportText] = useState<string | null>(null);

  let status: React.ReactNode = <span className="muted">Set up the spec on the left, then solve.</span>;
  if (solving) status = <span className="running">Solving... {seconds.toFixed(0)}s {log.length > 0 && `- ${log[log.length - 1]}`}</span>;
  else if (solveError) status = <span className="bad">{solveError}</span>;
  else if (result?.edited)
    status = <span className="muted">Edited by hand: {result.blockCount} blocks (see the check result in the view)</span>;
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
        <input type="range" min={5} max={600} step={5} value={timeLimit} disabled={solving} onChange={(e) => setTimeLimit(Number(e.target.value))} />
        <span className="value">{timeLimit}s</span>
      </label>
      <label className="check" title="Stop at the first circuit that passes the simulator, then remove unneeded blocks">
        <input type="checkbox" checked={firstOnly} disabled={solving} onChange={(e) => setFirstOnly(e.target.checked)} />
        first circuit only
      </label>
      {solving ? (
        <button className="danger" onClick={() => void cancelSolve()}>
          Cancel
        </button>
      ) : (
        <button className="primary" disabled={!!specError} onClick={() => void startSolve()}>
          Solve
        </button>
      )}
      <div className="status">{status}</div>
      <button className="ghost" onClick={() => setShowLog(!showLog)}>
        Log
      </button>
      {result && (
        <button className="ghost" onClick={() => void api.exportCommands(result.layout).then((r) => setExportText(r.text))}>
          Export
        </button>
      )}
      {showLog && (
        <div className="popover log">
          {log.length === 0 ? <div className="muted">No log yet.</div> : log.map((line, i) => <div key={i}>{line}</div>)}
        </div>
      )}
      {exportText !== null && (
        <div className="popover export">
          <div className="popover-head">
            <span>/setblock commands (relative to the lowest corner)</span>
            <button onClick={() => void navigator.clipboard.writeText(exportText)}>Copy</button>
            <button className="ghost" onClick={() => setExportText(null)}>
              Close
            </button>
          </div>
          <textarea readOnly value={exportText} />
        </div>
      )}
    </div>
  );
}
