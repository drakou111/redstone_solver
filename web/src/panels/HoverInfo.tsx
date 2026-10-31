import { blockAt, occupantAt } from '../specEdit';
import { useStore } from '../store';
import type { Block } from '../types';
import { key } from '../types';

function describe(block: Block): string {
  switch (block.type) {
    case 'input':
      return `IN${(block.channel ?? 0) + 1}: ${block.kind} input${block.kind === 'comparator' || block.kind === 'repeater' ? ` facing ${block.facing}` : ''}`;
    case 'output':
      return `OUT${(block.channel ?? 0) + 1}: ${block.kind} output${block.kind !== 'dust' ? ` facing ${block.facing}` : ''}`;
    case 'torch':
      return block.facing === 'down' ? 'torch (standing)' : `torch on the ${block.facing} block`;
    case 'repeater':
      return `repeater facing ${block.facing}, delay ${block.delay}`;
    case 'comparator':
      return `comparator facing ${block.facing}, ${block.mode}`;
    case 'observer':
      return `observer watching ${block.facing}`;
    case 'hopper':
    case 'barrel':
    case 'chest':
      return `${block.type} (signal ${block.signal})`;
    default:
      return block.type.replace('_', ' ');
  }
}

// Details of the cell under the pointer.
export function HoverInfo() {
  const hover = useStore((s) => s.hover);
  const view = useStore((s) => s.view);
  const spec = useStore((s) => s.spec);
  const result = useStore((s) => s.result);
  const sim = useStore((s) => s.sim);
  const cell = hover?.hit ?? hover?.place;
  if (!cell || !spec) return null;

  const lines: string[] = [`(${cell.join(', ')})`];
  if (view === 'result' && result) {
    const block = blockAt(result.layout, cell);
    lines.push(block ? describe(block) : 'air');
    const state = sim?.cells.find((c) => key(c.position) === key(cell));
    if (block && state) {
      if (block.type === 'dust' || (block.type === 'output' && block.kind === 'dust')) lines.push(`power ${state.power}`);
      else if (block.type === 'comparator') lines.push(`output ${state.comparator}`);
      else if (['torch', 'repeater', 'observer', 'note_block', 'hopper'].includes(block.type))
        lines.push(state.on ? (block.type === 'torch' ? 'lit' : 'powered') : 'off');
      if (state.locked) lines.push('locked');
    }
  } else {
    const occupant = occupantAt(spec, cell);
    if (occupant?.kind === 'port') {
      const port = occupant.port.input ? spec.inputs[occupant.port.index] : spec.outputs[occupant.port.index];
      if (port)
        lines.push(
          describe({
            type: occupant.port.input ? 'input' : 'output',
            kind: port.kind,
            channel: occupant.port.index,
            facing: port.facing,
          }),
        );
    } else if (occupant?.kind === 'fixed') {
      const fixed = spec.fixed[occupant.index];
      if (fixed) lines.push(`kept: ${describe(fixed)}`);
    } else if (occupant?.kind === 'forbidden') {
      lines.push('kept empty');
    } else {
      lines.push('free for the solver');
    }
  }
  return (
    <div className="hover-info">
      {lines.map((line, i) => (
        <div key={i} className={i === 0 ? 'muted' : ''}>
          {line}
        </div>
      ))}
    </div>
  );
}
