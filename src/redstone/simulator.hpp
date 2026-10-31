#pragma once

#include <cstdint>
#include <deque>
#include <queue>
#include <stdexcept>
#include <vector>

#include "redstone/geometry.hpp"
#include "redstone/update_order.hpp"
#include "redstone/world.hpp"

namespace rs {

class SimulationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// The block-state part of a cell. Changing it is what neighbours (observers)
// can see.
struct CellState {
  std::uint8_t power = 0; // dust power
  bool on = false; // torch lit, repeater/comparator/observer/note block/hopper
                   // powered
  bool locked = false; // repeater lock

  constexpr bool operator==(const CellState &) const = default;
};

// Java's TickPriority; lower runs first within a game tick.
enum class TickPriority : std::int8_t {
  ExtremelyHigh = -3,
  VeryHigh = -2,
  High = -1,
  Normal = 0
};

// Game-tick simulation of vanilla Java redstone (classic dust), following the
// game's update rules: neighbour updates through a depth-first collecting
// updater, shape updates, and scheduled ticks ordered by (time, priority,
// scheduling order).
//
// Inputs change between game ticks, as if flipped by a player.
class Simulator {
public:
  explicit Simulator(const World &world, Placement placement = {});

  // Sets an input's signal strength (0..15) and propagates the immediate
  // updates.
  void set_input(int channel, int signal);
  int input(int channel) const;
  int output(int channel) const;

  // Advances one game tick.
  void tick();
  // Ticks until nothing is scheduled. Returns false if still busy after
  // `max_ticks`.
  bool run_until_quiet(int max_ticks);
  bool has_pending_ticks() const { return !this->scheduled_.empty(); }
  std::int64_t time() const { return this->time_; }

  const World &world() const { return this->world_; }
  CellState state_at(Vec3 p) const;
  int comparator_output_at(Vec3 p) const;
  // Signal a component would read from the block at `p` in direction `d` (Java
  // Level.getSignal).
  int signal_from(Vec3 p, Dir d) const { return this->signal(p, d, true); }

private:
  struct NeighborUpdate {
    enum class Kind : std::uint8_t { Single, Multi, Shape };
    Kind kind = Kind::Single;
    Vec3 pos{};          // target (Single/Shape) or centre (Multi)
    Dir dir = Dir::Down; // Shape: from target towards the changed block. Multi:
                         // skipped side.
    bool has_skip = false;
    std::uint8_t next = 0;
  };

  struct ScheduledTick {
    std::int64_t time;
    TickPriority priority;
    std::int64_t order;
    int cell;

    bool operator>(const ScheduledTick &o) const {
      if (this->time != o.time)
        return this->time > o.time;
      if (this->priority != o.priority)
        return this->priority > o.priority;
      return this->order > o.order;
    }
  };

  struct TorchToggle {
    int cell;
    std::int64_t time;
  };

  // --- static helpers
  const Block &block(Vec3 p) const { return this->world_.at(p); }
  bool in_bounds(Vec3 p) const { return this->world_.in_bounds(p); }
  int cell(Vec3 p) const { return this->world_.index_of(p); }
  void compute_dust_connections();
  bool dust_connects_to_side(Vec3 p, Dir d, bool can_go_up) const;

  // --- signal queries (Java getSignal/getDirectSignal family; `d` points from
  // source to receiver)
  int dust_power(Vec3 p) const;
  int weak_emit(Vec3 p, Dir d, bool dust_signals) const;
  int strong_emit(Vec3 p, Dir d, bool dust_signals) const;
  int strong_power_into(Vec3 p, bool dust_signals) const;
  int signal(Vec3 p, Dir d, bool dust_signals) const;
  int best_neighbor_signal(Vec3 p, bool dust_signals) const;
  bool has_neighbor_signal(Vec3 p) const;
  int dust_target_strength(Vec3 p) const;
  bool torch_powered(Vec3 p) const;
  int diode_input(Vec3 p) const;
  int control_signal(Vec3 side, Dir toward_diode, bool diodes_only) const;
  int side_signal(Vec3 p, bool diodes_only) const;
  bool repeater_locked(Vec3 p) const { return this->side_signal(p, true) > 0; }
  int comparator_input(Vec3 p) const;
  int comparator_compute_output(Vec3 p) const;
  bool comparator_should_be_on(Vec3 p) const;
  bool diode_should_prioritize(Vec3 p) const;
  int input_value_at(Vec3 p) const;

  // --- state changes and update propagation
  void set_state(Vec3 p, CellState next, bool notify_neighbors);
  void on_state_replaced(Vec3 p);
  void enqueue(NeighborUpdate update);
  void run_updates();
  bool run_next(NeighborUpdate &update);
  void update_neighbors_at(Vec3 p) {
    this->enqueue({NeighborUpdate::Kind::Multi, p});
  }
  void update_neighbors_except(Vec3 p, Dir skip);
  void update_in_front(Vec3 p, Dir output);
  void update_comparators_around_container(Vec3 p);

  // --- block behaviour
  void neighbor_changed(Vec3 p);
  void shape_changed(Vec3 p, Dir toward_changed);
  void update_dust(Vec3 p);
  void repeater_check_tick(Vec3 p);
  void comparator_check_tick(Vec3 p);
  void comparator_refresh(Vec3 p);
  void run_scheduled_tick(int cell);
  void torch_tick(Vec3 p);
  void repeater_tick(Vec3 p);
  void observer_tick(Vec3 p);
  bool torch_toggled_too_often(int cell, bool record);

  // --- scheduling
  void schedule(Vec3 p, int delay,
                TickPriority priority = TickPriority::Normal);

  World world_;
  UpdateOrder order_;
  std::vector<std::uint8_t>
      dust_sides_; // bit per Dir index: dust powers that horizontal side
  std::vector<CellState> states_;
  std::vector<std::uint8_t> comparator_outputs_;
  std::vector<std::uint8_t> input_values_; // by channel
  std::vector<int> input_cells_;           // by channel, -1 if absent
  std::vector<int> output_cells_;          // by channel, -1 if absent

  std::vector<NeighborUpdate> update_stack_;
  std::vector<NeighborUpdate> updates_added_;
  int chained_updates_ = 0;

  std::priority_queue<ScheduledTick, std::vector<ScheduledTick>, std::greater<>>
      scheduled_;
  std::vector<bool> has_scheduled_;
  std::vector<bool> ticks_this_tick_;
  std::int64_t tick_order_ = 0;
  std::int64_t time_ = 0;
  std::deque<TorchToggle> torch_toggles_;
};

} // namespace rs
