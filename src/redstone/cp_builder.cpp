#include "redstone/cp_builder.hpp"

#include <algorithm>
#include <limits>

namespace rs::cp {

sat::LinearExpr Lit::expr() const {
  if (this->constant_)
    return sat::LinearExpr(*this->constant_ ? 1 : 0);
  return sat::LinearExpr(this->var_);
}

Lit Lit::operator!() const {
  std::optional<bool> negated;
  if (this->constant_)
    negated = !*this->constant_;
  return {this->var_.Not(), negated};
}

Int Int::operator+(const Int &other) const {
  return {this->expr_ + other.expr_, this->lo_ + other.lo_,
          this->hi_ + other.hi_};
}

Int Int::operator-(const Int &other) const {
  return {this->expr_ - other.expr_, this->lo_ - other.hi_,
          this->hi_ - other.lo_};
}

Int Int::plus(int offset) const {
  return {this->expr_ + sat::LinearExpr(offset), this->lo_ + offset,
          this->hi_ + offset};
}

Int Int::clamped(int lo, int hi) const {
  return {this->expr_, std::max(this->lo_, lo), std::min(this->hi_, hi)};
}

Builder::Builder(sat::CpModelBuilder &model) : model_(model) {
  this->true_ = Lit(model.TrueVar(), true);
  this->false_ = Lit(model.FalseVar(), false);
}

Lit Builder::constant(bool value) const {
  return value ? this->true_ : this->false_;
}

Lit Builder::new_bool() { return {this->model_.NewBoolVar(), std::nullopt}; }

Int Builder::new_int(int lo, int hi) {
  if (lo == hi)
    return Int::constant(lo);
  return {sat::LinearExpr(this->model_.NewIntVar({lo, hi})), lo, hi};
}

Int Builder::scaled(Lit lit, int value) {
  if (lit.is_constant() || value == 0)
    return Int::constant(lit.is_true() ? value : 0);
  return {sat::LinearExpr::Term(lit.var(), value), std::min(0, value),
          std::max(0, value)};
}

Lit Builder::all(std::vector<Lit> lits) {
  std::vector<int> key;
  std::vector<sat::BoolVar> vars;
  for (const Lit &lit : lits) {
    if (lit.is_false())
      return this->false_;
    if (lit.is_true())
      continue;
    key.push_back(lit.var().index());
    vars.push_back(lit.var());
  }
  if (vars.empty())
    return this->true_;
  if (vars.size() == 1)
    return {vars[0], std::nullopt};

  std::sort(key.begin(), key.end());
  key.erase(std::unique(key.begin(), key.end()), key.end());
  if (auto it = this->and_cache_.find(key); it != this->and_cache_.end())
    return it->second;

  Lit result = this->new_bool();
  this->model_.AddBoolAnd(vars).OnlyEnforceIf(result.var());
  std::vector<sat::BoolVar> negated;
  negated.reserve(vars.size());
  for (const sat::BoolVar &var : vars)
    negated.push_back(var.Not());
  this->model_.AddBoolOr(negated).OnlyEnforceIf(result.var().Not());
  this->and_cache_.emplace(std::move(key), result);
  return result;
}

Lit Builder::any(std::vector<Lit> lits) {
  for (Lit &lit : lits)
    lit = !lit;
  return !this->all(std::move(lits));
}

Lit Builder::any_exclusive(const std::vector<Lit> &lits) {
  std::vector<sat::BoolVar> vars;
  for (const Lit &lit : lits) {
    if (lit.is_true())
      return this->true_;
    if (!lit.is_false())
      vars.push_back(lit.var());
  }
  if (vars.empty())
    return this->false_;
  if (vars.size() == 1)
    return {vars[0], std::nullopt};
  Lit result = this->new_bool();
  this->model_.AddEquality(result.expr(), sat::LinearExpr::Sum(vars));
  return result;
}

Lit Builder::exclusive_or(Lit a, Lit b) {
  if (a.is_constant())
    return a.is_true() ? !b : b;
  if (b.is_constant())
    return b.is_true() ? !a : a;
  if (a.var().index() == b.var().index())
    return this->false_;
  Lit result = this->new_bool();
  this->model_.AddBoolXor({a.var(), b.var(), result.var().Not()});
  return result;
}

Lit Builder::positive(const Int &x) {
  if (x.hi() <= 0)
    return this->false_;
  if (x.lo() >= 1)
    return this->true_;
  Lit result = this->new_bool();
  this->model_.AddGreaterOrEqual(x.expr(), 1).OnlyEnforceIf(result.var());
  this->model_.AddLessOrEqual(x.expr(), 0).OnlyEnforceIf(result.var().Not());
  return result;
}

Lit Builder::greater(const Int &a, const Int &b) {
  if (a.lo() > b.hi())
    return this->true_;
  if (a.hi() <= b.lo())
    return this->false_;
  Lit result = this->new_bool();
  this->model_.AddGreaterOrEqual(a.expr(), b.expr() + sat::LinearExpr(1))
      .OnlyEnforceIf(result.var());
  this->model_.AddLessOrEqual(a.expr(), b.expr())
      .OnlyEnforceIf(result.var().Not());
  return result;
}

Lit Builder::not_equal(const Int &a, const Int &b) {
  if (a.is_constant() && b.is_constant())
    return this->constant(a.lo() != b.lo());
  if (a.hi() < b.lo() || b.hi() < a.lo())
    return this->true_;
  Lit result = this->new_bool();
  this->model_.AddNotEqual(a.expr(), b.expr()).OnlyEnforceIf(result.var());
  this->model_.AddEquality(a.expr(), b.expr())
      .OnlyEnforceIf(result.var().Not());
  return result;
}

Lit Builder::equals_value(const Int &x, int value) {
  return !this->not_equal(x, Int::constant(value));
}

Int Builder::select(Lit condition, const Int &x) {
  if (condition.is_false())
    return Int::constant(0);
  if (condition.is_true())
    return x;
  if (x.is_constant())
    return this->scaled(condition, x.lo());
  Int result = this->new_int(std::min(0, x.lo()), std::max(0, x.hi()));
  this->model_.AddEquality(result.expr(), x.expr())
      .OnlyEnforceIf(condition.var());
  this->model_.AddEquality(result.expr(), 0)
      .OnlyEnforceIf(condition.var().Not());
  return result;
}

Int Builder::if_else(Lit condition, const Int &yes, const Int &no) {
  if (condition.is_true())
    return yes;
  if (condition.is_false())
    return no;
  return this->select(condition, yes) + this->select(!condition, no);
}

Int Builder::max_of(std::vector<Int> xs) {
  int best_constant = std::numeric_limits<int>::min();
  std::vector<Int> variables;
  for (const Int &x : xs) {
    if (x.is_constant())
      best_constant = std::max(best_constant, x.lo());
    else
      variables.push_back(x);
  }
  // Terms that can never exceed the best constant do not matter.
  std::erase_if(variables,
                [&](const Int &x) { return x.hi() <= best_constant; });
  if (variables.empty())
    return Int::constant(best_constant);
  if (variables.size() == 1 && best_constant <= variables[0].lo())
    return variables[0];

  int lo = best_constant;
  int hi = best_constant;
  std::vector<sat::LinearExpr> exprs;
  for (const Int &x : variables) {
    lo = std::max(lo, x.lo());
    hi = std::max(hi, x.hi());
    exprs.push_back(x.expr());
  }
  if (best_constant != std::numeric_limits<int>::min())
    exprs.push_back(sat::LinearExpr(best_constant));
  Int result = this->new_int(lo, hi);
  this->model_.AddMaxEquality(result.expr(), exprs);
  return result;
}

Int Builder::exclusive_sum(const std::vector<Int> &terms) {
  Int total = Int::constant(0);
  int lo = 0;
  int hi = 0;
  for (const Int &term : terms) {
    total = total + term;
    lo = std::min(lo, term.lo());
    hi = std::max(hi, term.hi());
  }
  return total.clamped(lo, hi);
}

bool Builder::enforcement(const std::vector<Lit> &when,
                          std::vector<sat::BoolVar> &out) const {
  for (const Lit &lit : when) {
    if (lit.is_false())
      return false;
    if (!lit.is_true())
      out.push_back(lit.var());
  }
  return true;
}

void Builder::require(Lit lit, const std::vector<Lit> &when) {
  this->require_equal(lit, this->true_, when);
}

void Builder::require_equal(const Int &a, const Int &b,
                            const std::vector<Lit> &when) {
  std::vector<sat::BoolVar> enforce;
  if (!this->enforcement(when, enforce))
    return;
  this->model_.AddEquality(a.expr(), b.expr()).OnlyEnforceIf(enforce);
}

void Builder::require_equal(Lit a, Lit b, const std::vector<Lit> &when) {
  std::vector<sat::BoolVar> enforce;
  if (!this->enforcement(when, enforce))
    return;
  this->model_.AddEquality(a.expr(), b.expr()).OnlyEnforceIf(enforce);
}

void Builder::require_in_range(const Int &x, int lo, int hi,
                               const std::vector<Lit> &when) {
  std::vector<sat::BoolVar> enforce;
  if (!this->enforcement(when, enforce))
    return;
  this->model_.AddLinearConstraint(x.expr(), {lo, hi}).OnlyEnforceIf(enforce);
}

void Builder::require_any(const std::vector<Lit> &lits) {
  std::vector<sat::BoolVar> vars;
  for (const Lit &lit : lits) {
    if (lit.is_true())
      return;
    if (!lit.is_false())
      vars.push_back(lit.var());
  }
  this->model_.AddBoolOr(vars);
}

} // namespace rs::cp
