#include "gui/views.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "redstone/report.hpp"

namespace rs::gui {

namespace {

constexpr float kMinPitch = 0.05f;
constexpr float kMaxPitch = 1.55f;
constexpr float kRotateSpeed = 0.01f;
constexpr float kZoomStep = 1.1f;
constexpr float kFitMargin = 1.25f;
constexpr ImU32 kBackground = IM_COL32(28, 30, 34, 255);
constexpr ImU32 kGridColor = IM_COL32(70, 74, 82, 255);
constexpr ImU32 kLabelColor = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr float kDustHeight = 0.02f;
constexpr float kFlatDustHeight = 0.06f;
constexpr float kDiodeHeight = 2.0f / 16.0f;

// Face order used everywhere: top, bottom, north, south, west, east.
enum FaceIndex { kTop, kBottom, kNorth, kSouth, kWest, kEast, kFaceCount };

struct V3 {
  float x, y, z;
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 to_v3(Vec3 v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y),
          static_cast<float>(v.z)};
}
V3 dir_v3(Dir d) { return to_v3(offset(d)); }

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

ImU32 shade(ImU32 color, float factor) {
  auto channel = [&](int shift) {
    return static_cast<int>(((color >> shift) & 0xFF) * factor);
  };
  return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT),
                  channel(IM_COL32_B_SHIFT),
                  (color >> IM_COL32_A_SHIFT) & 0xFF);
}

// Minecraft's dust tint: dark red when off, bright red at full power.
ImU32 dust_color(int power) {
  float f = power / static_cast<float>(kMaxSignal);
  int red = static_cast<int>(255 * (power == 0 ? 0.3f : f * 0.6f + 0.4f));
  int green = static_cast<int>(std::max(0.0f, f * f * 0.7f - 0.5f) * 255);
  return rgb(red, green, 0);
}

// Clockwise quarter turns that bring "image up = north" to `d`.
int turns_towards(Dir d) {
  switch (d) {
  case Dir::East:
    return 1;
  case Dir::South:
    return 2;
  case Dir::West:
    return 3;
  default:
    return 0;
  }
}

// Flat colour of a block when no texture is available.
ImU32 block_color(const Block &b, CellState s, int comparator_output) {
  switch (b.type) {
  case BlockType::Air:
    return rgb(0, 0, 0, 0);
  case BlockType::Solid:
    return rgb(22, 137, 145);
  case BlockType::Glass:
    return rgb(165, 225, 240, 70);
  case BlockType::Dust:
    return dust_color(s.power);
  case BlockType::Torch:
    return s.on ? rgb(255, 120, 40) : rgb(90, 35, 30);
  case BlockType::RedstoneBlock:
    return rgb(175, 24, 5);
  case BlockType::Repeater:
    return s.on ? rgb(200, 150, 150) : rgb(160, 160, 160);
  case BlockType::Comparator:
    return comparator_output > 0 ? rgb(215, 170, 170) : rgb(185, 185, 185);
  case BlockType::Target:
    return rgb(225, 215, 195);
  case BlockType::NoteBlock:
    return s.on ? rgb(140, 90, 60) : rgb(100, 65, 45);
  case BlockType::Hopper:
    return rgb(65, 65, 72);
  case BlockType::Barrel:
    return rgb(135, 98, 52);
  case BlockType::Chest:
    return rgb(165, 115, 40);
  case BlockType::Observer:
    return s.on ? rgb(150, 110, 110) : rgb(110, 110, 115);
  case BlockType::Input:
    return rgb(60, 190, 80);
  case BlockType::Output:
    return is_dust(b) ? dust_color(s.power) : rgb(70, 120, 230);
  }
  return rgb(255, 0, 255);
}

// Textures for the six faces of a box; 0 means flat colour.
struct Skin {
  std::array<ImTextureID, kFaceCount> faces{};
  int top_turns = 0; // clockwise quarter turns of the top texture
  ImU32 tint = kWhite;

  bool any() const {
    for (ImTextureID id : this->faces)
      if (id != ImTextureID{})
        return true;
    return false;
  }
};

Skin uniform_skin(ImTextureID texture) {
  Skin skin;
  skin.faces.fill(texture);
  return skin;
}

struct Face {
  std::array<ImVec2, 4> points;
  std::array<ImVec2, 4> uv;
  ImTextureID texture{};
  float group_depth; // depth of the owning block's centre
  float depth;
  ImU32 color;
  bool outline = true;
  int first_line = 0;
  int line_count = 0;
};

struct Line {
  ImVec2 a, b;
  ImU32 color;
};

struct Label {
  ImVec2 at;
  std::string text;
};

// Rotates block-local (u, v) on a top face by clockwise quarter turns.
ImVec2 rotate_uv(ImVec2 uv, int turns) {
  for (int i = 0; i < ((turns % 4) + 4) % 4; ++i)
    uv = {uv.y, 1.0f - uv.x};
  return uv;
}

class Scene {
public:
  Scene(ImVec2 origin, float scale, V3 center, float yaw, float pitch)
      : origin_(origin), scale_(scale), center_(center),
        cos_yaw_(std::cos(yaw)), sin_yaw_(std::sin(yaw)),
        cos_pitch_(std::cos(pitch)), sin_pitch_(std::sin(pitch)) {}

  ImVec2 project(V3 p) const {
    V3 v = p - center_;
    float x1 = v.x * cos_yaw_ - v.z * sin_yaw_;
    float z1 = v.x * sin_yaw_ + v.z * cos_yaw_;
    float screen_y = -(v.y * cos_pitch_ + z1 * sin_pitch_);
    // Minecraft axes are left-handed on screen: flip x so east is on the
    // right when looking north.
    return {origin_.x - x1 * scale_, origin_.y + screen_y * scale_};
  }

  // Larger is farther from the viewer.
  float depth(V3 p) const {
    V3 v = p - center_;
    float z1 = v.x * sin_yaw_ + v.z * cos_yaw_;
    return -v.y * sin_pitch_ + z1 * cos_pitch_;
  }

  // Adds an axis-aligned box inside the block at `block`; returns the index
  // of its top face.
  int box(V3 block, V3 lo, V3 hi, ImU32 color, const Skin *skin = nullptr) {
    std::array<V3, 8> c{V3{lo.x, lo.y, lo.z}, V3{hi.x, lo.y, lo.z},
                        V3{hi.x, lo.y, hi.z}, V3{lo.x, lo.y, hi.z},
                        V3{lo.x, hi.y, lo.z}, V3{hi.x, hi.y, lo.z},
                        V3{hi.x, hi.y, hi.z}, V3{lo.x, hi.y, hi.z}};
    struct Side {
      std::array<int, 4> corners;
      float light;
    };
    constexpr std::array<Side, kFaceCount> kSides{{
        {{4, 5, 6, 7}, 1.0f},  // top
        {{3, 2, 1, 0}, 0.5f},  // bottom
        {{4, 5, 1, 0}, 0.8f},  // north
        {{7, 6, 2, 3}, 0.8f},  // south
        {{4, 7, 3, 0}, 0.65f}, // west
        {{5, 6, 2, 1}, 0.65f}, // east
    }};
    int top = static_cast<int>(faces_.size());
    for (int f = 0; f < kFaceCount; ++f) {
      const Side &side = kSides[f];
      Face face{};
      float depth = 0;
      for (int k = 0; k < 4; ++k) {
        V3 corner = c[side.corners[k]];
        face.points[k] = project(corner);
        depth += this->depth(corner);
        V3 local = corner - block;
        if (f == kTop || f == kBottom)
          face.uv[k] =
              rotate_uv({local.x, local.z}, skin ? skin->top_turns : 0);
        else if (f == kNorth || f == kSouth)
          face.uv[k] = {local.x, 1.0f - local.y};
        else
          face.uv[k] = {local.z, 1.0f - local.y};
      }
      face.depth = depth / 4;
      face.group_depth = group_depth_;
      if (skin && skin->faces[f] != ImTextureID{}) {
        face.texture = skin->faces[f];
        face.color = shade(skin->tint, side.light);
        face.outline = false;
      } else {
        face.color = shade(color, side.light);
      }
      faces_.push_back(face);
    }
    return top;
  }

  // A textured flat quad (torch planes, dust strips). Corners in order.
  void quad(V3 block, std::array<V3, 4> corners, ImTextureID texture,
            ImU32 color, int top_turns, bool vertical) {
    Face face{};
    float depth = 0;
    for (int k = 0; k < 4; ++k) {
      face.points[k] = project(corners[k]);
      depth += this->depth(corners[k]);
      V3 local = corners[k] - block;
      if (vertical) {
        float u = std::abs(corners[k].x - corners[0].x) +
                  std::abs(corners[k].z - corners[0].z);
        face.uv[k] = {u, 1.0f - local.y};
      } else {
        face.uv[k] = rotate_uv({local.x, local.z}, top_turns);
      }
    }
    face.depth = depth / 4 - 0.01f; // in front of the face it lies on
    face.group_depth = group_depth_;
    face.texture = texture;
    face.color = color;
    face.outline = false;
    faces_.push_back(face);
  }

  void arrow(int face, V3 from, V3 to, ImU32 color) {
    Face &f = faces_[face];
    if (f.line_count == 0)
      f.first_line = static_cast<int>(lines_.size());
    V3 dir = to - from;
    V3 side{-dir.z * 0.35f, 0, dir.x * 0.35f};
    V3 head = to - dir * 0.4f;
    lines_.push_back({project(from), project(to), color});
    lines_.push_back({project(to), project(head + side), color});
    lines_.push_back({project(to), project(head + side * -1.0f), color});
    f.line_count += 3;
  }

  // Faces added after this belong to the block whose centre is `center`.
  // Whole blocks are drawn back to front, which is exact for grid cells.
  void begin_block(V3 center) { group_depth_ = this->depth(center); }

  void label(V3 at, std::string text) {
    labels_.push_back({project(at), std::move(text)});
  }

  void draw(ImDrawList *list, float thickness) {
    std::vector<int> order(faces_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
      order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      const Face &fa = faces_[a];
      const Face &fb = faces_[b];
      if (fa.group_depth != fb.group_depth)
        return fa.group_depth > fb.group_depth;
      return fa.depth > fb.depth;
    });
    for (int i : order) {
      const Face &f = faces_[i];
      if (f.texture != ImTextureID{}) {
        list->AddImageQuad(f.texture, f.points[0], f.points[1], f.points[2],
                           f.points[3], f.uv[0], f.uv[1], f.uv[2], f.uv[3],
                           f.color);
      } else {
        list->AddQuadFilled(f.points[0], f.points[1], f.points[2], f.points[3],
                            f.color);
      }
      if (f.outline)
        list->AddQuad(f.points[0], f.points[1], f.points[2], f.points[3],
                      shade(f.color, 0.6f), 1.0f);
      for (int k = 0; k < f.line_count; ++k) {
        const Line &line = lines_[f.first_line + k];
        list->AddLine(line.a, line.b, line.color, thickness);
      }
    }
    for (const Label &l : labels_) {
      ImVec2 size = ImGui::CalcTextSize(l.text.c_str());
      ImVec2 at{l.at.x - size.x / 2, l.at.y - size.y / 2};
      list->AddRectFilled({at.x - 2, at.y - 1},
                          {at.x + size.x + 2, at.y + size.y + 1},
                          IM_COL32(0, 0, 0, 160), 3.0f);
      list->AddText(at, kLabelColor, l.text.c_str());
    }
  }

private:
  ImVec2 origin_;
  float scale_;
  V3 center_;
  float cos_yaw_, sin_yaw_, cos_pitch_, sin_pitch_;
  float group_depth_ = 0;
  std::vector<Face> faces_;
  std::vector<Line> lines_;
  std::vector<Label> labels_;
};

// Textures of a full block, or nullopt when it has no textured look.
std::optional<Skin> block_skin(const Block &b, CellState s,
                               int comparator_output, const TextureLibrary &t) {
  Skin skin;
  switch (b.type) {
  case BlockType::Solid:
    return uniform_skin(t.get("cyan_wool"));
  case BlockType::Glass:
    return uniform_skin(t.get("cyan_stained_glass"));
  case BlockType::RedstoneBlock:
    return uniform_skin(t.get("redstone_block"));
  case BlockType::NoteBlock:
    return uniform_skin(t.get("note_block"));
  case BlockType::Chest:
    return uniform_skin(t.get("oak_planks"));
  case BlockType::Target:
    skin = uniform_skin(t.get("target_side"));
    skin.faces[kTop] = skin.faces[kBottom] = t.get("target_top");
    return skin;
  case BlockType::Hopper:
    skin = uniform_skin(t.get("hopper_outside"));
    skin.faces[kTop] = t.get("hopper_top");
    return skin;
  case BlockType::Barrel:
    skin = uniform_skin(t.get("barrel_side"));
    skin.faces[kTop] = t.get("barrel_top");
    skin.faces[kBottom] = t.get("barrel_bottom");
    return skin;
  case BlockType::Observer: {
    skin = uniform_skin(t.get("observer_side"));
    constexpr std::array<Dir, kFaceCount> kFaceDirs{
        Dir::Up, Dir::Down, Dir::North, Dir::South, Dir::West, Dir::East};
    for (int f = 0; f < kFaceCount; ++f) {
      if (kFaceDirs[f] == b.facing)
        skin.faces[f] = t.get("observer_front");
      else if (kFaceDirs[f] == opposite(b.facing))
        skin.faces[f] = t.get(s.on ? "observer_back_on" : "observer_back");
      else if (f == kTop || f == kBottom)
        skin.faces[f] = t.get("observer_top");
    }
    // The arrow on top points towards the output.
    if (is_horizontal(b.facing))
      skin.top_turns = turns_towards(opposite(b.facing));
    return skin;
  }
  case BlockType::Input:
    return uniform_skin(
        t.get(is_container_input(b) ? "barrel_side" : "lime_concrete"));
  default:
    return std::nullopt;
  }
  (void)comparator_output;
}

void add_block(Scene &scene, const World &world, const Simulator *sim,
               const TextureLibrary *textures, Vec3 p) {
  const Block &b = world.at(p);
  CellState s = sim ? sim->state_at(p) : CellState{};
  int comparator_output = sim ? sim->comparator_output_at(p) : 0;
  ImU32 color = block_color(b, s, comparator_output);
  V3 o = to_v3(p);
  V3 mid = o + V3{0.5f, 0.5f, 0.5f};
  ImU32 arrow_color = IM_COL32(40, 40, 40, 255);
  auto tex = [&](const char *name) {
    return textures ? textures->get(name) : ImTextureID{};
  };

  if (is_dust(b)) {
    ImTextureID dot = tex("redstone_dust_dot");
    ImTextureID line = tex("redstone_dust_line0");
    bool textured = dot != ImTextureID{} && line != ImTextureID{};
    float y = o.y + (textured ? kDustHeight : 0.0f);
    if (textured)
      scene.quad(o,
                 {o + V3{0, kDustHeight, 0}, o + V3{1, kDustHeight, 0},
                  o + V3{1, kDustHeight, 1}, o + V3{0, kDustHeight, 1}},
                 dot, color, 0, false);
    else
      scene.box(o, o + V3{0.38f, 0, 0.38f},
                o + V3{0.62f, kFlatDustHeight, 0.62f}, color);
    for (Dir d : kHorizontalDirs) {
      if (sim && !sim->dust_points(p, d))
        continue;
      // Half of the block from the centre towards `d`.
      float x0 = d == Dir::East ? 0.5f : 0.0f;
      float x1 = d == Dir::West ? 0.5f : 1.0f;
      float z0 = d == Dir::South ? 0.5f : 0.0f;
      float z1 = d == Dir::North ? 0.5f : 1.0f;
      bool along_z = d == Dir::North || d == Dir::South;
      if (textured) {
        scene.quad(o,
                   {V3{o.x + x0, y, o.z + z0}, V3{o.x + x1, y, o.z + z0},
                    V3{o.x + x1, y, o.z + z1}, V3{o.x + x0, y, o.z + z1}},
                   line, color, along_z ? 0 : 1, false);
      } else {
        V3 a = mid + dir_v3(d) * 0.12f;
        V3 e = mid + dir_v3(d) * 0.5f;
        float wx = along_z ? 0.08f : 0.0f;
        float wz = along_z ? 0.0f : 0.08f;
        scene.box(o, {std::min(a.x, e.x) - wx, o.y, std::min(a.z, e.z) - wz},
                  {std::max(a.x, e.x) + wx, o.y + kFlatDustHeight,
                   std::max(a.z, e.z) + wz},
                  color);
      }
    }
    if (b.type == BlockType::Output)
      scene.label(mid + V3{0, -0.3f, 0}, std::format("OUT{}", b.channel + 1));
    if (b.type == BlockType::Input)
      scene.label(mid + V3{0, -0.3f, 0}, std::format("IN{}", b.channel + 1));
    return;
  }

  if (b.type == BlockType::Torch) {
    ImTextureID torch = tex(s.on ? "redstone_torch" : "redstone_torch_off");
    V3 base = mid + V3{0, -0.5f, 0};
    V3 lean{0, 0, 0};
    if (b.facing != Dir::Down) {
      base = base + dir_v3(b.facing) * 0.5f + V3{0, 0.2f, 0};
      lean = dir_v3(b.facing) * -0.25f;
    }
    if (torch != ImTextureID{}) {
      // Two crossed planes, like Minecraft's torch model.
      for (bool along_x : {true, false}) {
        V3 half = along_x ? V3{0.5f, 0, 0} : V3{0, 0, 0.5f};
        V3 up{0, 1, 0};
        scene.quad(base - half,
                   {base - half + up + lean, base + half + up + lean,
                    base + half, base - half},
                   torch, kWhite, 0, true);
      }
    } else {
      scene.box(o, base + V3{-0.09f, 0, -0.09f}, base + V3{0.09f, 0.5f, 0.09f},
                rgb(120, 85, 50));
      scene.box(o, base + V3{-0.13f, 0.5f, -0.13f},
                base + V3{0.13f, 0.72f, 0.13f}, color);
    }
    return;
  }

  if (b.type == BlockType::Repeater || b.type == BlockType::Comparator ||
      is_diode_port(b)) {
    bool repeater = b.type == BlockType::Repeater ||
                    is_input(b, InputKind::Repeater) ||
                    is_output(b, OutputKind::Repeater);
    bool powered = repeater ? s.on : comparator_output > 0;
    if (sim && b.type == BlockType::Input)
      powered = sim->input(b.channel) > 0;
    if (sim && b.type == BlockType::Output)
      powered = sim->output(b.channel) > 0;
    Skin skin = uniform_skin(tex("smooth_stone"));
    skin.faces[kTop] =
        tex(repeater ? (powered ? "repeater_on" : "repeater")
                     : (powered ? "comparator_on" : "comparator"));
    skin.top_turns = turns_towards(b.facing);
    bool textured = skin.faces[kTop] != ImTextureID{};
    int top = scene.box(o, o, o + V3{1, kDiodeHeight, 1}, color,
                        textured ? &skin : nullptr);
    V3 surface = mid + V3{0, kDiodeHeight - 0.5f, 0};
    if (!textured)
      scene.arrow(top, surface + dir_v3(b.facing) * -0.35f,
                  surface + dir_v3(b.facing) * 0.35f, arrow_color);
    if (b.type == BlockType::Input)
      scene.label(surface, std::format("IN{}", b.channel + 1));
    else if (b.type == BlockType::Output)
      scene.label(surface, std::format("OUT{}", b.channel + 1));
    else
      scene.label(surface,
                  repeater ? std::to_string(b.repeater_delay())
                           : (b.comparator_mode() == ComparatorMode::Subtract
                                  ? std::string("-")
                                  : std::string("=")));
    return;
  }

  if (b.type == BlockType::Output) {
    Skin skin = uniform_skin(tex("light_blue_concrete"));
    scene.box(o, o + V3{0.2f, 0, 0.2f}, o + V3{0.8f, 0.35f, 0.8f}, color,
              skin.any() ? &skin : nullptr);
    scene.label(mid, std::format("OUT{}", b.channel + 1));
    return;
  }

  std::optional<Skin> skin;
  if (textures)
    skin = block_skin(b, s, comparator_output, *textures);
  bool textured = skin && skin->any();
  V3 hi = o + V3{1, b.type == BlockType::Chest ? 0.88f : 1.0f, 1};
  int top = scene.box(o, o, hi, color, textured ? &*skin : nullptr);

  if (b.type == BlockType::Observer && !textured) {
    V3 surface = mid + V3{0, 0.5f, 0};
    if (is_horizontal(b.facing))
      scene.arrow(top, surface + dir_v3(b.facing) * -0.35f,
                  surface + dir_v3(b.facing) * 0.35f,
                  s.on ? IM_COL32(230, 40, 40, 255) : arrow_color);
  }
  if (b.type == BlockType::Input) {
    V3 surface = mid + V3{0, 0.5f, 0};
    if (is_diode_input(b) && is_horizontal(b.facing))
      scene.arrow(top, surface, surface + dir_v3(b.facing) * 0.45f,
                  arrow_color);
    scene.label(surface, std::format("IN{}", b.channel + 1));
  }
}

} // namespace

void draw_world_3d(const char *id, const World &world, const Simulator *sim,
                   const TextureLibrary *textures, Camera &camera,
                   int top_layer, ImVec2 size) {
  if (textures && !textures->loaded())
    textures = nullptr;
  ImVec2 start = ImGui::GetCursorScreenPos();
  size.x = std::max(size.x, 100.0f);
  size.y = std::max(size.y, 100.0f);
  ImGui::InvisibleButton(id, size);
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    ImVec2 delta = ImGui::GetIO().MouseDelta;
    camera.yaw += delta.x * kRotateSpeed;
    camera.pitch =
        std::clamp(camera.pitch + delta.y * kRotateSpeed, kMinPitch, kMaxPitch);
  }
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0)
    camera.zoom *= std::pow(kZoomStep, ImGui::GetIO().MouseWheel);

  ImDrawList *list = ImGui::GetWindowDrawList();
  ImVec2 end{start.x + size.x, start.y + size.y};
  list->PushClipRect(start, end, true);
  list->AddRectFilled(start, end, kBackground);

  Vec3 dims = world.size();
  float diagonal = std::sqrt(
      static_cast<float>(dims.x * dims.x + dims.y * dims.y + dims.z * dims.z));
  float scale =
      std::min(size.x, size.y) / (diagonal * kFitMargin) * camera.zoom;
  V3 center{dims.x / 2.0f, dims.y / 2.0f, dims.z / 2.0f};
  Scene scene({start.x + size.x / 2, start.y + size.y / 2}, scale, center,
              camera.yaw, camera.pitch);

  // Footprint grid and compass on the ground plane.
  for (int x = 0; x <= dims.x; ++x)
    list->AddLine(
        scene.project({static_cast<float>(x), 0, 0}),
        scene.project({static_cast<float>(x), 0, static_cast<float>(dims.z)}),
        kGridColor);
  for (int z = 0; z <= dims.z; ++z)
    list->AddLine(
        scene.project({0, 0, static_cast<float>(z)}),
        scene.project({static_cast<float>(dims.x), 0, static_cast<float>(z)}),
        kGridColor);
  list->AddText(scene.project({center.x, 0, -0.7f}), kLabelColor, "N");
  list->AddText(scene.project({dims.x + 0.5f, 0, center.z}), kLabelColor, "E");

  for (int i = 0; i < world.volume(); ++i) {
    Vec3 p = world.position_of(i);
    if (p.y > top_layer || world.at(p).type == BlockType::Air)
      continue;
    scene.begin_block(to_v3(p) + V3{0.5f, 0.5f, 0.5f});
    add_block(scene, world, sim, textures, p);
  }
  scene.draw(list, std::max(1.5f, scale * 0.05f));
  list->PopClipRect();
}

void draw_layer(const char *id, const World &world, const Simulator *sim,
                const TextureLibrary *textures, int y) {
  if (textures && !textures->loaded())
    textures = nullptr;
  Vec3 dims = world.size();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float cell = std::clamp(std::min(avail.x / dims.x, (avail.y - 4) / dims.z),
                          22.0f, 64.0f);
  ImVec2 start = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, {cell * dims.x, cell * dims.z});
  ImDrawList *list = ImGui::GetWindowDrawList();

  for (int z = 0; z < dims.z; ++z) {
    for (int x = 0; x < dims.x; ++x) {
      Vec3 p{x, y, z};
      const Block &b = world.at(p);
      CellState s = sim ? sim->state_at(p) : CellState{};
      int comparator_output = sim ? sim->comparator_output_at(p) : 0;
      ImVec2 lo{start.x + x * cell, start.y + z * cell};
      ImVec2 hi{lo.x + cell - 1, lo.y + cell - 1};
      ImU32 fill = b.type == BlockType::Air
                       ? kBackground
                       : block_color(b, s, comparator_output);
      list->AddRectFilled(lo, hi, fill);
      if (textures && b.type != BlockType::Air) {
        std::optional<Skin> skin =
            block_skin(b, s, comparator_output, *textures);
        if (skin && skin->faces[kTop] != ImTextureID{}) {
          std::array<ImVec2, 4> uv;
          for (int k = 0; k < 4; ++k)
            uv[k] = rotate_uv(
                {k == 1 || k == 2 ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f},
                skin->top_turns);
          list->AddImageQuad(skin->faces[kTop], lo, {hi.x, lo.y}, hi,
                             {lo.x, hi.y}, uv[0], uv[1], uv[2], uv[3]);
        }
      }
      list->AddRect(lo, hi, kGridColor);
      if (b.type == BlockType::Air)
        continue;

      std::string text = block_code(b);
      if (is_dust(b) && sim)
        text += std::format("\n{}", s.power);
      ImVec2 size = ImGui::CalcTextSize(text.c_str());
      ImVec2 at{lo.x + (cell - size.x) / 2, lo.y + (cell - size.y) / 2};
      list->AddRectFilled({at.x - 2, at.y - 1},
                          {at.x + size.x + 2, at.y + size.y + 1},
                          IM_COL32(0, 0, 0, 140), 3.0f);
      list->AddText(at, kLabelColor, text.c_str());

      if (ImGui::IsMouseHoveringRect(lo, hi)) {
        std::string details =
            std::format("({}, {}, {}) {}", x, y, z, to_string(b));
        if (sim) {
          if (is_dust(b))
            details += std::format("\npower {}", s.power);
          else if (b.type == BlockType::Comparator)
            details += std::format("\noutput {}", comparator_output);
          else if (b.type != BlockType::Solid && b.type != BlockType::Glass)
            details += s.on ? "\non" : "\noff";
          if (s.locked)
            details += " (locked)";
        }
        ImGui::SetTooltip("%s", details.c_str());
      }
    }
  }
}

} // namespace rs::gui
