import { Html, OrbitControls } from '@react-three/drei';
import { Canvas } from '@react-three/fiber';
import { useMemo } from 'react';
import * as THREE from 'three';

import type { CellState, SimState, World } from '../types';
import { key } from '../types';
import { BlockView, type TextureMap } from './BlockView';

interface Props {
  world: World;
  sim: SimState | null;
  textures: TextureMap;
  topLayer: number;
}

const BACKGROUND = '#16181d';
const GRID_COLOR = '#3a3f4b';
const BOUNDS_COLOR = '#5b6475';

function VolumeOutline({ size }: { size: World['size'] }) {
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

export function Scene({ world, sim, textures, topLayer }: Props) {
  const [sx, sy, sz] = world.size;
  const center: [number, number, number] = [sx / 2, sy / 3, sz / 2];
  const distance = Math.max(sx, sy, sz) * 1.6 + 3;
  const cells = useMemo(() => {
    const map = new Map<string, CellState>();
    for (const cell of sim?.cells ?? []) map.set(key(cell.position), cell);
    return map;
  }, [sim]);

  return (
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
      {world.blocks
        .filter((b) => b.position[1] <= topLayer)
        .map((b) => {
          const portValue =
            b.type === 'input'
              ? sim?.inputs[b.channel ?? 0]
              : b.type === 'output'
                ? sim?.outputs[b.channel ?? 0]
                : undefined;
          return (
            <BlockView
              key={`${key(b.position)}:${b.type}`}
              block={b}
              position={b.position}
              state={cells.get(key(b.position))}
              portValue={portValue}
              textures={textures}
            />
          );
        })}
    </Canvas>
  );
}
