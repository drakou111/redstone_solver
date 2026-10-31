import { useState } from 'react';

import { formatRange, parseRange } from '../rangeText';
import { useStore } from '../store';
import type { Dir, InputPort, OutputPort, SignalRange, Spec } from '../types';
import { HORIZONTAL, INPUT_KINDS, OUTPUT_KINDS } from '../types';

const TICKS_PER_REDSTONE_TICK = 2;
const MAX_SIDE = 16;
const MAX_PORTS = 8;
const AXES = [0, 1, 2] as const;
const AXIS_NAMES = ['x', 'y', 'z'] as const;

function Section({ title, children, open = true }: { title: string; children: React.ReactNode; open?: boolean }) {
  return (
    <details className="section" open={open}>
      <summary>{title}</summary>
      <div className="section-body">{children}</div>
    </details>
  );
}

function NumberField({ value, onChange, min, max, width = 52 }: { value: number; onChange: (v: number) => void; min: number; max: number; width?: number }) {
  return (
    <input
      type="number"
      className="number"
      style={{ width }}
      value={value}
      min={min}
      max={max}
      onChange={(e) => {
        const v = Number(e.target.value);
        if (Number.isFinite(v)) onChange(Math.min(max, Math.max(min, Math.round(v))));
      }}
    />
  );
}

// A truth-table cell; keeps the typed text until it parses.
function RangeCell({ value, onChange }: { value: SignalRange; onChange: (r: SignalRange) => void }) {
  const [text, setText] = useState<string | null>(null);
  const invalid = text !== null && parseRange(text) === null;
  return (
    <input
      className={`cell ${invalid ? 'invalid' : ''}`}
      value={text ?? formatRange(value)}
      onChange={(e) => {
        setText(e.target.value);
        const parsed = parseRange(e.target.value);
        if (parsed) onChange(parsed);
      }}
      onBlur={() => setText(null)}
    />
  );
}

function isDirectional(port: InputPort | OutputPort, input: boolean): boolean {
  return input ? port.kind === 'comparator' || port.kind === 'repeater' : port.kind !== 'dust';
}

function PortList({ spec, input }: { spec: Spec; input: boolean }) {
  const updateSpec = useStore((s) => s.updateSpec);
  const ports: (InputPort | OutputPort)[] = input ? spec.inputs : spec.outputs;
  const prefix = input ? 'IN' : 'OUT';
  const kinds: readonly string[] = input ? INPUT_KINDS : OUTPUT_KINDS;

  const setPorts = (next: (InputPort | OutputPort)[]) =>
    updateSpec((s) => (input ? { ...s, inputs: next as InputPort[] } : { ...s, outputs: next as OutputPort[] }));
  const update = (i: number, change: Partial<InputPort | OutputPort>) =>
    setPorts(ports.map((p, j) => (j === i ? ({ ...p, ...change } as InputPort | OutputPort) : p)));
  const remove = (i: number) =>
    updateSpec((s) => {
      const rows = s.rows.map((r) =>
        input ? { ...r, inputs: r.inputs.filter((_, j) => j !== i) } : { ...r, outputs: r.outputs.filter((_, j) => j !== i) },
      );
      return input
        ? { ...s, inputs: s.inputs.filter((_, j) => j !== i), rows }
        : { ...s, outputs: s.outputs.filter((_, j) => j !== i), rows };
    });
  const add = () =>
    updateSpec((s) => {
      const rows = s.rows.map((r) =>
        input ? { ...r, inputs: [...r.inputs, [0, 15] as SignalRange] } : { ...r, outputs: [...r.outputs, [0, 15] as SignalRange] },
      );
      const position: [number, number, number] = input ? [0, 1, 0] : [s.size[0] - 1, 1, 0];
      return input
        ? { ...s, rows, inputs: [...s.inputs, { kind: 'comparator', position, facing: 'east' }] }
        : { ...s, rows, outputs: [...s.outputs, { kind: 'dust', position, facing: 'east' }] };
    });

  return (
    <div className="ports">
      {ports.map((port, i) => (
        <div className="port" key={i}>
          <span className={`port-name ${input ? 'in' : 'out'}`}>
            {prefix}
            {i + 1}
          </span>
          <select value={port.kind} onChange={(e) => update(i, { kind: e.target.value as InputPort['kind'] })}>
            {kinds.map((k) => (
              <option key={k}>{k}</option>
            ))}
          </select>
          {AXES.map((axis) => (
            <NumberField
              key={axis}
              value={port.position[axis]}
              min={0}
              max={spec.size[axis] - 1}
              width={44}
              onChange={(v) => {
                const position = [...port.position] as [number, number, number];
                position[axis] = v;
                update(i, { position });
              }}
            />
          ))}
          {isDirectional(port, input) ? (
            <select value={port.facing} onChange={(e) => update(i, { facing: e.target.value as Dir })}>
              {HORIZONTAL.map((d) => (
                <option key={d}>{d}</option>
              ))}
            </select>
          ) : (
            <span className="facing-placeholder" />
          )}
          <button className="icon" title="Remove" onClick={() => remove(i)}>
            x
          </button>
        </div>
      ))}
      {ports.length < MAX_PORTS && (
        <button className="link" onClick={add}>
          + {input ? 'input' : 'output'}
        </button>
      )}
    </div>
  );
}

function TruthTable({ spec }: { spec: Spec }) {
  const updateSpec = useStore((s) => s.updateSpec);
  const setCell = (row: number, input: boolean, column: number, range: SignalRange) =>
    updateSpec((s) => ({
      ...s,
      rows: s.rows.map((r, i) => {
        if (i !== row) return r;
        const cells = [...(input ? r.inputs : r.outputs)];
        cells[column] = range;
        return input ? { ...r, inputs: cells } : { ...r, outputs: cells };
      }),
    }));

  return (
    <div>
      <table className="truth">
        <thead>
          <tr>
            {spec.inputs.map((_, i) => (
              <th key={`i${i}`} className="in">
                IN{i + 1}
              </th>
            ))}
            {spec.outputs.map((_, i) => (
              <th key={`o${i}`} className="out">
                OUT{i + 1}
              </th>
            ))}
            <th />
          </tr>
        </thead>
        <tbody>
          {spec.rows.map((row, r) => (
            <tr key={r}>
              {row.inputs.map((range, c) => (
                <td key={`i${c}`}>
                  <RangeCell value={range} onChange={(v) => setCell(r, true, c, v)} />
                </td>
              ))}
              {row.outputs.map((range, c) => (
                <td key={`o${c}`} className="out-cell">
                  <RangeCell value={range} onChange={(v) => setCell(r, false, c, v)} />
                </td>
              ))}
              <td>
                <button className="icon" title="Remove row" onClick={() => updateSpec((s) => ({ ...s, rows: s.rows.filter((_, i) => i !== r) }))}>
                  x
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="row-actions">
        <button
          className="link"
          onClick={() =>
            updateSpec((s) => ({
              ...s,
              rows: [...s.rows, { inputs: s.inputs.map(() => [0, 15] as SignalRange), outputs: s.outputs.map(() => [0, 15] as SignalRange) }],
            }))
          }
        >
          + row
        </button>
        <span className="hint">* = anything, 0, 5, 1..15</span>
      </div>
    </div>
  );
}

function Timing({ spec }: { spec: Spec }) {
  const updateSpec = useStore((s) => s.updateSpec);
  const [redstoneTicks, setRedstoneTicks] = useState(false);
  const scale = redstoneTicks ? TICKS_PER_REDSTONE_TICK : 1;
  const t = spec.timing;
  const mode = t.earliest !== null ? 'window' : t.latest !== null ? 'deadline' : 'none';
  const setTiming = (change: Partial<Spec['timing']>) => updateSpec((s) => ({ ...s, timing: { ...s.timing, ...change } }));
  const shown = (gt: number) => Math.round(gt / scale);
  const ticks = (label: string, gt: number, apply: (gt: number) => void) => (
    <label className="field">
      <span>{label}</span>
      <NumberField value={shown(gt)} min={0} max={200} onChange={(v) => apply(v * scale)} />
    </label>
  );

  return (
    <div className="timing">
      <div className="segmented">
        {(['none', 'deadline', 'window'] as const).map((m) => (
          <button
            key={m}
            className={mode === m ? 'active' : ''}
            onClick={() =>
              setTiming(
                m === 'none'
                  ? { earliest: null, latest: null, holdPrevious: false }
                  : m === 'deadline'
                    ? { earliest: null, latest: t.latest ?? 4, holdPrevious: false }
                    : { earliest: t.earliest ?? 0, latest: t.latest ?? 4 },
              )
            }
          >
            {m === 'none' ? 'No deadline' : m === 'deadline' ? 'Deadline' : 'Window'}
          </button>
        ))}
      </div>
      <div className="segmented small">
        <button className={!redstoneTicks ? 'active' : ''} onClick={() => setRedstoneTicks(false)}>
          game ticks
        </button>
        <button className={redstoneTicks ? 'active' : ''} onClick={() => setRedstoneTicks(true)}>
          redstone ticks
        </button>
      </div>
      {mode === 'window' && ticks('Earliest', t.earliest ?? 0, (v) => setTiming({ earliest: v }))}
      {mode !== 'none' && ticks('Correct from', t.latest ?? 0, (v) => setTiming({ latest: v }))}
      {mode === 'window' && (
        <label className="check">
          <input type="checkbox" checked={t.holdPrevious} onChange={(e) => setTiming({ holdPrevious: e.target.checked })} />
          Keep old outputs until "earliest"
        </label>
      )}
      {mode === 'none'
        ? ticks('Time to settle', t.horizon, (v) => setTiming({ horizon: Math.max(1, v) }))
        : ticks('Extra settle time', t.settle, (v) => setTiming({ settle: v }))}
    </div>
  );
}

function Blocks({ spec }: { spec: Spec }) {
  const session = useStore((s) => s.session);
  const updateSpec = useStore((s) => s.updateSpec);
  const all = session?.palette ?? spec.palette;
  const toggle = (name: string) =>
    updateSpec((s) => ({
      ...s,
      palette: s.palette.includes(name) ? s.palette.filter((p) => p !== name) : all.filter((p) => p === name || s.palette.includes(p)),
    }));
  return (
    <div>
      <div className="chips">
        {all.map((name) => (
          <button key={name} className={`chip ${spec.palette.includes(name) ? 'on' : ''}`} onClick={() => toggle(name)}>
            {name.replace('_', ' ')}
          </button>
        ))}
      </div>
      <div className="row-actions">
        <button className="link" onClick={() => updateSpec((s) => ({ ...s, palette: [...all] }))}>
          all
        </button>
        <button className="link" onClick={() => updateSpec((s) => ({ ...s, palette: [] }))}>
          none
        </button>
        <label className="check">
          <input type="checkbox" checked={spec.feedback} onChange={(e) => updateSpec((s) => ({ ...s, feedback: e.target.checked }))} />
          allow loops and repeater locks (memory)
        </label>
      </div>
    </div>
  );
}

function Volume({ spec }: { spec: Spec }) {
  const updateSpec = useStore((s) => s.updateSpec);
  return (
    <div className="volume">
      {AXES.map((i) => (
        <label className="field" key={i}>
          <span>{AXIS_NAMES[i]}</span>
          <NumberField
            value={spec.size[i]}
            min={1}
            max={MAX_SIDE}
            onChange={(v) =>
              updateSpec((s) => {
                const size = [...s.size] as Spec['size'];
                size[i] = v;
                return { ...s, size };
              })
            }
          />
          <label className="check small" title={`Copies can be placed side by side along ${AXIS_NAMES[i]}`}>
            <input
              type="checkbox"
              checked={spec.tiled[i]}
              onChange={(e) =>
                updateSpec((s) => {
                  const tiled = [...s.tiled] as Spec['tiled'];
                  tiled[i] = e.target.checked;
                  return { ...s, tiled };
                })
              }
            />
            tile
          </label>
        </label>
      ))}
    </div>
  );
}

function FileBar() {
  const session = useStore((s) => s.session);
  const specPath = useStore((s) => s.specPath);
  const loadSpec = useStore((s) => s.loadSpec);
  const saveSpec = useStore((s) => s.saveSpec);
  const notice = useStore((s) => s.notice);
  const [path, setPath] = useState<string | null>(null);
  const current = path ?? specPath;
  return (
    <div className="filebar">
      <select className="examples" value="" onChange={(e) => e.target.value && void loadSpec(e.target.value)}>
        <option value="">Open example...</option>
        {session?.examples.map((file) => (
          <option key={file} value={file}>
            {file.split(/[\\/]/).pop()}
          </option>
        ))}
      </select>
      <div className="path-row">
        <input className="path" value={current} onChange={(e) => setPath(e.target.value)} spellCheck={false} />
        <button onClick={() => void loadSpec(current).then(() => setPath(null))}>Open</button>
        <button onClick={() => void saveSpec(current).then(() => setPath(null))}>Save</button>
      </div>
      {notice && <div className="notice">{notice}</div>}
    </div>
  );
}

export function SpecPanel() {
  const spec = useStore((s) => s.spec);
  const specError = useStore((s) => s.specError);
  if (!spec) return <aside className="panel">Loading...</aside>;
  return (
    <aside className="panel">
      <FileBar />
      {specError && <div className="error">{specError}</div>}
      <Section title="Volume">
        <Volume spec={spec} />
      </Section>
      <Section title="Inputs">
        <PortList spec={spec} input />
      </Section>
      <Section title="Outputs">
        <PortList spec={spec} input={false} />
      </Section>
      <Section title="Truth table">
        <TruthTable spec={spec} />
      </Section>
      <Section title="Timing">
        <Timing spec={spec} />
      </Section>
      <Section title="Allowed blocks">
        <Blocks spec={spec} />
      </Section>
    </aside>
  );
}
