import { facingChoices } from '../specEdit';
import { PLACING, type Placing, useStore } from '../store';
import type { Block, BlockType, Dir } from '../types';
import { INPUT_KINDS, OUTPUT_KINDS } from '../types';

export const PLACING_INFO: Record<Placing, { label: string; key: string }> = {
  input: { label: 'Input', key: 'I' },
  output: { label: 'Output', key: 'O' },
  keep: { label: 'Block', key: 'K' },
  empty: { label: 'Keep empty', key: 'E' },
};

const SPEC_HELP = 'Right-click: place  |  Left-click: remove  |  R: rotate  |  Drag: orbit, right-drag: pan, wheel: zoom';
const RESULT_HELP = 'Right-click: place block  |  Left-click: remove (or switch an input)  |  R: rotate';

const BRUSH_TYPES: readonly BlockType[] = [
  'solid',
  'glass',
  'dust',
  'torch',
  'redstone_block',
  'repeater',
  'comparator',
  'target',
  'note_block',
  'hopper',
  'barrel',
  'chest',
  'observer',
];

function defaults(type: BlockType): Block {
  switch (type) {
    case 'torch':
      return { type, facing: 'down' };
    case 'observer':
      return { type, facing: 'north' };
    case 'repeater':
      return { type, facing: 'east', delay: 1 };
    case 'comparator':
      return { type, facing: 'east', mode: 'compare' };
    case 'hopper':
    case 'barrel':
    case 'chest':
      return { type, signal: 0 };
    default:
      return { type };
  }
}

function BrushEditor() {
  const brush = useStore((s) => s.brush);
  const setBrush = useStore((s) => s.setBrush);
  const facings = facingChoices(brush);
  return (
    <div className="brush">
      <select value={brush.type} onChange={(e) => setBrush(defaults(e.target.value as BlockType))}>
        {BRUSH_TYPES.map((t) => (
          <option key={t} value={t}>
            {t.replace('_', ' ')}
          </option>
        ))}
      </select>
      {facings.length > 0 && (
        <select value={brush.facing} onChange={(e) => setBrush({ ...brush, facing: e.target.value as Dir })} title={brush.type === 'torch' ? 'side it hangs on' : 'facing'}>
          {facings.map((d) => (
            <option key={d}>{d}</option>
          ))}
        </select>
      )}
      {brush.type === 'repeater' && (
        <select value={brush.delay} onChange={(e) => setBrush({ ...brush, delay: Number(e.target.value) })}>
          {[1, 2, 3, 4].map((d) => (
            <option key={d} value={d}>
              delay {d}
            </option>
          ))}
        </select>
      )}
      {brush.type === 'comparator' && (
        <select value={brush.mode} onChange={(e) => setBrush({ ...brush, mode: e.target.value as 'compare' | 'subtract' })}>
          <option>compare</option>
          <option>subtract</option>
        </select>
      )}
      {brush.signal !== undefined && (
        <label className="field">
          <span>ss</span>
          <input type="number" className="number" min={0} max={15} value={brush.signal} style={{ width: 48 }} onChange={(e) => setBrush({ ...brush, signal: Math.max(0, Math.min(15, Number(e.target.value))) })} />
        </label>
      )}
    </div>
  );
}

export function Toolbar() {
  const view = useStore((s) => s.view);
  const placing = useStore((s) => s.placing);
  const setPlacing = useStore((s) => s.setPlacing);
  const newInputKind = useStore((s) => s.newInputKind);
  const newOutputKind = useStore((s) => s.newOutputKind);
  const setNewInputKind = useStore((s) => s.setNewInputKind);
  const setNewOutputKind = useStore((s) => s.setNewOutputKind);
  const inSpec = view === 'spec';

  return (
    <div className="toolbar">
      {inSpec && (
        <div className="tools">
          <span className="tools-label">Right-click places</span>
          {PLACING.map((p) => (
            <button key={p} className={placing === p ? 'active' : ''} onClick={() => setPlacing(p)} title={`${PLACING_INFO[p].label} (${PLACING_INFO[p].key})`}>
              {PLACING_INFO[p].label}
              <kbd>{PLACING_INFO[p].key}</kbd>
            </button>
          ))}
        </div>
      )}
      {inSpec && placing === 'input' && (
        <select value={newInputKind} onChange={(e) => setNewInputKind(e.target.value as (typeof INPUT_KINDS)[number])}>
          {INPUT_KINDS.map((k) => (
            <option key={k}>{k}</option>
          ))}
        </select>
      )}
      {inSpec && placing === 'output' && (
        <select value={newOutputKind} onChange={(e) => setNewOutputKind(e.target.value as (typeof OUTPUT_KINDS)[number])}>
          {OUTPUT_KINDS.map((k) => (
            <option key={k}>{k}</option>
          ))}
        </select>
      )}
      {(!inSpec || placing === 'keep') && <BrushEditor />}
      <div className="tool-help">{inSpec ? SPEC_HELP : RESULT_HELP}</div>
    </div>
  );
}
