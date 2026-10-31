import { facingChoices } from '../specEdit';
import { RESULT_TOOLS, SPEC_TOOLS, type Tool, useStore } from '../store';
import type { Block, BlockType, Dir } from '../types';
import { INPUT_KINDS, OUTPUT_KINDS } from '../types';

export const TOOL_INFO: Record<Tool, { label: string; key: string; help: string }> = {
  select: { label: 'Select', key: 'V', help: 'Click a port to select it, then click a cell to move it. R rotates, Del removes.' },
  input: { label: 'Input', key: 'I', help: 'Click a cell to add an input. R rotates the selected one.' },
  output: { label: 'Output', key: 'O', help: 'Click a cell to add an output. R rotates the selected one.' },
  keep: { label: 'Keep block', key: 'K', help: 'Click to pre-place the block below; click a kept block to remove it. R rotates.' },
  empty: { label: 'Keep empty', key: 'E', help: 'Click a cell the solver must leave empty; click again to allow it.' },
  place: { label: 'Place', key: 'P', help: 'Click to add the block below to the circuit. R rotates the block under the cursor.' },
  remove: { label: 'Remove', key: 'X', help: 'Click a block to remove it from the circuit.' },
};

const RESULT_SELECT_HELP = 'Click an input to switch it on or off. Hover a block to inspect it.';
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
  const tool = useStore((s) => s.tool);
  const setTool = useStore((s) => s.setTool);
  const newInputKind = useStore((s) => s.newInputKind);
  const newOutputKind = useStore((s) => s.newOutputKind);
  const setNewInputKind = useStore((s) => s.setNewInputKind);
  const setNewOutputKind = useStore((s) => s.setNewOutputKind);
  const tools = view === 'result' ? RESULT_TOOLS : SPEC_TOOLS;
  const help = view === 'result' && tool === 'select' ? RESULT_SELECT_HELP : TOOL_INFO[tool].help;

  return (
    <div className="toolbar">
      <div className="tools">
        {tools.map((t) => (
          <button key={t} className={tool === t ? 'active' : ''} onClick={() => setTool(t)} title={`${TOOL_INFO[t].label} (${TOOL_INFO[t].key})`}>
            {TOOL_INFO[t].label}
            <kbd>{TOOL_INFO[t].key}</kbd>
          </button>
        ))}
      </div>
      {tool === 'input' && (
        <select value={newInputKind} onChange={(e) => setNewInputKind(e.target.value as (typeof INPUT_KINDS)[number])}>
          {INPUT_KINDS.map((k) => (
            <option key={k}>{k}</option>
          ))}
        </select>
      )}
      {tool === 'output' && (
        <select value={newOutputKind} onChange={(e) => setNewOutputKind(e.target.value as (typeof OUTPUT_KINDS)[number])}>
          {OUTPUT_KINDS.map((k) => (
            <option key={k}>{k}</option>
          ))}
        </select>
      )}
      {(tool === 'keep' || tool === 'place') && <BrushEditor />}
      <div className="tool-help">{help}</div>
    </div>
  );
}
