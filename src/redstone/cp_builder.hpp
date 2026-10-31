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

// An integer linear expression with known bounds; constant when lo == hi.
class Int {
public:
  Int() = default;
  Int(sat::LinearExpr expr, int lo, int hi)
      : expr_(std::move(expr)), lo_(lo), hi_(hi) {}
  static Int constant(int value) {
    return {sat::LinearExpr(value), value, value};
  }

  bool is_constant() const { return this->lo_ == this->hi_; }
  int lo() const { return this->lo_; }
  int hi() const { return this->hi_; }
  const sat::LinearExpr &expr() const { return this->expr_; }

  Int operator+(const Int &other) const;
  Int operator-(const Int &other) const;
  Int plus(int offset) const;
  // Narrows the bounds when the caller knows more than interval arithmetic.
  Int clamped(int lo, int hi) const;

private:
  sat::LinearExpr expr_{0};
  int lo_ = 0;
  int hi_ = 0;
};

// Builds reified helper constraints, folding constants so impossible cases
// add nothing to the model.
class Builder {
public:
  explicit Builder(sat::CpModelBuilder &model);

  sat::CpModelBuilder &model() { return this->model_; }

  Lit constant(bool value) const;
  Lit new_bool();
  Int new_int(int lo, int hi);
  // value * lit
  Int scaled(Lit lit, int value);

  Lit all(std::vector<Lit> lits);
  Lit any(std::vector<Lit> lits);
  // OR of literals of which at most one can be true (a linear sum).
  Lit any_exclusive(const std::vector<Lit> &lits);
  Lit exclusive_or(Lit a, Lit b);
  Lit equal(Lit a, Lit b) { return !this->exclusive_or(a, b); }
  Lit positive(const Int &x); // x > 0, for x >= 0
  Lit greater(const Int &a, const Int &b);
  Lit not_equal(const Int &a, const Int &b);
  Lit equals_value(const Int &x, int value);

  Int select(Lit condition, const Int &x); // condition ? x : 0
  Int if_else(Lit condition, const Int &yes, const Int &no);
  Int max_of(std::vector<Int> xs);
  // Sum of terms of which at most one is non-zero.
  static Int exclusive_sum(const std::vector<Int> &terms);

  // The `when` literals form a conjunction; constants are folded.
  void require(Lit lit, const std::vector<Lit> &when = {});
  void require_equal(const Int &a, const Int &b,
                     const std::vector<Lit> &when = {});
  void require_equal(Lit a, Lit b, const std::vector<Lit> &when = {});
  void require_in_range(const Int &x, int lo, int hi,
                        const std::vector<Lit> &when = {});
  // At least one of `lits` holds.
  void require_any(const std::vector<Lit> &lits);

private:
  // Returns false if some literal is constant false (constraint vacuous).
  bool enforcement(const std::vector<Lit> &when,
                   std::vector<sat::BoolVar> &out) const;

  sat::CpModelBuilder &model_;
  Lit true_;
  Lit false_;
  std::map<std::vector<int>, Lit> and_cache_;
};

} // namespace rs::cp
