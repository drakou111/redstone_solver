#pragma once

#include <map>
#include <optional>
#include <vector>

#include "ortools/sat/cp_model.h"

namespace rs::cp {

namespace sat = operations_research::sat;

// A boolean that is either a known constant or a model variable.
class Lit {
public:
  Lit() = default;
  Lit(sat::BoolVar var, std::optional<bool> constant)
      : var_(var), constant_(constant) {}

  bool is_constant() const { return this->constant_.has_value(); }
  bool is_true() const { return this->constant_ == true; }
  bool is_false() const { return this->constant_ == false; }
  sat::BoolVar var() const { return this->var_; }
  sat::LinearExpr expr() const;
  Lit operator!() const;

private:
  sat::BoolVar var_;
  std::optional<bool> constant_;
};

// A bounded integer in order encoding: ge(k) is the literal "x >= k".
// Every constraint on it is a clause, so the whole model is plain CNF.
class Int {
public:
  Int() = default;
  static Int constant(int value) { return Int(value, {}); }
  Int(int lo, std::vector<Lit> levels) : lo_(lo), ge_(std::move(levels)) {}

  int lo() const { return this->lo_; }
  int hi() const { return this->lo_ + static_cast<int>(this->ge_.size()); }
  bool is_constant() const { return this->ge_.empty(); }
  // Constant outside the range; callers fold it.
  Lit ge(int k) const;
  Int plus(int offset) const { return {this->lo_ + offset, this->ge_}; }
  const std::vector<Lit> &levels() const { return this->ge_; }

private:
  int lo_ = 0;
  std::vector<Lit> ge_; // ge_[i] means x >= lo + 1 + i
};

// Builds the model as clauses, folding constants so impossible cases add
// nothing.
class Builder {
public:
  // Without `cp_constraints` the clauses are only recorded, not added to the
  // CP-SAT model (variables are still created there for their numbering).
  Builder(sat::CpModelBuilder &model, bool cp_constraints);

  sat::CpModelBuilder &model() { return this->model_; }
  const std::vector<std::vector<int>> &clauses() const {
    return this->clauses_;
  }
  int variable_count() const { return this->variables_; }

  Lit constant(bool value) const;
  Lit new_bool();
  Int new_int(int lo, int hi);
  // value * lit, value >= 0
  Int scaled(Lit lit, int value);

  Lit all(std::vector<Lit> lits);
  Lit any(std::vector<Lit> lits);
  Lit any_exclusive(const std::vector<Lit> &lits) { return this->any(lits); }
  Lit exclusive_or(Lit a, Lit b);
  Lit equal(Lit a, Lit b) { return !this->exclusive_or(a, b); }
  Lit positive(const Int &x) { return x.ge(1); }
  Lit greater(const Int &a, const Int &b);
  Lit not_equal(const Int &a, const Int &b);
  Lit equals_value(const Int &x, int value);

  Int select(Lit condition, const Int &x); // condition ? x : 0
  Int if_else(Lit condition, const Int &yes, const Int &no);
  Int max_of(const std::vector<Int> &xs);
  // Number of true literals, encoded one way only (a totalizer): x >= k is
  // forced once k literals hold, so only upper bounds on x are meaningful.
  Int count_for_upper_bound(const std::vector<Lit> &lits);
  // Sum of non-negative terms of which at most one is non-zero.
  Int exclusive_sum(const std::vector<Int> &terms) {
    return this->max_of(terms);
  }

  void exactly_one(const std::vector<Lit> &lits);
  // The `when` literals form a conjunction; constants are folded.
  void require(Lit lit, const std::vector<Lit> &when = {});
  void require_equal(const Int &a, const Int &b,
                     const std::vector<Lit> &when = {});
  void require_equal(Lit a, Lit b, const std::vector<Lit> &when = {});
  void require_in_range(const Int &x, int lo, int hi,
                        const std::vector<Lit> &when = {});
  // a - b >= gap
  void require_gap(const Int &a, const Int &b, int gap,
                   const std::vector<Lit> &when = {});
  // result == a - b, given a >= b
  void require_difference(const Int &result, const Int &a, const Int &b,
                          const std::vector<Lit> &when = {});
  // At least one of `lits` holds.
  void require_any(const std::vector<Lit> &lits);

private:
  void clause(const std::vector<Lit> &lits);

  sat::CpModelBuilder &model_;
  bool cp_constraints_;
  int variables_ = 0;
  Lit true_;
  Lit false_;
  std::map<std::vector<int>, Lit> and_cache_;
  std::vector<std::vector<int>> clauses_; // DIMACS literals
};

} // namespace rs::cp
