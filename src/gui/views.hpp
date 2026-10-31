#pragma once

#include "gui/textures.hpp"
#include "imgui.h"
#include "redstone/simulator.hpp"
#include "redstone/world.hpp"

namespace rs::gui {

struct Camera {
  float yaw = 3.75f; // looking north-west from the south-east
  float pitch = 0.6f;
  float zoom = 1.0f;
};

// Orbit view of the world (drag to rotate, wheel to zoom). Layers above
// `top_layer` are hidden. `sim` (optional) supplies live power states and
// `textures` (optional) Minecraft block textures.
void draw_world_3d(const char *id, const World &world, const Simulator *sim,
                   const TextureLibrary *textures, Camera &camera,
                   int top_layer, ImVec2 size);

// Top-down grid of layer `y`, north at the top. Hovering shows details.
void draw_layer(const char *id, const World &world, const Simulator *sim,
                const TextureLibrary *textures, int y);

} // namespace rs::gui
