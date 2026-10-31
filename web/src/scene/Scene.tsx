import { Html, OrbitControls } from '@react-three/drei';
import { Canvas, type ThreeEvent } from '@react-three/fiber';
import { useMemo } from 'react';
import * as THREE from 'three';

import type { Hover } from '../store';
import type { CellState, SimState, Vec3, World } from '../types';
import { key } from '../types';
import { BlockView, type TextureMap } from './BlockView';

export type GhostStyle = 'place' | 'remove' | 'move' | null;

interface Props {
  world: World;
  sim: SimState | null;
  textures: TextureMap;
  topLayer: number;
  forbidden: Vec3[];
  kept: Vec3[];
  selected: Vec3 | null;
  hover: Hover | null;
  ghost: GhostStyle;
  onHover: (hover: Hover | null) => void;
  onClick: (hover: Hover) => void;
}

const BACKGROUND = '#16181d';
const GRID_COLOR = '#3a3f4b';
const BOUNDS_COLOR = '#5b6475';
const GHOST_COLORS: Record<Exclude<GhostStyle, null>, string> = {
  place: '#63d07e',
  remove: '#ff5a4d',
  move: '#f0c060',
};
// Pointer travel (px) above which a press counts as a camera drag, not a click.
const CLICK_TOLERANCE = 5;

function VolumeOutline({ size }: { size: Vec3 }) {
  const [sx, sy, sz] = size;
  const edges = useMemo(() => new THREE.EdgesGeometry(new THREE.BoxGeometry(sx, sy, sz)), [sx, sy, sz]);
  const grid = useMemo(() => {
    const points: number[] = [];
    for (let x = 0; x <= sx; ++x) points.push(x, 0, 0, x, 0, sz);
    for (let z = 0; z <= sz; ++z) points.push(0, 0, z, sx, 0, z);
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(points, 3));
    return geometry;
  }, [sx, sz]);
  return (
    <group>
      <lineSegments geometry={edges} position={[sx / 2, sy / 2, sz / 2]}>
        <lineBasicMaterial color={BOUNDS_COLOR} transparent opacity={0.5} />
      </lineSegments>
      <lineSegments geometry={grid}>
        <lineBasicMaterial color={GRID_COLOR} />
      </lineSegments>
      <Html position={[sx / 2, 0, -0.8]} center className="compass">
        N
      </Html>
      <Html position={[sx + 0.8, 0, sz / 2]} center className="compass">
        E
      </Html>
    </group>
  );
}

function CellBox({ cell, color, opacity = 1, solid = false, scale = 1.02 }: { cell: Vec3; color: string; opacity?: number; solid?: boolean; scale?: number }) {
  const center: Vec3 = [cell[0] + 0.5, cell[1] + 0.5, cell[2] + 0.5];
  return solid ? (
    <mesh position={center} scale={scale} raycast={() => null}>
      <boxGeometry />
      <meshBasicMaterial color={color} transparent opacity={opacity} depthWrite={false} />
    </mesh>
  ) : (
    <lineSegments position={center} scale={scale} raycast={() => null}>
      <edgesGeometry args={[new THREE.BoxGeometry(1, 1, 1)]} />
      <lineBasicMaterial color={color} transparent opacity={opacity} />
    </lineSegments>
  );
}

// The empty cell next to the face that was hit (the face normal is turned
// towards the camera, since every face is double sided).
function hoverFromBlock(event: ThreeEvent<PointerEvent | MouseEvent>, cell: Vec3): Hover {
  const normal = event.face?.normal.clone();
  if (!normal) return { hit: cell, place: null };
  if (normal.dot(event.ray.direction) > 0) normal.negate();
  return { hit: cell, place: [cell[0] + Math.round(normal.x), cell[1] + Math.round(normal.y), cell[2] + Math.round(normal.z)] };
}

export function Scene({ world, sim, textures, topLayer, forbidden, kept, selected, hover, ghost, onHover, onClick }: Props) {
  const [sx, sy, sz] = world.size;
  const top = Math.min(topLayer, sy - 1);
  // Clicking empty space places on the floor, or on the top shown layer when
  // upper layers are hidden.
  const workLayer = top < sy - 1 ? top : 0;
  const center: Vec3 = [sx / 2, sy / 3, sz / 2];
  const distance = Math.max(sx, sy, sz) * 1.6 + 3;
  const cells = useMemo(() => {
    const map = new Map<string, CellState>();
    for (const cell of sim?.cells ?? []) map.set(key(cell.position), cell);
    return map;
  }, [sim]);

  const move = (cell: Vec3) => (event: ThreeEvent<PointerEvent>) => {
    event.stopPropagation();
    onHover(hoverFromBlock(event, cell));
  };
  const click = (cell: Vec3) => (event: ThreeEvent<MouseEvent>) => {
    event.stopPropagation();
    if (event.delta <= CLICK_TOLERANCE) onClick(hoverFromBlock(event, cell));
  };
  const planeHover = (event: ThreeEvent<PointerEvent | MouseEvent>): Hover => ({
    hit: null,
    place: [Math.floor(event.point.x), workLayer, Math.floor(event.point.z)],
  });

  const ghostCell = ghost === 'remove' ? hover?.hit : hover?.place;

  return (
    <div className="canvas-wrap" onPointerLeave={() => onHover(null)}>
      <Canvas
        camera={{ position: [center[0] + distance * 0.6, center[1] + distance * 0.7, center[2] + distance], fov: 40 }}
        gl={{ antialias: true }}
      >
        <color attach="background" args={[BACKGROUND]} />
        <ambientLight intensity={1.6} />
        <directionalLight position={[4, 10, 6]} intensity={1.6} />
        <directionalLight position={[-6, 4, -3]} intensity={0.5} />
        <OrbitControls target={center} makeDefault enableDamping dampingFactor={0.15} />
        <VolumeOutline size={world.size} />

        <mesh
          position={[sx / 2, workLayer, sz / 2]}
          rotation={[-Math.PI / 2, 0, 0]}
          onPointerMove={(e) => {
            e.stopPropagation();
            onHover(planeHover(e));
          }}
          onClick={(e) => {
            e.stopPropagation();
            if (e.delta <= CLICK_TOLERANCE) onClick(planeHover(e));
          }}
        >
          <planeGeometry args={[sx, sz]} />
          <meshBasicMaterial transparent opacity={workLayer > 0 ? 0.06 : 0} color="#ffffff" depthWrite={false} side={THREE.DoubleSide} />
        </mesh>

        {world.blocks
          .filter((b) => b.position[1] <= top)
          .map((b) => {
            const portValue =
              b.type === 'input' ? sim?.inputs[b.channel ?? 0] : b.type === 'output' ? sim?.outputs[b.channel ?? 0] : undefined;
            return (
              <BlockView
                key={`${key(b.position)}:${b.type}`}
                block={b}
                position={b.position}
                state={cells.get(key(b.position))}
                portValue={portValue}
                textures={textures}
                onPointerMove={move(b.position)}
                onClick={click(b.position)}
              />
            );
          })}

        {forbidden
          .filter((c) => c[1] <= top)
          .map((c) => (
            <group key={key(c)}>
              <mesh position={[c[0] + 0.5, c[1] + 0.5, c[2] + 0.5]} scale={0.9} onPointerMove={move(c)} onClick={click(c)}>
                <boxGeometry />
                <meshBasicMaterial color="#ff5a4d" transparent opacity={0.18} depthWrite={false} />
              </mesh>
              <CellBox cell={c} color="#ff5a4d" opacity={0.7} scale={0.9} />
            </group>
          ))}
        {kept
          .filter((c) => c[1] <= top)
          .map((c) => (
            <CellBox key={key(c)} cell={c} color="#7aa7ff" opacity={0.8} />
          ))}
        {selected && <CellBox cell={selected} color="#f0c060" scale={1.08} />}
        {ghost && ghostCell && ghostCell.every((v, i) => v >= 0 && v < (world.size[i] ?? 0)) && (
          <>
            <CellBox cell={ghostCell} color={GHOST_COLORS[ghost]} />
            <CellBox cell={ghostCell} color={GHOST_COLORS[ghost]} solid opacity={0.15} scale={1} />
          </>
        )}
      </Canvas>
    </div>
  );
}
