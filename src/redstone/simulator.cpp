#include "redstone/simulator.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace rs {

namespace {

constexpr int kMaxChannels = 256;
constexpr int kTorchDelay = 2;
constexpr int kComparatorDelay = 2;
constexpr int kObserverDelay = 2;
constexpr int kButtonPressTicks = 20; // stone button
constexpr int kLampOffDelay = 4;
constexpr int kGameTicksPerRepeaterStep = 2;
// RedstoneTorchBlock burnout: 8 turn-offs within 60 game ticks, retry after
// 160.
constexpr int kTorchBurnoutToggles = 8;
constexpr int kTorchToggleWindow = 60;
constexpr int kTorchBurnoutRestartDelay = 160;
// Java's default max-chained-neighbor-updates.
constexpr int kMaxChainedUpdates = 1'000'000;

constexpr std::uint8_t bit(Dir d) {
  return static_cast<std::uint8_t>(1u << index_of(d));
}

bool dust_should_connect_to(const Block &b, Dir toward) {
  if (is_dust(b))
    return true;
  if (connects_along_axis(b))
    return same_axis(toward, b.facing);
  if (b.type == BlockType::Observer)
    return toward == b.facing;
  return is_signal_source(b);
}

} // namespace

Simulator::Simulator(const World &world, Placement placement)
    : world_(world), order_(placement) {
  std::vector<std::string> errors = world.validate();
  if (!errors.empty()) {
    std::string message = "invalid world:";
    for (const std::string &error : errors)
      message += "\n  " + error;
    throw std::invalid_argument(message);
  }

  int volume = world.volume();
  this->states_.resize(volume);
  this->comparator_outputs_.assign(volume, 0);
  this->has_scheduled_.assign(volume, false);
  this->ticks_this_tick_.assign(volume, false);
  this->input_values_.assign(kMaxChannels, 0);
  this->input_cells_.assign(kMaxChannels, -1);
  this->output_cells_.assign(kMaxChannels, -1);

  for (int c = 0; c < volume; ++c) {
    const Block &b = this->block(world.position_of(c));
    if (b.type == BlockType::Torch)
      this->states_[c].on = true;
    if (b.type == BlockType::Lever)
      this->states_[c].on = b.value != 0;
    if (b.type == BlockType::Input)
      this->input_cells_[b.channel] = c;
    if (b.type == BlockType::Output)
      this->output_cells_[b.channel] = c;
  }
  this->compute_dust_connections();

  // Let every block react to its surroundings once, as if it had just been
  // placed.
  for (int c = 0; c < volume; ++c)
    this->enqueue({NeighborUpdate::Kind::Single, world.position_of(c)});
}

// ---------------------------------------------------------------------------------------------
// Public interface

void Simulator::use(Vec3 p) {
  if (!this->in_bounds(p))
    throw std::invalid_argument("nothing to use outside the world");
  const Block &b = this->block(p);
  CellState s = this->states_[this->cell(p)];
  if (b.type == BlockType::Lever) {
    s.on = !s.on;
  } else if (b.type == BlockType::Button) {
    if (s.on)
      return; // already pressed
    s.on = true;
    this->schedule(p, kButtonPressTicks);
  } else {
    throw std::invalid_argument("only levers and buttons can be used");
  }
  // LeverBlock/ButtonBlock.updateNeighbours: around itself and around the
  // block it is attached to.
  this->set_state(p, s, true);
  this->update_neighbors_at(p + offset(b.facing));
}

void Simulator::set_input(int channel, int signal) {
  if (channel < 0 || channel >= kMaxChannels || this->input_cells_[channel] < 0)
    throw std::invalid_argument(
        std::format("no input with channel {}", channel));
  if (signal < 0 || signal > kMaxSignal)
    throw std::invalid_argument(
        std::format("input signal {} is outside 0..{}", signal, kMaxSignal));
  if (this->input_values_[channel] == signal)
    return;

  this->input_values_[channel] = static_cast<std::uint8_t>(signal);
  Vec3 p = this->world_.position_of(this->input_cells_[channel]);
  const Block &b = this->block(p);
  if (is_diode_input(b)) {
    CellState next = this->states_[this->cell(p)];
    next.on = signal > 0;
    this->set_state(p, next, false);
    this->update_in_front(p, b.facing);
    return;
  }
  if (is_dust(b)) {
    // Like dust whose power changed: update around it and its neighbours.
    CellState next = this->states_[this->cell(p)];
    next.power = static_cast<std::uint8_t>(signal);
    this->set_state(p, next, false);
    for (Vec3 q : this->order_.dust_update_positions(p))
      this->update_neighbors_at(q);
    return;
  }
  this->update_comparators_around_container(p);
}

int Simulator::input(int channel) const {
  if (channel < 0 || channel >= kMaxChannels || this->input_cells_[channel] < 0)
    throw std::invalid_argument(
        std::format("no input with channel {}", channel));
  return this->input_values_[channel];
}

int Simulator::output(int channel) const {
  if (channel < 0 || channel >= kMaxChannels ||
      this->output_cells_[channel] < 0)
    throw std::invalid_argument(
        std::format("no output with channel {}", channel));
  Vec3 p = this->world_.position_of(this->output_cells_[channel]);
  const Block &b = this->block(p);
  if (is_dust(b))
    return this->dust_power(p);
  return is_output(b, OutputKind::Comparator) ? this->comparator_input(p)
                                              : this->diode_input(p);
}

void Simulator::tick() {
  ++this->time_;
  std::vector<int> due;
  while (!this->scheduled_.empty() &&
         this->scheduled_.top().time <= this->time_) {
    int c = this->scheduled_.top().cell;
    this->scheduled_.pop();
    this->has_scheduled_[c] = false;
    this->ticks_this_tick_[c] = true;
    due.push_back(c);
  }
  for (int c : due) {
    this->ticks_this_tick_[c] = false;
    this->run_scheduled_tick(c);
  }
}

bool Simulator::run_until_quiet(int max_ticks) {
  for (int i = 0; i < max_ticks && this->has_pending_ticks(); ++i)
    this->tick();
  return !this->has_pending_ticks();
}

CellState Simulator::state_at(Vec3 p) const {
  if (!this->in_bounds(p))
    return {};
  return this->states_[this->cell(p)];
}

bool Simulator::dust_points(Vec3 p, Dir d) const {
  if (!this->in_bounds(p) || !is_dust(this->block(p)) || !is_horizontal(d))
    return false;
  return (this->dust_sides_[this->cell(p)] & bit(d)) != 0;
}

int Simulator::comparator_output_at(Vec3 p) const {
  if (!this->in_bounds(p))
    return 0;
  return this->comparator_outputs_[this->cell(p)];
}

// ---------------------------------------------------------------------------------------------
// Dust shape (static, since blocks never move)

void Simulator::compute_dust_connections() {
  this->dust_sides_.assign(this->world_.volume(), 0);
  constexpr std::uint8_t kNorthSouth = bit(Dir::North) | bit(Dir::South);
  constexpr std::uint8_t kEastWest = bit(Dir::East) | bit(Dir::West);

  for (int c = 0; c < this->world_.volume(); ++c) {
    Vec3 p = this->world_.position_of(c);
    if (!is_dust(this->block(p)))
      continue;

    bool can_go_up = !is_conductor(this->block(p + offset(Dir::Up)));
    std::uint8_t sides = 0;
    for (Dir d : kHorizontalDirs)
      if (this->dust_connects_to_side(p, d, can_go_up))
        sides |= bit(d);

    // RedStoneWireBlock.getConnectionState: a missing axis is filled in, so
    // lone dust is a cross and a single connection becomes a straight line.
    std::uint8_t filled = sides;
    if ((sides & kNorthSouth) == 0)
      filled |= kEastWest;
    if ((sides & kEastWest) == 0)
      filled |= kNorthSouth;
    this->dust_sides_[c] = filled;
  }
}

bool Simulator::dust_connects_to_side(Vec3 p, Dir d, bool can_go_up) const {
  Vec3 side = p + offset(d);
  const Block &side_block = this->block(side);
  if (can_go_up && supports_on_top(side_block) &&
      is_dust(this->block(side + offset(Dir::Up))))
    return true;
  return dust_should_connect_to(side_block, d) ||
         (!is_conductor(side_block) &&
          is_dust(this->block(side + offset(Dir::Down))));
}

// ---------------------------------------------------------------------------------------------
// Signal queries

int Simulator::dust_power(Vec3 p) const {
  if (!this->in_bounds(p) || !is_dust(this->block(p)))
    return 0;
  return this->states_[this->cell(p)].power;
}

int Simulator::weak_emit(Vec3 p, Dir d, bool dust_signals) const {
  if (!this->in_bounds(p))
    return 0;
  const Block &b = this->block(p);
  int c = this->cell(p);
  const CellState &s = this->states_[c];

  if (is_dust(b)) {
    if (!dust_signals || s.power == 0 || d == Dir::Up)
      return 0;
    if (d == Dir::Down)
      return s.power;
    return (this->dust_sides_[c] & bit(d)) != 0 ? s.power : 0;
  }
  switch (b.type) {
  case BlockType::Torch:
    return s.on && d != b.facing ? kMaxSignal : 0;
  case BlockType::Lever:
  case BlockType::Button:
    return s.on ? kMaxSignal : 0;
  case BlockType::RedstoneBlock:
    return kMaxSignal;
  case BlockType::Repeater:
    return s.on && d == b.facing ? kMaxSignal : 0;
  case BlockType::Comparator:
    return d == b.facing ? this->comparator_outputs_[c] : 0;
  case BlockType::Observer:
    return s.on && d == opposite(b.facing) ? kMaxSignal : 0;
  case BlockType::Input:
    if (!is_diode_input(b) || d != b.facing)
      return 0;
    if (is_input(b, InputKind::Repeater))
      return this->input_value_at(p) > 0 ? kMaxSignal : 0;
    return this->input_value_at(p);
  default:
    return 0;
  }
}

int Simulator::strong_emit(Vec3 p, Dir d, bool dust_signals) const {
  if (!this->in_bounds(p))
    return 0;
  const Block &b = this->block(p);
  if (b.type == BlockType::Torch)
    return this->states_[this->cell(p)].on && d == Dir::Up ? kMaxSignal : 0;
  if (b.type == BlockType::RedstoneBlock)
    return 0;
  if (b.type == BlockType::Lever || b.type == BlockType::Button)
    return this->states_[this->cell(p)].on && d == b.facing ? kMaxSignal : 0;
  return this->weak_emit(p, d, dust_signals);
}

int Simulator::strong_power_into(Vec3 p, bool dust_signals) const {
  int best = 0;
  for (Dir k : kAllDirs)
    best = std::max(
        best, this->strong_emit(p + offset(k), opposite(k), dust_signals));
  return best;
}

int Simulator::signal(Vec3 p, Dir d, bool dust_signals) const {
  int emitted = this->weak_emit(p, d, dust_signals);
  if (!is_conductor(this->block(p)))
    return emitted;
  return std::max(emitted, this->strong_power_into(p, dust_signals));
}

int Simulator::best_neighbor_signal(Vec3 p, bool dust_signals) const {
  int best = 0;
  for (Dir k : kAllDirs)
    best =
        std::max(best, this->signal(p + offset(k), opposite(k), dust_signals));
  return best;
}

bool Simulator::has_neighbor_signal(Vec3 p) const {
  for (Dir k : kAllDirs)
    if (this->signal(p + offset(k), opposite(k), true) > 0)
      return true;
  return false;
}

int Simulator::dust_target_strength(Vec3 p) const {
  int from_sources = this->best_neighbor_signal(p, false);
  if (from_sources >= kMaxSignal)
    return from_sources;

  bool covered = is_conductor(this->block(p + offset(Dir::Up)));
  int from_dust = 0;
  for (Dir d : kHorizontalDirs) {
    Vec3 side = p + offset(d);
    bool side_conducts = is_conductor(this->block(side));
    from_dust = std::max(from_dust, this->dust_power(side));
    if (side_conducts && !covered)
      from_dust = std::max(from_dust, this->dust_power(side + offset(Dir::Up)));
    else if (!side_conducts)
      from_dust =
          std::max(from_dust, this->dust_power(side + offset(Dir::Down)));
  }
  return std::max(from_sources, from_dust - 1);
}

bool Simulator::torch_powered(Vec3 p) const {
  Dir attached = this->block(p).facing;
  return this->signal(p + offset(attached), opposite(attached), true) > 0;
}

int Simulator::diode_input(Vec3 p) const {
  Dir facing = this->block(p).facing;
  Vec3 behind = p + offset(opposite(facing));
  int input = this->signal(behind, facing, true);
  if (input >= kMaxSignal)
    return input;
  return std::max(input, this->dust_power(behind));
}

int Simulator::control_signal(Vec3 side, Dir toward_diode,
                              bool diodes_only) const {
  const Block &b = this->block(side);
  if (diodes_only)
    return is_diode(b) ? this->strong_emit(side, toward_diode, true) : 0;
  if (b.type == BlockType::RedstoneBlock)
    return kMaxSignal;
  if (is_dust(b))
    return this->dust_power(side);
  return is_signal_source(b) ? this->strong_emit(side, toward_diode, true) : 0;
}

int Simulator::side_signal(Vec3 p, bool diodes_only) const {
  Dir facing = this->block(p).facing;
  Dir right = rotate_clockwise(facing);
  Dir left = rotate_counter_clockwise(facing);
  return std::max(
      this->control_signal(p + offset(right), opposite(right), diodes_only),
      this->control_signal(p + offset(left), opposite(left), diodes_only));
}

int Simulator::comparator_input(Vec3 p) const {
  Dir back = opposite(this->block(p).facing);
  int input = this->diode_input(p);
  Vec3 behind = p + offset(back);
  const Block &behind_block = this->block(behind);
  if (is_container(behind_block))
    return this->input_value_at(behind);
  if (input < kMaxSignal && is_conductor(behind_block)) {
    Vec3 further = behind + offset(back);
    if (is_container(this->block(further)))
      return this->input_value_at(further);
  }
  return input;
}

int Simulator::comparator_compute_output(Vec3 p) const {
  int input = this->comparator_input(p);
  if (input == 0)
    return 0;
  int side = this->side_signal(p, false);
  if (side > input)
    return 0;
  return this->block(p).comparator_mode() == ComparatorMode::Subtract
             ? input - side
             : input;
}

bool Simulator::comparator_should_be_on(Vec3 p) const {
  int input = this->comparator_input(p);
  if (input == 0)
    return false;
  int side = this->side_signal(p, false);
  return input > side || (input == side && this->block(p).comparator_mode() ==
                                               ComparatorMode::Compare);
}

bool Simulator::diode_should_prioritize(Vec3 p) const {
  Dir facing = this->block(p).facing;
  const Block &front = this->block(p + offset(facing));
  return is_diode(front) && front.facing != opposite(facing);
}

// Analog (comparator) signal of a container, or an input's current value.
int Simulator::input_value_at(Vec3 p) const {
  const Block &b = this->block(p);
  if (b.type == BlockType::Input)
    return this->input_values_[b.channel];
  return b.stored_signal();
}

// ---------------------------------------------------------------------------------------------
// State changes and update propagation (Level.setBlock +
// CollectingNeighborUpdater)

void Simulator::set_state(Vec3 p, CellState next, bool notify_neighbors) {
  int c = this->cell(p);
  if (this->states_[c] == next)
    return;
  this->states_[c] = next;

  this->on_state_replaced(p);
  if (notify_neighbors)
    this->update_neighbors_at(p);
  for (Dir d : this->order_.shape_order())
    this->enqueue({NeighborUpdate::Kind::Shape, p + offset(d), opposite(d)});
}

// Block.onPlace hooks that also fire when only the state changes.
void Simulator::on_state_replaced(Vec3 p) {
  const Block &b = this->block(p);
  switch (b.type) {
  case BlockType::Torch:
    for (Dir d : this->order_.values_order())
      this->update_neighbors_at(p + offset(d));
    break;
  case BlockType::Repeater:
  case BlockType::Comparator:
    this->update_in_front(p, b.facing);
    break;
  default:
    break;
  }
}

void Simulator::enqueue(NeighborUpdate update) {
  if (this->chained_updates_ >= kMaxChainedUpdates)
    throw SimulationError(std::format("more than {} chained neighbour updates",
                                      kMaxChainedUpdates));
  bool running = this->chained_updates_ > 0;
  ++this->chained_updates_;
  if (running) {
    this->updates_added_.push_back(update);
    return;
  }
  this->update_stack_.push_back(update);
  this->run_updates();
}

void Simulator::run_updates() {
  while (!this->update_stack_.empty() || !this->updates_added_.empty()) {
    for (auto it = this->updates_added_.rbegin();
         it != this->updates_added_.rend(); ++it)
      this->update_stack_.push_back(*it);
    this->updates_added_.clear();

    // Handlers only append to updates_added_, so the reference to the top stays
    // valid.
    while (this->updates_added_.empty()) {
      if (!this->run_next(this->update_stack_.back())) {
        this->update_stack_.pop_back();
        break;
      }
    }
  }
  this->chained_updates_ = 0;
}

bool Simulator::run_next(NeighborUpdate &update) {
  switch (update.kind) {
  case NeighborUpdate::Kind::Single:
    this->neighbor_changed(update.pos);
    return false;
  case NeighborUpdate::Kind::Shape:
    this->shape_changed(update.pos, update.dir);
    return false;
  case NeighborUpdate::Kind::Multi:
    break;
  }

  const std::array<Dir, kDirCount> &order = this->order_.neighbor_order();
  if (update.has_skip && order[update.next] == update.dir)
    ++update.next;
  Dir d = order[update.next++];
  this->neighbor_changed(update.pos + offset(d));
  if (update.has_skip && update.next < kDirCount &&
      order[update.next] == update.dir)
    ++update.next;
  return update.next < kDirCount;
}

void Simulator::update_neighbors_except(Vec3 p, Dir skip) {
  this->enqueue({NeighborUpdate::Kind::Multi, p, skip, true});
}

// DiodeBlock/ObserverBlock.updateNeighborsInFront.
void Simulator::update_in_front(Vec3 p, Dir output) {
  Vec3 front = p + offset(output);
  this->enqueue({NeighborUpdate::Kind::Single, front});
  this->update_neighbors_except(front, opposite(output));
}

// Level.updateNeighbourForOutputSignal.
void Simulator::update_comparators_around_container(Vec3 p) {
  for (Dir d : this->order_.horizontal_order()) {
    Vec3 side = p + offset(d);
    const Block &side_block = this->block(side);
    if (side_block.type == BlockType::Comparator) {
      this->enqueue({NeighborUpdate::Kind::Single, side});
    } else if (is_conductor(side_block)) {
      Vec3 further = side + offset(d);
      if (this->block(further).type == BlockType::Comparator)
        this->enqueue({NeighborUpdate::Kind::Single, further});
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Block behaviour

void Simulator::neighbor_changed(Vec3 p) {
  if (!this->in_bounds(p))
    return;
  const Block &b = this->block(p);
  if (is_dust(b)) {
    this->update_dust(p);
    return;
  }

  int c = this->cell(p);
  CellState s = this->states_[c];
  switch (b.type) {
  case BlockType::Torch:
    if (s.on == this->torch_powered(p) && !this->ticks_this_tick_[c])
      this->schedule(p, kTorchDelay);
    break;
  case BlockType::Repeater:
    this->repeater_check_tick(p);
    break;
  case BlockType::Comparator:
    this->comparator_check_tick(p);
    break;
  case BlockType::Lamp: {
    // RedstoneLampBlock: lights at once, goes dark a few ticks later.
    bool powered = this->has_neighbor_signal(p);
    if (s.on && !powered) {
      this->schedule(p, kLampOffDelay);
    } else if (!s.on && powered) {
      s.on = true;
      this->set_state(p, s, false);
    }
    break;
  }
  case BlockType::NoteBlock:
  case BlockType::Hopper: {
    bool powered = this->has_neighbor_signal(p);
    if (powered == s.on)
      break;
    s.on = powered;
    this->set_state(p, s, b.type == BlockType::NoteBlock);
    break;
  }
  default:
    break;
  }
}

void Simulator::shape_changed(Vec3 p, Dir toward_changed) {
  if (!this->in_bounds(p))
    return;
  const Block &b = this->block(p);
  int c = this->cell(p);
  CellState s = this->states_[c];

  if (b.type == BlockType::Observer) {
    if (b.facing == toward_changed && !s.on && !this->has_scheduled_[c])
      this->schedule(p, kObserverDelay);
    return;
  }
  if (b.type == BlockType::Repeater && !same_axis(toward_changed, b.facing)) {
    bool locked = this->repeater_locked(p);
    if (locked == s.locked)
      return;
    s.locked = locked;
    this->set_state(p, s, false);
  }
}

// DefaultRedstoneController.updatePowerStrength.
void Simulator::update_dust(Vec3 p) {
  if (is_input(this->block(p), InputKind::Dust))
    return; // held at the input value
  int target = this->dust_target_strength(p);
  CellState s = this->states_[this->cell(p)];
  if (s.power == target)
    return;

  s.power = static_cast<std::uint8_t>(target);
  this->set_state(p, s, false);
  for (Vec3 q : this->order_.dust_update_positions(p))
    this->update_neighbors_at(q);
}

void Simulator::repeater_check_tick(Vec3 p) {
  if (this->repeater_locked(p))
    return;
  int c = this->cell(p);
  bool on = this->states_[c].on;
  bool should_be_on = this->diode_input(p) > 0;
  if (on == should_be_on || this->ticks_this_tick_[c])
    return;

  TickPriority priority = TickPriority::High;
  if (this->diode_should_prioritize(p))
    priority = TickPriority::ExtremelyHigh;
  else if (on)
    priority = TickPriority::VeryHigh;
  this->schedule(p, this->block(p).repeater_delay() * kGameTicksPerRepeaterStep,
                 priority);
}

void Simulator::comparator_check_tick(Vec3 p) {
  int c = this->cell(p);
  if (this->ticks_this_tick_[c])
    return;
  bool output_changes =
      this->comparator_compute_output(p) != this->comparator_outputs_[c];
  if (!output_changes &&
      this->states_[c].on == this->comparator_should_be_on(p))
    return;
  TickPriority priority = this->diode_should_prioritize(p)
                              ? TickPriority::High
                              : TickPriority::Normal;
  this->schedule(p, kComparatorDelay, priority);
}

// ComparatorBlock.refreshOutputState.
void Simulator::comparator_refresh(Vec3 p) {
  int c = this->cell(p);
  int output = this->comparator_compute_output(p);
  int previous = this->comparator_outputs_[c];
  this->comparator_outputs_[c] = static_cast<std::uint8_t>(output);
  const Block &b = this->block(p);
  if (previous == output && b.comparator_mode() != ComparatorMode::Compare)
    return;

  CellState s = this->states_[c];
  bool should_be_on = this->comparator_should_be_on(p);
  if (s.on != should_be_on) {
    s.on = should_be_on;
    this->set_state(p, s, false);
  }
  this->update_in_front(p, b.facing);
}

void Simulator::run_scheduled_tick(int cell) {
  Vec3 p = this->world_.position_of(cell);
  switch (this->block(p).type) {
  case BlockType::Torch:
    this->torch_tick(p);
    break;
  case BlockType::Repeater:
    this->repeater_tick(p);
    break;
  case BlockType::Comparator:
    this->comparator_refresh(p);
    break;
  case BlockType::Observer:
    this->observer_tick(p);
    break;
  case BlockType::Lamp: {
    CellState s = this->states_[cell];
    if (s.on && !this->has_neighbor_signal(p)) {
      s.on = false;
      this->set_state(p, s, false);
    }
    break;
  }
  case BlockType::Button: {
    CellState s = this->states_[cell];
    s.on = false;
    this->set_state(p, s, true);
    this->update_neighbors_at(p + offset(this->block(p).facing));
    break;
  }
  default:
    break;
  }
}

void Simulator::torch_tick(Vec3 p) {
  int c = this->cell(p);
  bool powered = this->torch_powered(p);
  while (!this->torch_toggles_.empty() &&
         this->time_ - this->torch_toggles_.front().time > kTorchToggleWindow)
    this->torch_toggles_.pop_front();

  CellState s = this->states_[c];
  if (s.on) {
    if (!powered)
      return;
    s.on = false;
    this->set_state(p, s, true);
    if (this->torch_toggled_too_often(c, true))
      this->schedule(p, kTorchBurnoutRestartDelay);
    return;
  }
  if (!powered && !this->torch_toggled_too_often(c, false)) {
    s.on = true;
    this->set_state(p, s, true);
  }
}

bool Simulator::torch_toggled_too_often(int cell, bool record) {
  if (record)
    this->torch_toggles_.push_back({cell, this->time_});
  int count = 0;
  for (const TorchToggle &toggle : this->torch_toggles_)
    if (toggle.cell == cell && ++count >= kTorchBurnoutToggles)
      return true;
  return false;
}

void Simulator::repeater_tick(Vec3 p) {
  if (this->repeater_locked(p))
    return;
  int c = this->cell(p);
  CellState s = this->states_[c];
  bool should_be_on = this->diode_input(p) > 0;
  if (s.on && !should_be_on) {
    s.on = false;
    this->set_state(p, s, false);
  } else if (!s.on) {
    s.on = true;
    this->set_state(p, s, false);
    if (!should_be_on)
      this->schedule(
          p, this->block(p).repeater_delay() * kGameTicksPerRepeaterStep,
          TickPriority::VeryHigh);
  }
}

void Simulator::observer_tick(Vec3 p) {
  int c = this->cell(p);
  CellState s = this->states_[c];
  s.on = !s.on;
  this->set_state(p, s, false);
  if (s.on)
    this->schedule(p, kObserverDelay);
  this->update_in_front(p, opposite(this->block(p).facing));
}

void Simulator::schedule(Vec3 p, int delay, TickPriority priority) {
  int c = this->cell(p);
  if (this->has_scheduled_[c])
    return;
  this->has_scheduled_[c] = true;
  this->scheduled_.push(
      {this->time_ + delay, priority, this->tick_order_++, c});
}

} // namespace rs
