import { Html } from '@react-three/drei';
import type { ThreeEvent } from '@react-three/fiber';
import { useEffect, useMemo } from 'react';
import * as THREE from 'three';

import type { Block, CellState, Vec3 } from '../types';
import { blockParts, type Look, type QuadPart } from './blockModel';
import { lookKey, partsToQuads, quadBuffers } from './geometry';

export type TextureMap = ReadonlyMap<string, THREE.Texture>;

const materialCache = new Map<string, THREE.Material>();

function material(look: Look, textures: TextureMap): THREE.Material {
  const texture = look.texture ? textures.get(look.texture) : undefined;
  const cacheKey = `${lookKey(look)}|${texture ? texture.uuid : 'flat'}`;
  let cached = materialCache.get(cacheKey);
  if (!cached) {
    cached = new THREE.MeshLambertMaterial({
      map: texture ?? null,
      color: new THREE.Color(texture ? look.color : fallbackColor(look)),
      transparent: !!look.transparent,
      alphaTest: look.transparent ? 0.05 : 0,
      depthWrite: !look.transparent || !texture || look.texture === 'redstone_dust_dot' || look.texture === 'redstone_dust_line0',
      side: THREE.DoubleSide,
    });
    materialCache.set(cacheKey, cached);
  }
  return cached;
}

// Used when the Minecraft textures are not available.
function fallbackColor(look: Look): string {
  const flat: Record<string, string> = {
    cyan_wool: '#16898f',
    cyan_stained_glass: '#9fe0f0',
    smooth_stone: '#a0a0a0',
    redstone_block: '#af1805',
    stone: '#7a7a7a',
  };
  return (look.texture && flat[look.texture]) ?? look.color;
}

interface Props {
  block: Block;
  position: Vec3;
  state: CellState | undefined;
  portValue?: number;
  textures: TextureMap;
  onPointerDown?: (event: ThreeEvent<PointerEvent>) => void;
  onPointerMove?: (event: ThreeEvent<PointerEvent>) => void;
}

export function BlockView({ block, position, state, portValue, textures, onPointerDown, onPointerMove }: Props) {
  const parts = useMemo(() => blockParts(block, state, portValue), [block, state, portValue]);
  const groups = useMemo(() => {
    const byLook = new Map<string, { look: Look; quads: QuadPart[] }>();
    for (const quad of partsToQuads(parts)) {
      const k = lookKey(quad.look);
      const group = byLook.get(k) ?? { look: quad.look, quads: [] };
      group.quads.push(quad);
      byLook.set(k, group);
    }
    return [...byLook.values()].map(({ look, quads }) => {
      const { positions, uvs, indices } = quadBuffers(quads);
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
      geometry.setAttribute('uv', new THREE.BufferAttribute(uvs, 2));
      geometry.setIndex(indices);
      geometry.computeVertexNormals();
      return { look, geometry };
    });
  }, [parts]);
  useEffect(() => () => groups.forEach(({ geometry }) => geometry.dispose()), [groups]);

  return (
    <group position={position} onPointerDown={onPointerDown} onPointerMove={onPointerMove}>
      {groups.map(({ look, geometry }) => (
        <mesh key={lookKey(look)} geometry={geometry} material={material(look, textures)} renderOrder={look.transparent ? 1 : 0} />
      ))}
      {parts.map((part, i) =>
        part.kind === 'label' && part.text ? (
          <Html key={i} position={part.at} center className="block-label" zIndexRange={[10, 0]}>
            {part.text}
          </Html>
        ) : null,
      )}
    </group>
  );
}
