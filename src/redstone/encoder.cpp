#include "redstone/encoder.hpp"

#include <array>
#include <format>
#include <stdexcept>

#include "redstone/cp_builder.hpp"

namespace rs {

namespace sat = operations_research::sat;
using cp::Int;
using cp::Lit;

namespace {

enum Kind : int {
  kAir,
  kSolid,
  kGlass,
  kDust,
  kTorch,
  kRedstoneBlock,
  kRepeater,
  kComparator,
  kTarget,
  kObserver,
  kNoteBlock,
  kHopper,
  kBarrel,
  kChest,
  kInputComparator,
  kInputContainer,
  kInputRepeater,
  kInputDust,
  kOutputDust,
  kOutputRepeater,
  kOutputComparator,
  kKindCount
};

constexpr int kRepeaterDelays = kMaxRepeaterDelay;
constexpr int kTicksPerRepeaterStep = 2;
constexpr int kComponentDelay = 2; // torch, comparator, observer
constexpr int kMaxScheduleDelay = kRepeaterDelays * kTicksPerRepeaterStep;
constexpr int kNoChannel = -1;

constexpr std::array<Dir, 5> kTorchAttachments{
    Dir::Down, Dir::North, Dir::South, Dir::West, Dir::East};

Kind palette_kind(BlockType type) {
  switch (type) {
  case BlockType::Solid:
    return kSolid;
  case BlockType::Glass:
    return kGlass;
  case BlockType::Dust:
    return kDust;
  case BlockType::Torch:
    return kTorch;
  case BlockType::RedstoneBlock:
    return kRedstoneBlock;
  case BlockType::Repeater:
    return kRepeater;
  case BlockType::Comparator:
    return kComparator;
  case BlockType::Target:
    return kTarget;
  case BlockType::Observer:
    return kObserver;
  case BlockType::NoteBlock:
    return kNoteBlock;
  case BlockType::Hopper:
    return kHopper;
  case BlockType::Barrel:
    return kBarrel;
  case BlockType::Chest:
    return kChest;
  default:
    throw std::invalid_argument("block type cannot be placed by the solver");
  }
}

Kind kind_of(const Block &block) {
  switch (block.type) {
  case BlockType::Air:
    return kAir;
  case BlockType::Input:
    switch (block.input_kind()) {
    case InputKind::Comparator:
      return kInputComparator;
    case InputKind::Container:
      return kInputContainer;
    case InputKind::Repeater:
      return kInputRepeater;
    case InputKind::Dust:
      return kInputDust;
    }
    throw std::logic_error("unknown input kind");
  case BlockType::Output:
    switch (block.output_kind()) {
    case OutputKind::Dust:
      return kOutputDust;
    case OutputKind::Repeater:
      return kOutputRepeater;
    case OutputKind::Comparator:
      return kOutputComparator;
    }
    throw std::logic_error("unknown output kind");
  default:
    return palette_kind(block.type);
  }
}

bool is_directional(Kind kind) {
  return kind == kTorch || kind == kRepeater || kind == kComparator ||
         kind == kObserver;
}

bool has_stored_signal(Kind kind) {
  return kind == kHopper || kind == kBarrel || kind == kChest;
}

} // namespace

struct CircuitEncoder::Impl {
  struct Cell {
    Vec3 position{};
    bool fixed = false;
    int channel = kNoChannel; // port channel for fixed cells
    std::array<Lit, kKindCount> kind;
    std::array<Lit, kDirCount> facing;
    std::array<Lit, kRepeaterDelays> delay; // delay[k]: repeater delay k + 1
    Lit subtract;
    Int stored = Int::constant(0);

    Lit conductor, top_support, side_support, dust, container, component,
        directional;
    std::array<Lit, kDirCount> points; // dust powers this horizontal side

    bool may(Kind k) const { return !this->kind[k].is_false(); }
  };

  struct Components {
    Lit lit;    // torch
    Lit rep_on; // repeater
    Lit obs_on; // observer
    Int comp_out = Int::constant(0);
  };

  struct Slice {
    std::vector<Components> comp;
    InputVector inputs;
    std::vector<Int> dust;
    std::vector<Lit> torch_powered, rep_should, rep_locked, block_powered,
        comp_positive;
    std::vector<Int> calc;
    std::vector<Int> outputs;
  };

  // Scratch values that only live while one slice is being built.
  struct SliceScratch {
    std::vector<std::array<Int, kDirCount>> weak, strong, dust_emit;
    std::vector<Int> conducted_without_dust, conducted;
    std::vector<std::array<std::optional<Lit>, kDirCount>> signal_positive;
  };

  struct TransitionData {
    TransitionCase transition;
    std::vector<std::vector<Int>> outputs; // [tick][channel]
  };

  Impl(const Spec &spec, const ModelPlan &plan, EncoderOptions options)
      : spec(spec), options(std::move(options)), b(model),
        horizon(spec.timing.horizon()) {
    spec.validate();
    this->base = spec.base_world();
    this->build_cells();
    this->add_static_constraints();
    if (!spec.allow_feedback || spec.is_tiled())
      this->add_dependency_constraints();

    for (const SteadyCase &steady : plan.steady) {
      if (steady.inputs.size() != spec.inputs.size())
        throw std::invalid_argument(
            "steady case does not match the input count");
      this->steady_slices.push_back(this->make_steady(steady));
    }
    for (const TransitionCase &transition : plan.transitions) {
      if (transition.from < 0 || transition.to < 0 ||
          transition.from >= static_cast<int>(plan.steady.size()) ||
          transition.to >= static_cast<int>(plan.steady.size()))
        throw std::invalid_argument(
            "transition refers to a missing steady case");
      this->add_transition(transition, plan.steady[transition.to]);
    }

    if (this->options.fixed_layout)
      this->require_layout(*this->options.fixed_layout, true);
    for (const World &layout : this->options.forbidden_layouts)
      this->require_layout(layout, false);
    if (this->options.max_blocks)
      this->limit_blocks(*this->options.max_blocks);
    if (this->options.minimize_blocks)
      this->add_objective();
  }

  void limit_blocks(int max_blocks) {
    sat::LinearExpr placed;
    for (const Cell &c : this->cells)
      if (!c.fixed)
        placed += sat::LinearExpr(1) - c.kind[kAir].expr();
    this->model.AddLessOrEqual(placed, max_blocks);
  }

  // ---------------------------------------------------------------- cells

  // Tiled axes wrap around: the neighbour across the edge is the same cell
  // of the next copy.
  int cell_index(Vec3 p) const {
    Vec3 size = this->base.size();
    auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    if (this->spec.tiled[0])
      p.x = wrap(p.x, size.x);
    if (this->spec.tiled[1])
      p.y = wrap(p.y, size.y);
    if (this->spec.tiled[2])
      p.z = wrap(p.z, size.z);
    return this->base.in_bounds(p) ? this->base.index_of(p) : -1;
  }

  const Cell &cell(int index) const {
    return index < 0 ? this->air_cell : this->cells[index];
  }

  const Cell &cell_at(Vec3 p) const { return this->cell(this->cell_index(p)); }

  void build_cells() {
    this->air_cell = this->constant_cell(kAir);
    this->cells.resize(this->base.volume());

    std::vector<Kind> palette{kAir};
    for (BlockType type : this->spec.palette)
      palette.push_back(palette_kind(type));
    bool any_directional = false;
    bool any_stored = false;
    for (Kind k : palette) {
      any_directional |= is_directional(k);
      any_stored |= has_stored_signal(k);
    }

    for (int i = 0; i < this->base.volume(); ++i) {
      Vec3 p = this->base.position_of(i);
      const Block &block = this->base.at(p);
      Cell &c = this->cells[i];
      if (block.type == BlockType::Input || block.type == BlockType::Output) {
        c = this->constant_cell(kind_of(block));
        c.facing = this->constant_facing(block.facing);
        c.fixed = true;
        c.channel = block.channel;
      } else {
        for (Lit &lit : c.kind)
          lit = this->b.constant(false);
        std::vector<sat::BoolVar> kind_vars;
        for (Kind k : palette) {
          c.kind[k] = this->b.new_bool();
          kind_vars.push_back(c.kind[k].var());
        }
        this->model.AddExactlyOne(kind_vars);

        if (any_directional) {
          std::vector<sat::BoolVar> facing_vars;
          for (Dir d : kAllDirs) {
            c.facing[index_of(d)] = this->b.new_bool();
            facing_vars.push_back(c.facing[index_of(d)].var());
          }
          this->model.AddExactlyOne(facing_vars);
        } else {
          c.facing = this->constant_facing(Dir::Down);
        }

        if (c.may(kRepeater)) {
          std::vector<sat::BoolVar> delay_vars;
          for (Lit &lit : c.delay) {
            lit = this->b.new_bool();
            delay_vars.push_back(lit.var());
          }
          this->model.AddExactlyOne(delay_vars);
        } else {
          c.delay = this->constant_delay();
        }
        c.subtract =
            c.may(kComparator) ? this->b.new_bool() : this->b.constant(false);
        c.stored =
            any_stored ? this->b.new_int(0, kMaxSignal) : Int::constant(0);
      }
      c.position = p;
    }

    for (Cell &c : this->cells)
      this->derive(c);
    this->derive(this->air_cell);
    for (Cell &c : this->cells)
      this->derive_dust_shape(c);
  }

  Cell constant_cell(Kind kind) const {
    Cell c;
    for (int k = 0; k < kKindCount; ++k)
      c.kind[k] = this->b.constant(k == kind);
    c.facing = this->constant_facing(Dir::Down);
    c.delay = this->constant_delay();
    c.subtract = this->b.constant(false);
    for (Lit &lit : c.points)
      lit = this->b.constant(false);
    return c;
  }

  std::array<Lit, kDirCount> constant_facing(Dir facing) const {
    std::array<Lit, kDirCount> result;
    for (Dir d : kAllDirs)
      result[index_of(d)] = this->b.constant(d == facing);
    return result;
  }

  std::array<Lit, kRepeaterDelays> constant_delay() const {
    std::array<Lit, kRepeaterDelays> result;
    for (int k = 0; k < kRepeaterDelays; ++k)
      result[k] = this->b.constant(k == 0);
    return result;
  }

  void derive(Cell &c) {
    const auto &k = c.kind;
    c.conductor = this->b.any_exclusive(
        {k[kSolid], k[kTarget], k[kNoteBlock], k[kBarrel], k[kInputContainer]});
    c.side_support = this->b.any_exclusive(
        {k[kSolid], k[kGlass], k[kRedstoneBlock], k[kTarget], k[kNoteBlock],
         k[kBarrel], k[kObserver], k[kInputContainer]});
    c.top_support = this->b.any_exclusive({c.side_support, k[kHopper]});
    c.dust = this->b.any_exclusive({k[kDust], k[kOutputDust], k[kInputDust]});
    c.container = this->b.any_exclusive(
        {k[kHopper], k[kBarrel], k[kChest], k[kInputContainer]});
    c.directional = this->b.any_exclusive(
        {k[kTorch], k[kRepeater], k[kComparator], k[kObserver]});
    c.component = c.directional;
  }

  // RedStoneWireBlock connection rules; static because blocks never change.
  void derive_dust_shape(Cell &c) {
    for (Lit &lit : c.points)
      lit = this->b.constant(false);
    if (c.dust.is_false())
      return;

    Vec3 p = c.position;
    Lit can_go_up = !this->cell_at(p + offset(Dir::Up)).conductor;
    std::array<Lit, kDirCount> connected;
    for (Dir d : kHorizontalDirs) {
      Vec3 side = p + offset(d);
      const Cell &n = this->cell_at(side);
      Lit up = this->b.all({can_go_up, n.top_support,
                            this->cell_at(side + offset(Dir::Up)).dust});
      Lit down = this->b.all(
          {!n.conductor, this->cell_at(side + offset(Dir::Down)).dust});
      connected[index_of(d)] =
          this->b.any({up, this->dust_connects_to(n, d), down});
    }
    auto conn = [&](Dir d) { return connected[index_of(d)]; };
    Lit no_east_west = !this->b.any({conn(Dir::East), conn(Dir::West)});
    Lit no_north_south = !this->b.any({conn(Dir::North), conn(Dir::South)});
    c.points[index_of(Dir::North)] =
        this->b.any({conn(Dir::North), no_east_west});
    c.points[index_of(Dir::South)] =
        this->b.any({conn(Dir::South), no_east_west});
    c.points[index_of(Dir::East)] =
        this->b.any({conn(Dir::East), no_north_south});
    c.points[index_of(Dir::West)] =
        this->b.any({conn(Dir::West), no_north_south});
  }

  Lit dust_connects_to(const Cell &n, Dir toward) {
    const auto &k = n.kind;
    Lit axis = this->b.any_exclusive(
        {n.facing[index_of(toward)], n.facing[index_of(opposite(toward))]});
    return this->b.any_exclusive({
        n.dust,
        this->b.all({k[kRepeater], axis}),
        this->b.all({k[kOutputRepeater], axis}),
        this->b.all({k[kInputRepeater], axis}),
        this->b.all({k[kObserver], n.facing[index_of(toward)]}),
        k[kTorch],
        k[kRedstoneBlock],
        k[kComparator],
        k[kTarget],
        k[kInputComparator],
        k[kOutputComparator],
    });
  }

  void add_static_constraints() {
    for (const Cell &c : this->cells) {
      Vec3 p = c.position;
      const Cell &below = this->cell_at(p + offset(Dir::Down));
      this->b.require(below.top_support, {c.dust});
      // Ports standing in for repeaters and comparators need a block below.
      this->b.require(
          below.top_support,
          {this->b.any_exclusive(
              {c.kind[kInputComparator], c.kind[kInputRepeater],
               c.kind[kOutputRepeater], c.kind[kOutputComparator]})});
      if (c.fixed)
        continue;

      const auto &k = c.kind;
      for (Kind diode : {kRepeater, kComparator}) {
        this->b.require(below.top_support, {k[diode]});
        this->b.require(!c.facing[index_of(Dir::Up)], {k[diode]});
        this->b.require(!c.facing[index_of(Dir::Down)], {k[diode]});
      }
      // A locked repeater holds a stale value, which is memory just like a
      // loop; truth-table mode forbids diodes feeding a repeater's side.
      if (!this->spec.allow_feedback && c.may(kRepeater)) {
        for (Dir d : kHorizontalDirs)
          for (Dir side : {rotate_clockwise(d), rotate_counter_clockwise(d)}) {
            const Cell &n = this->cell_at(p + offset(side));
            Lit n_diode = this->b.any_exclusive(
                {n.kind[kRepeater], n.kind[kComparator],
                 n.kind[kInputComparator], n.kind[kInputRepeater]});
            this->b.require_any({!k[kRepeater], !c.facing[index_of(d)],
                                 !n_diode,
                                 !n.facing[index_of(opposite(side))]});
          }
      }
      this->b.require(!c.facing[index_of(Dir::Up)], {k[kTorch]});
      this->b.require(below.top_support,
                      {k[kTorch], c.facing[index_of(Dir::Down)]});
      for (Dir d : kHorizontalDirs)
        this->b.require(this->cell_at(p + offset(d)).side_support,
                        {k[kTorch], c.facing[index_of(d)]});

      // Symmetry breaking: unused settings take a fixed value.
      this->b.require(c.facing[index_of(Dir::Down)], {!c.directional});
      this->b.require(c.delay[0], {!k[kRepeater]});
      this->b.require(!c.subtract, {!k[kComparator]});
      Lit stores = this->b.any_exclusive({k[kHopper], k[kBarrel], k[kChest]});
      this->b.require_equal(c.stored, Int::constant(0), {!stores});

      for (Dir d : kHorizontalDirs)
        this->b.require_any(
            {!k[kChest], !this->cell_at(p + offset(d)).kind[kChest]});
      this->b.require(!this->cell_at(p + offset(Dir::Up)).container,
                      {k[kHopper]});
      this->b.require(!below.container, {k[kHopper]});
    }
  }

  // Can cell q send power to its neighbour in direction d? `strong` limits it
  // to what can power a conductor.
  Lit may_emit(const Cell &q, Dir d, bool strong) {
    const auto &k = q.kind;
    Lit forward = q.facing[index_of(d)];
    std::vector<Lit> ways{
        this->b.all(
            {this->b.any_exclusive({k[kRepeater], k[kComparator],
                                    k[kInputComparator], k[kInputRepeater]}),
             forward}),
        this->b.all({k[kObserver], q.facing[index_of(opposite(d))]}),
        this->b.all({q.dust, d == Dir::Down ? this->b.constant(true)
                                            : q.points[index_of(d)]}),
    };
    if (strong) {
      if (d == Dir::Up)
        ways.push_back(k[kTorch]);
    } else {
      ways.push_back(this->b.all({k[kTorch], !forward}));
      ways.push_back(q.conductor);
    }
    return this->b.any_exclusive(ways);
  }

  // Every signal dependency between neighbouring cells is an edge from the
  // cell read (q) to the cell reading it (p).
  //  - Without feedback, every cell gets a rank: components rank strictly
  //    above what they read and everything else at least as high as what
  //    powers it, so no signal loop can pass through a component.
  //  - With tiling, no edge may cross into a neighbouring copy, so copies
  //    cannot influence each other whatever their inputs are.
  void add_dependency_constraints() {
    const int n = static_cast<int>(this->cells.size());
    const bool ranked = !this->spec.allow_feedback;
    std::vector<Int> rank;
    if (ranked)
      for (int i = 0; i < n; ++i)
        rank.push_back(this->b.new_int(0, n));

    auto edge = [&](int p, Vec3 q_position, int gap, std::vector<Lit> when) {
      int q = this->cell_index(q_position);
      if (q < 0)
        return;
      if (!this->base.in_bounds(q_position)) {
        when.push_back(!this->cells[q].kind[kAir]);
        for (Lit &lit : when)
          lit = !lit;
        this->b.require_any(when);
        return;
      }
      if (ranked)
        this->b.require_in_range(rank[p] - rank[q], gap, n, when);
    };

    for (int p = 0; p < n; ++p) {
      const Cell &c = this->cells[p];
      Vec3 pos = c.position;
      const auto &k = c.kind;

      for (Dir d : kAllDirs) {
        Vec3 q = pos + offset(d);
        const Cell &neighbor = this->cell_at(q);
        Dir toward_p = opposite(d);
        Lit weak = this->may_emit(neighbor, toward_p, false);
        edge(p, q, 0, {c.dust, weak});
        edge(p, q, 0, {k[kHopper], weak});
        edge(p, q, 0, {c.conductor, this->may_emit(neighbor, toward_p, true)});
      }
      if (!c.dust.is_false()) {
        for (Dir d : kHorizontalDirs)
          for (Dir vertical : {Dir::Up, Dir::Down}) {
            Vec3 q = pos + offset(d) + offset(vertical);
            edge(p, q, 0, {c.dust, this->cell_at(q).dust});
          }
      }

      for (Dir d : kTorchAttachments)
        edge(p, pos + offset(d), 1, {k[kTorch], c.facing[index_of(d)]});
      for (Dir d : kAllDirs)
        edge(p, pos + offset(d), 1, {k[kObserver], c.facing[index_of(d)]});
      Lit diode = this->b.any_exclusive({k[kRepeater], k[kComparator]});
      for (Dir d : kHorizontalDirs) {
        Lit facing = c.facing[index_of(d)];
        Vec3 behind = pos + offset(opposite(d));
        edge(p, behind, 1, {diode, facing});
        edge(p, behind + offset(opposite(d)), 1, {k[kComparator], facing});
        for (Dir side : {rotate_clockwise(d), rotate_counter_clockwise(d)})
          edge(p, pos + offset(side), 1, {diode, facing});
      }
    }
  }

  // --------------------------------------------------------------- slices

  Components new_components(const Cell &c, bool steady) {
    Components x;
    x.lit = c.may(kTorch) ? this->b.new_bool() : this->b.constant(false);
    x.rep_on = c.may(kRepeater) ? this->b.new_bool() : this->b.constant(false);
    x.obs_on = c.may(kObserver) && !steady ? this->b.new_bool()
                                           : this->b.constant(false);
    x.comp_out =
        c.may(kComparator) ? this->b.new_int(0, kMaxSignal) : Int::constant(0);
    this->b.require(!x.lit, {!c.kind[kTorch]});
    this->b.require(!x.rep_on, {!c.kind[kRepeater]});
    this->b.require(!x.obs_on, {!c.kind[kObserver]});
    this->b.require_equal(x.comp_out, Int::constant(0), {!c.kind[kComparator]});
    return x;
  }

  std::vector<Components> new_component_layer(bool steady) {
    std::vector<Components> layer;
    layer.reserve(this->cells.size());
    for (const Cell &c : this->cells)
      layer.push_back(this->new_components(c, steady));
    return layer;
  }

  int input_value(const Slice &s, const Cell &c) const {
    return c.channel == kNoChannel ? 0 : s.inputs[c.channel];
  }

  // Computes everything that settles within a tick from the component states.
  void build_instant(Slice &s) {
    const int n = static_cast<int>(this->cells.size());
    SliceScratch x;
    x.weak.resize(n);
    x.strong.resize(n);
    x.dust_emit.resize(n);
    x.conducted_without_dust.assign(n, Int::constant(0));
    x.conducted.assign(n, Int::constant(0));
    x.signal_positive.resize(n);
    s.dust.assign(n, Int::constant(0));
    s.torch_powered.assign(n, this->b.constant(false));
    s.rep_should = s.torch_powered;
    s.rep_locked = s.torch_powered;
    s.block_powered = s.torch_powered;
    s.comp_positive = s.torch_powered;
    s.calc.assign(n, Int::constant(0));

    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      if (c.kind[kInputDust].is_true())
        s.dust[i] = Int::constant(this->input_value(s, c));
      else if (!c.dust.is_false())
        s.dust[i] = this->b.new_int(0, kMaxSignal);
      if (c.may(kComparator))
        s.comp_positive[i] = this->b.positive(s.comp[i].comp_out);
    }

    // Emission of every block towards each side (dust separately).
    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      const Components &k = s.comp[i];
      for (Dir d : kAllDirs) {
        int di = index_of(d);
        std::vector<Int> diode;
        if (c.may(kRepeater))
          diode.push_back(this->b.scaled(this->b.all({k.rep_on, c.facing[di]}),
                                         kMaxSignal));
        if (c.may(kComparator))
          diode.push_back(this->b.select(c.facing[di], k.comp_out));
        if (c.may(kObserver))
          diode.push_back(this->b.scaled(
              this->b.all({k.obs_on, c.facing[index_of(opposite(d))]}),
              kMaxSignal));
        if (c.facing[di].is_true()) {
          if (c.kind[kInputComparator].is_true())
            diode.push_back(Int::constant(this->input_value(s, c)));
          if (c.kind[kInputRepeater].is_true())
            diode.push_back(
                Int::constant(this->input_value(s, c) > 0 ? kMaxSignal : 0));
        }
        Int diode_sum = cp::Builder::exclusive_sum(diode);

        std::vector<Int> weak{
            diode_sum, this->b.scaled(c.kind[kRedstoneBlock], kMaxSignal)};
        std::vector<Int> strong{diode_sum};
        if (c.may(kTorch)) {
          weak.push_back(
              this->b.scaled(this->b.all({k.lit, !c.facing[di]}), kMaxSignal));
          if (d == Dir::Up)
            strong.push_back(this->b.scaled(k.lit, kMaxSignal));
        }
        x.weak[i][di] = cp::Builder::exclusive_sum(weak);
        x.strong[i][di] = cp::Builder::exclusive_sum(strong);

        if (d == Dir::Down)
          x.dust_emit[i][di] = s.dust[i];
        else if (d == Dir::Up)
          x.dust_emit[i][di] = Int::constant(0);
        else
          x.dust_emit[i][di] = this->b.select(c.points[di], s.dust[i]);
      }
    }

    auto neighbor_value =
        [&](const std::vector<std::array<Int, kDirCount>> &table, Vec3 p,
            Dir k) {
          int j = this->cell_index(p + offset(k));
          return j < 0 ? Int::constant(0) : table[j][index_of(opposite(k))];
        };

    // Power held by conductors (Java getDirectSignalTo), with and without dust.
    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      if (c.conductor.is_false())
        continue;
      std::vector<Int> strong_in{Int::constant(0)};
      std::vector<Int> dust_in;
      for (Dir k : kAllDirs) {
        strong_in.push_back(neighbor_value(x.strong, c.position, k));
        dust_in.push_back(neighbor_value(x.dust_emit, c.position, k));
      }
      Int without_dust = this->b.max_of(strong_in);
      dust_in.push_back(without_dust);
      Int with_dust = this->b.max_of(dust_in);
      x.conducted_without_dust[i] = this->b.select(c.conductor, without_dust);
      x.conducted[i] = this->b.select(c.conductor, with_dust);
    }

    // Java Level.getSignal from cell j towards d.
    auto signal_without_dust = [&](int j, Dir d) {
      if (j < 0)
        return Int::constant(0);
      return cp::Builder::exclusive_sum(
          {x.weak[j][index_of(d)], x.conducted_without_dust[j]});
    };
    auto signal = [&](int j, Dir d) {
      if (j < 0)
        return Int::constant(0);
      return cp::Builder::exclusive_sum({x.weak[j][index_of(d)],
                                         x.dust_emit[j][index_of(d)],
                                         x.conducted[j]});
    };
    auto signal_positive = [&](int j, Dir d) {
      if (j < 0)
        return this->b.constant(false);
      std::optional<Lit> &cached = x.signal_positive[j][index_of(d)];
      if (!cached)
        cached = this->b.positive(signal(j, d));
      return *cached;
    };
    auto from_side = [&](Vec3 p, Dir k) {
      return this->cell_index(p + offset(k));
    };

    // Dust power: strongest source, or strongest connected dust minus one.
    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      if (c.dust.is_false() || c.kind[kInputDust].is_true())
        continue;
      Vec3 p = c.position;
      std::vector<Int> candidates{Int::constant(0)};
      for (Dir k : kAllDirs)
        candidates.push_back(signal_without_dust(from_side(p, k), opposite(k)));
      Lit covered = this->cell_at(p + offset(Dir::Up)).conductor;
      for (Dir d : kHorizontalDirs) {
        Vec3 side = p + offset(d);
        const Cell &neighbor = this->cell_at(side);
        auto dust_at = [&](Vec3 q) {
          int j = this->cell_index(q);
          return j < 0 ? Int::constant(0) : s.dust[j];
        };
        candidates.push_back(dust_at(side).plus(-1));
        candidates.push_back(
            this->b
                .select(this->b.all({neighbor.conductor, !covered}),
                        dust_at(side + offset(Dir::Up)))
                .plus(-1));
        candidates.push_back(
            this->b
                .select(!neighbor.conductor, dust_at(side + offset(Dir::Down)))
                .plus(-1));
      }
      Int strongest = this->b.max_of(candidates);
      this->b.require_equal(s.dust[i], strongest, {c.dust});
      this->b.require_equal(s.dust[i], Int::constant(0), {!c.dust});
    }

    // What a comparator at p sending towards d reads from behind, including
    // containers (also through a conductor).
    auto rear_reading = [&](Vec3 p, Dir d) {
      int behind = from_side(p, opposite(d));
      int further =
          this->cell_index(p + offset(opposite(d)) + offset(opposite(d)));
      Int rear_signal = signal(behind, d);
      const Cell &behind_cell = this->cell(behind);
      Lit through = this->b.all(
          {behind_cell.conductor, this->cell(further).container,
           !this->b.greater(rear_signal, Int::constant(kMaxSignal - 1))});
      return this->b.if_else(
          behind_cell.container, this->analog(s, behind),
          this->b.if_else(through, this->analog(s, further), rear_signal));
    };

    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      Vec3 p = c.position;

      if (c.may(kTorch)) {
        std::vector<Lit> attached;
        for (Dir d : kTorchAttachments)
          attached.push_back(
              this->b.all({c.facing[index_of(d)],
                           signal_positive(from_side(p, d), opposite(d))}));
        s.torch_powered[i] = this->b.any_exclusive(attached);
      }

      if (c.may(kRepeater)) {
        std::vector<Lit> should;
        std::vector<Lit> locked;
        for (Dir d : kHorizontalDirs) {
          Lit facing = c.facing[index_of(d)];
          should.push_back(this->b.all(
              {facing, signal_positive(from_side(p, opposite(d)), d)}));
          Dir right = rotate_clockwise(d);
          Dir left = rotate_counter_clockwise(d);
          Lit side = this->b.any(
              {this->diode_powering(s, from_side(p, right), opposite(right)),
               this->diode_powering(s, from_side(p, left), opposite(left))});
          locked.push_back(this->b.all({facing, side}));
        }
        s.rep_should[i] = this->b.any_exclusive(should);
        s.rep_locked[i] = this->b.any_exclusive(locked);
      }

      if (c.may(kNoteBlock) || c.may(kHopper)) {
        std::vector<Lit> powered;
        for (Dir k : kAllDirs)
          powered.push_back(signal_positive(from_side(p, k), opposite(k)));
        s.block_powered[i] = this->b.all(
            {this->b.any_exclusive({c.kind[kNoteBlock], c.kind[kHopper]}),
             this->b.any(powered)});
      }

      if (c.may(kComparator)) {
        std::vector<Int> rear_terms;
        std::vector<Int> side_terms;
        for (Dir d : kHorizontalDirs) {
          Lit facing = c.facing[index_of(d)];
          if (facing.is_false())
            continue;
          Int rear = rear_reading(p, d);
          rear_terms.push_back(this->b.select(facing, rear));

          Dir right = rotate_clockwise(d);
          Dir left = rotate_counter_clockwise(d);
          Int side = this->b.max_of(
              {this->control_signal(s, x, from_side(p, right), opposite(right)),
               this->control_signal(s, x, from_side(p, left), opposite(left))});
          side_terms.push_back(this->b.select(facing, side));
        }
        Int rear = cp::Builder::exclusive_sum(rear_terms);
        Int side = cp::Builder::exclusive_sum(side_terms);
        Int calc = this->b.new_int(0, kMaxSignal);
        Lit is_comparator = c.kind[kComparator];
        Lit passes =
            this->b.all({this->b.positive(rear), !this->b.greater(side, rear)});
        this->b.require_equal(calc, Int::constant(0), {!is_comparator});
        this->b.require_equal(calc, Int::constant(0), {is_comparator, !passes});
        this->b.require_equal(calc, rear - side,
                              {is_comparator, passes, c.subtract});
        this->b.require_equal(calc, rear, {is_comparator, passes, !c.subtract});
        s.calc[i] = calc;
      }
    }

    s.outputs.clear();
    for (const OutputPort &port : this->spec.outputs) {
      int j = this->cell_index(port.position);
      if (port.kind == OutputKind::Dust)
        s.outputs.push_back(s.dust[j]);
      else if (port.kind == OutputKind::Comparator)
        s.outputs.push_back(rear_reading(port.position, port.facing));
      else
        s.outputs.push_back(
            signal(this->cell_index(port.position - offset(port.facing)),
                   port.facing));
    }
  }

  // Comparator analog reading of a container (or a container-like input).
  Int analog(const Slice &s, int j) const {
    if (j < 0)
      return Int::constant(0);
    const Cell &c = this->cells[j];
    if (c.kind[kInputContainer].is_true())
      return Int::constant(this->input_value(s, c));
    return c.stored;
  }

  // A repeater, comparator or strong input at j pointing towards d with output.
  Lit diode_powering(const Slice &s, int j, Dir d) {
    if (j < 0)
      return this->b.constant(false);
    const Cell &c = this->cells[j];
    Lit facing = c.facing[index_of(d)];
    if (c.kind[kInputComparator].is_true() || c.kind[kInputRepeater].is_true())
      return this->b.constant(facing.is_true() && this->input_value(s, c) > 0);
    return this->b.any_exclusive({this->b.all({s.comp[j].rep_on, facing}),
                                  this->b.all({s.comp_positive[j], facing})});
  }

  // Comparator side input from j towards d (Java getControlInputSignal).
  Int control_signal(const Slice &s, const SliceScratch &x, int j, Dir d) {
    if (j < 0)
      return Int::constant(0);
    const Cell &c = this->cells[j];
    return cp::Builder::exclusive_sum(
        {x.strong[j][index_of(d)],
         this->b.scaled(c.kind[kRedstoneBlock], kMaxSignal), s.dust[j]});
  }

  // ------------------------------------------------------------ dynamics

  void
  require_outputs(const Slice &s,
                  const std::optional<std::vector<SignalRange>> &expected) {
    if (!this->options.enforce_outputs || !expected)
      return;
    for (std::size_t o = 0; o < expected->size(); ++o)
      this->b.require_in_range(s.outputs[o], (*expected)[o].min,
                               (*expected)[o].max);
  }

  Slice make_steady(const SteadyCase &steady) {
    Slice s;
    s.inputs = steady.inputs;
    s.comp = this->new_component_layer(true);
    this->build_instant(s);
    for (std::size_t i = 0; i < this->cells.size(); ++i) {
      const Cell &c = this->cells[i];
      const Components &k = s.comp[i];
      this->b.require_equal(k.lit, !s.torch_powered[i], {c.kind[kTorch]});
      // Even a locked repeater must agree with its input: a latched value
      // would be memory, which a truth table cannot rely on.
      this->b.require_equal(k.rep_on, s.rep_should[i], {c.kind[kRepeater]});
      this->b.require_equal(k.comp_out, s.calc[i], {c.kind[kComparator]});
    }
    this->require_outputs(s, steady.expected);
    return s;
  }

  // Did the block state of cell j change between two slices (what observers
  // see)?
  Lit changed(const Slice &before, const Slice &after, int j) {
    if (j < 0)
      return this->b.constant(false);
    const Cell &c = this->cells[j];
    std::vector<Lit> diffs;
    if (!c.dust.is_false())
      diffs.push_back(this->b.not_equal(before.dust[j], after.dust[j]));
    if (c.may(kTorch))
      diffs.push_back(
          this->b.exclusive_or(before.comp[j].lit, after.comp[j].lit));
    if (c.may(kRepeater)) {
      diffs.push_back(
          this->b.exclusive_or(before.comp[j].rep_on, after.comp[j].rep_on));
      diffs.push_back(
          this->b.exclusive_or(before.rep_locked[j], after.rep_locked[j]));
    }
    if (c.may(kComparator))
      diffs.push_back(this->b.exclusive_or(before.comp_positive[j],
                                           after.comp_positive[j]));
    if (c.may(kObserver))
      diffs.push_back(
          this->b.exclusive_or(before.comp[j].obs_on, after.comp[j].obs_on));
    if (c.may(kNoteBlock) || c.may(kHopper))
      diffs.push_back(this->b.exclusive_or(before.block_powered[j],
                                           after.block_powered[j]));
    return this->b.any(diffs);
  }

  void add_transition(TransitionCase transition, const SteadyCase &target) {
    const int n = static_cast<int>(this->cells.size());
    const int ticks = this->horizon;
    const Slice &start = this->steady_slices[transition.from];
    const Slice &end = this->steady_slices[transition.to];

    // schedule[t][cell]: a scheduled tick fires at time t.
    std::vector<std::vector<Lit>> schedule(
        ticks + kMaxScheduleDelay + 1,
        std::vector<Lit>(n, this->b.constant(false)));
    for (int t = 2; t <= ticks; ++t)
      for (int i = 0; i < n; ++i)
        if (!this->cells[i].component.is_false())
          schedule[t][i] = this->b.new_bool();
    for (int t = 2; t <= ticks; ++t)
      for (int i = 0; i < n; ++i)
        this->b.require(!schedule[t][i], {!this->cells[i].component});

    std::vector<Slice> slices(ticks + 1);
    slices[0].inputs = target.inputs;
    slices[0].comp = start.comp;
    this->build_instant(slices[0]);
    for (int t = 1; t < ticks; ++t) {
      slices[t].inputs = target.inputs;
      slices[t].comp = this->new_component_layer(false);
      this->build_instant(slices[t]);
    }
    if (ticks >= 1)
      slices[ticks] = end;
    else
      throw std::logic_error("horizon must be at least one tick");

    for (int t = 1; t <= ticks; ++t)
      this->add_step(slices[t - 1], slices[t], schedule[t]);
    for (int t = 0; t <= ticks; ++t)
      this->add_scheduling(t == 0 ? start : slices[t - 1], slices[t], schedule,
                           t);
    for (int i = 0; i < n; ++i) {
      const Cell &c = this->cells[i];
      for (int k = 0; k < kRepeaterDelays; ++k)
        for (int t = 1; t < (k + 1) * kTicksPerRepeaterStep && t <= ticks; ++t)
          this->b.require(!schedule[t][i], {c.kind[kRepeater], c.delay[k]});
    }

    TransitionData data{transition, {}};
    for (int t = 0; t <= ticks; ++t)
      data.outputs.push_back(slices[t].outputs);
    if (this->options.enforce_outputs && target.expected) {
      const TimingSpec &timing = this->spec.timing;
      int first_checked = timing.latest ? *timing.latest : ticks;
      for (int t = std::min(first_checked, ticks); t <= ticks; ++t)
        for (std::size_t o = 0; o < target.expected->size(); ++o)
          this->b.require_in_range(slices[t].outputs[o],
                                   (*target.expected)[o].min,
                                   (*target.expected)[o].max);
      if (timing.hold_previous && timing.earliest)
        for (int t = 0; t < std::min(*timing.earliest, ticks + 1); ++t)
          for (std::size_t o = 0; o < this->spec.outputs.size(); ++o)
            this->b.require_equal(slices[t].outputs[o], start.outputs[o]);
    }
    this->transitions.push_back(std::move(data));
  }

  // State changes from tick t-1 to t given which scheduled ticks fire at t.
  void add_step(const Slice &prev, const Slice &cur,
                const std::vector<Lit> &fires) {
    for (std::size_t i = 0; i < this->cells.size(); ++i) {
      const Cell &c = this->cells[i];
      if (c.component.is_false())
        continue;
      const Components &a = prev.comp[i];
      const Components &z = cur.comp[i];
      Lit fire = fires[i];
      const auto &k = c.kind;

      this->b.require_equal(z.lit, !prev.torch_powered[i], {k[kTorch], fire});
      this->b.require_equal(z.lit, a.lit, {k[kTorch], !fire});

      Lit locked = prev.rep_locked[i];
      this->b.require_equal(z.rep_on,
                            this->b.any({!a.rep_on, prev.rep_should[i]}),
                            {k[kRepeater], fire, !locked});
      this->b.require_equal(z.rep_on, a.rep_on, {k[kRepeater], fire, locked});
      this->b.require_equal(z.rep_on, a.rep_on, {k[kRepeater], !fire});

      this->b.require_equal(z.comp_out, prev.calc[i], {k[kComparator], fire});
      this->b.require_equal(z.comp_out, a.comp_out, {k[kComparator], !fire});

      this->b.require_equal(z.obs_on, !a.obs_on, {k[kObserver], fire});
      this->b.require_equal(z.obs_on, a.obs_on, {k[kObserver], !fire});
    }
  }

  // Ticks scheduled at the end of tick t (the state in `cur`).
  void add_scheduling(const Slice &prev, const Slice &cur,
                      std::vector<std::vector<Lit>> &schedule, int t) {
    for (std::size_t i = 0; i < this->cells.size(); ++i) {
      const Cell &c = this->cells[i];
      if (c.component.is_false())
        continue;
      const Components &z = cur.comp[i];
      const auto &k = c.kind;
      Lit pending_next = schedule[t + 1][i];

      this->b.require_equal(
          schedule[t + kComponentDelay][i],
          this->b.all(
              {this->b.equal(z.lit, cur.torch_powered[i]), !pending_next}),
          {k[kTorch]});

      if (c.may(kRepeater)) {
        for (int step = 0; step < kRepeaterDelays; ++step) {
          int delay = (step + 1) * kTicksPerRepeaterStep;
          std::vector<Lit> condition{
              !cur.rep_locked[i],
              this->b.exclusive_or(z.rep_on, cur.rep_should[i])};
          for (int q = t + 1; q < t + delay; ++q)
            condition.push_back(!schedule[q][i]);
          this->b.require_equal(schedule[t + delay][i], this->b.all(condition),
                                {k[kRepeater], c.delay[step]});
        }
      }

      this->b.require_equal(
          schedule[t + kComponentDelay][i],
          this->b.all(
              {this->b.not_equal(cur.calc[i], z.comp_out), !pending_next}),
          {k[kComparator]});

      if (c.may(kObserver)) {
        std::vector<Lit> watched;
        for (Dir d : kAllDirs) {
          Lit facing = c.facing[index_of(d)];
          if (facing.is_false())
            continue;
          watched.push_back(this->b.all(
              {facing,
               this->changed(prev, cur,
                             this->cell_index(c.position + offset(d)))}));
        }
        Lit triggered =
            this->b.all({this->b.any(watched), !z.obs_on, !pending_next});
        Lit rearm = this->b.all({schedule[t][i], z.obs_on});
        this->b.require_equal(schedule[t + kComponentDelay][i],
                              this->b.any({triggered, rearm}), {k[kObserver]});
      }
    }
  }

  // ------------------------------------------------------- layouts & goal

  std::vector<Lit> layout_literals(const World &layout) {
    if (!(layout.size() == this->base.size()))
      throw std::invalid_argument("layout size does not match the spec volume");
    std::vector<Lit> lits;
    for (const Cell &c : this->cells) {
      if (c.fixed)
        continue;
      const Block &block = layout.at(c.position);
      Kind kind = kind_of(block);
      if (block.type == BlockType::Input || block.type == BlockType::Output)
        throw std::invalid_argument(
            "layout has a port where the spec has none");
      lits.push_back(c.kind[kind]);
      if (is_directional(kind))
        lits.push_back(c.facing[index_of(block.facing)]);
      if (kind == kRepeater)
        lits.push_back(c.delay[block.repeater_delay() - 1]);
      if (kind == kComparator)
        lits.push_back(block.comparator_mode() == ComparatorMode::Subtract
                           ? c.subtract
                           : !c.subtract);
      if (has_stored_signal(kind))
        lits.push_back(this->b.equals_value(c.stored, block.stored_signal()));
    }
    return lits;
  }

  void require_layout(const World &layout, bool required) {
    std::vector<Lit> lits = this->layout_literals(layout);
    if (required) {
      for (const Lit &lit : lits)
        this->b.require(lit);
      return;
    }
    for (Lit &lit : lits)
      lit = !lit;
    this->b.require_any(lits);
  }

  // Fewest blocks first; among equal counts, prefer plain blocks so the
  // result is easier to build and the search sees fewer equivalent layouts.
  void add_objective() {
    constexpr int kBlockCost = 100;
    constexpr std::array<std::pair<Kind, int>, 9> kPreference{{
        {kGlass, 1},
        {kRedstoneBlock, 1},
        {kObserver, 1},
        {kTarget, 2},
        {kNoteBlock, 2},
        {kRepeater, 1},
        {kComparator, 1},
        {kBarrel, 3},
        {kHopper, 3},
    }};
    sat::LinearExpr cost;
    for (const Cell &c : this->cells) {
      if (c.fixed)
        continue;
      cost += kBlockCost * (sat::LinearExpr(1) - c.kind[kAir].expr());
      for (auto [kind, penalty] : kPreference)
        cost += penalty * c.kind[kind].expr();
      cost += 3 * c.kind[kChest].expr();
    }
    this->model.Minimize(cost);
  }

  // --------------------------------------------------------------- decode

  EncodedSolution decode(const sat::CpSolverResponse &response) const {
    auto value = [&](const Lit &lit) {
      return lit.is_constant() ? lit.is_true()
                               : sat::SolutionBooleanValue(response, lit.var());
    };
    auto int_value = [&](const Int &x) {
      return static_cast<int>(sat::SolutionIntegerValue(response, x.expr()));
    };

    EncodedSolution solution{this->base, 0, {}, {}, {}};
    for (const Cell &c : this->cells) {
      if (c.fixed)
        continue;
      int kind = 0;
      while (kind < kKindCount && !value(c.kind[kind]))
        ++kind;
      Dir facing = Dir::Down;
      for (Dir d : kAllDirs)
        if (value(c.facing[index_of(d)]))
          facing = d;
      int delay = 1;
      for (int k = 0; k < kRepeaterDelays; ++k)
        if (value(c.delay[k]))
          delay = k + 1;
      ComparatorMode mode = value(c.subtract) ? ComparatorMode::Subtract
                                              : ComparatorMode::Compare;
      int stored = int_value(c.stored);

      Block block;
      switch (kind) {
      case kAir:
        block = Block::air();
        break;
      case kSolid:
        block = Block::solid();
        break;
      case kGlass:
        block = Block::glass();
        break;
      case kDust:
        block = Block::dust();
        break;
      case kTorch:
        block = Block::torch(facing);
        break;
      case kRedstoneBlock:
        block = Block::redstone_block();
        break;
      case kRepeater:
        block = Block::repeater(facing, delay);
        break;
      case kComparator:
        block = Block::comparator(facing, mode);
        break;
      case kTarget:
        block = Block::target();
        break;
      case kObserver:
        block = Block::observer(facing);
        break;
      case kNoteBlock:
        block = Block::note_block();
        break;
      case kHopper:
        block = Block::hopper(stored);
        break;
      case kBarrel:
        block = Block::barrel(stored);
        break;
      case kChest:
        block = Block::chest(stored);
        break;
      default:
        throw std::logic_error(
            std::format("cell has no block kind (kind {})", kind));
      }
      if (block.type != BlockType::Air)
        ++solution.block_count;
      solution.layout.set(c.position, block);
    }

    auto read_outputs = [&](const std::vector<Int> &outputs) {
      std::vector<int> values;
      for (const Int &output : outputs)
        values.push_back(int_value(output));
      return values;
    };
    for (const Slice &s : this->steady_slices) {
      solution.steady_inputs.push_back(s.inputs);
      solution.steady_outputs.push_back(read_outputs(s.outputs));
    }
    for (const TransitionData &data : this->transitions) {
      TransitionTrace trace{data.transition, {}};
      for (const std::vector<Int> &outputs : data.outputs)
        trace.outputs.push_back(read_outputs(outputs));
      solution.traces.push_back(std::move(trace));
    }
    return solution;
  }

  const Spec &spec;
  EncoderOptions options;
  sat::CpModelBuilder model;
  cp::Builder b;
  int horizon;
  World base{{1, 1, 1}};
  std::vector<Cell> cells;
  Cell air_cell;
  std::vector<Slice> steady_slices;
  std::vector<TransitionData> transitions;
};

CircuitEncoder::CircuitEncoder(const Spec &spec, const ModelPlan &plan,
                               EncoderOptions options)
    : impl_(std::make_unique<Impl>(spec, plan, std::move(options))) {}

CircuitEncoder::~CircuitEncoder() = default;

const sat::CpModelBuilder &CircuitEncoder::model() const {
  return this->impl_->model;
}

EncodedSolution
CircuitEncoder::decode(const sat::CpSolverResponse &response) const {
  return this->impl_->decode(response);
}

int CircuitEncoder::horizon() const { return this->impl_->horizon; }

} // namespace rs
