import type { BoxFace, BoxPart, Look, Part, QuadPart, UV, V3 } from './blockModel';

// Converts block parts into quads; box faces get Minecraft-style UVs (side
// textures upright, top textures with image-up = north).

const FACES: BoxFace[] = ['top', 'bottom', 'north', 'south', 'west', 'east'];

function boxFaceQuad(box: BoxPart, face: BoxFace, look: Look): QuadPart {
  const [x0, y0, z0] = box.min;
  const [x1, y1, z1] = box.max;
  let corners: [V3, V3, V3, V3];
  switch (face) {
    case 'top':
      corners = [[x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1]];
      break;
    case 'bottom':
      corners = [[x0, y0, z1], [x1, y0, z1], [x1, y0, z0], [x0, y0, z0]];
      break;
    case 'north':
      corners = [[x1, y0, z0], [x0, y0, z0], [x0, y1, z0], [x1, y1, z0]];
      break;
    case 'south':
      corners = [[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]];
      break;
    case 'west':
      corners = [[x0, y0, z0], [x0, y0, z1], [x0, y1, z1], [x0, y1, z0]];
      break;
    case 'east':
      corners = [[x1, y0, z1], [x1, y0, z0], [x1, y1, z0], [x1, y1, z1]];
      break;
  }
  const uvs = corners.map(([x, y, z]): UV => {
    if (face === 'top' || face === 'bottom') return [x, 1 - z];
    if (face === 'north' || face === 'south') return [face === 'north' ? 1 - x : x, y];
    return [face === 'east' ? 1 - z : z, y];
  }) as [UV, UV, UV, UV];
  return { kind: 'quad', corners, uvs, look };
}

export function partsToQuads(parts: Part[]): QuadPart[] {
  const quads: QuadPart[] = [];
  for (const part of parts) {
    if (part.kind === 'quad') {
      quads.push(part);
    } else if (part.kind === 'box') {
      for (const face of FACES) {
        const look = part.faces[face];
        if (look) quads.push(boxFaceQuad(part, face, look));
      }
    }
  }
  return quads;
}

export function lookKey(look: Look): string {
  return `${look.texture ?? '-'}|${look.color}|${look.transparent ? 't' : 'o'}`;
}

// Flat arrays for one BufferGeometry holding the given quads.
export function quadBuffers(quads: QuadPart[]): { positions: Float32Array; uvs: Float32Array; indices: number[] } {
  const positions = new Float32Array(quads.length * 12);
  const uvs = new Float32Array(quads.length * 8);
  const indices: number[] = [];
  quads.forEach((quad, q) => {
    quad.corners.forEach((corner, k) => {
      positions.set(corner, q * 12 + k * 3);
      uvs.set(quad.uvs[k] as UV, q * 8 + k * 2);
    });
    const base = q * 4;
    indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
  });
  return { positions, uvs, indices };
}
