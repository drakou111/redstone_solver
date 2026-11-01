#include "redstone/cp_builder.hpp"

#include <algorithm>

namespace rs::cp {

namespace {

int dimacs(const sat::BoolVar &var) {
  int index = var.index();
  return index >= 0 ? index + 1 : index; // Not(i) has index -i-1
}

} // namespace

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

Lit Int::ge(int k) const {
  if (k <= this->lo_)
    return {sat::BoolVar(), true};
  if (k > this->hi())
    return {sat::BoolVar(), false};
  return this->ge_[k - this->lo_ - 1];
}

Builder::Builder(sat::CpModelBuilder &model, bool cp_constraints)
    : model_(model), cp_constraints_(cp_constraints) {
  this->true_ = Lit(sat::BoolVar(), true);
  this->false_ = Lit(sat::BoolVar(), false);
}

Lit Builder::constant(bool value) const {
  return value ? this->true_ : this->false_;
}

Lit Builder::new_bool() {
  ++this->variables_;
  return {this->model_.NewBoolVar(), std::nullopt};
}

void Builder::clause(const std::vector<Lit> &lits) {
  std::vector<sat::BoolVar> vars;
  std::vector<int> ints;
  for (const Lit &lit : lits) {
    if (lit.is_true())
      return;
    if (lit.is_false())
      continue;
    vars.push_back(lit.var());
    ints.push_back(dimacs(lit.var()));
  }
  if (this->cp_constraints_)
    this->model_.AddBoolOr(vars);
  this->clauses_.push_back(std::move(ints));
}

Int Builder::new_int(int lo, int hi) {
  std::vector<Lit> levels;
  for (int k = lo + 1; k <= hi; ++k) {
    levels.push_back(this->new_bool());
    if (levels.size() >= 2)
      this->clause({!levels.back(), levels[levels.size() - 2]});
  }
  return {lo, std::move(levels)};
}

Int Builder::scaled(Lit lit, int value) {
  if (lit.is_constant() || value == 0)
    return Int::constant(lit.is_true() ? value : 0);
  return {0, std::vector<Lit>(value, lit)};
}

Lit Builder::all(std::vector<Lit> lits) {
  std::vector<int> key;
  std::vector<Lit> vars;
  for (const Lit &lit : lits) {
    if (lit.is_false())
      return this->false_;
    if (lit.is_true())
      continue;
    key.push_back(lit.var().index());
    vars.push_back(lit);
  }
  if (vars.empty())
    return this->true_;
  if (vars.size() == 1)
    return vars[0];

  std::sort(key.begin(), key.end());
  key.erase(std::unique(key.begin(), key.end()), key.end());
  for (int k : key)
    if (std::binary_search(key.begin(), key.end(), -k - 1))
      return this->false_;
  if (auto it = this->and_cache_.find(key); it != this->and_cache_.end())
    return it->second;

  Lit result = this->new_bool();
  std::vector<Lit> back{result};
  for (const Lit &lit : vars) {
    this->clause({!result, lit});
    back.push_back(!lit);
  }
  this->clause(back);
  this->and_cache_.emplace(std::move(key), result);
  return result;
}

Lit Builder::any(std::vector<Lit> lits) {
  for (Lit &lit : lits)
    lit = !lit;
  return !this->all(std::move(lits));
}

Lit Builder::exclusive_or(Lit a, Lit b) {
  if (a.is_constant())
    return a.is_true() ? !b : b;
  if (b.is_constant())
    return b.is_true() ? !a : a;
  if (a.var().index() == b.var().index())
    return this->false_;
  Lit result = this->new_bool();
  this->clause({!result, a, b});
  this->clause({!result, !a, !b});
  this->clause({result, !a, b});
  this->clause({result, a, !b});
  return result;
}

Lit Builder::greater(const Int &a, const Int &b) {
  std::vector<Lit> ways;
  for (int k = a.lo() + 1; k <= a.hi(); ++k)
    ways.push_back(this->all({a.ge(k), !b.ge(k)}));
  if (b.lo() < a.lo())
    ways.push_back(!b.ge(a.lo())); // a >= a.lo always
  return this->any(ways);
}

Lit Builder::not_equal(const Int &a, const Int &b) {
  return this->any({this->greater(a, b), this->greater(b, a)});
}

Lit Builder::equals_value(const Int &x, int value) {
  return this->all({x.ge(value), !x.ge(value + 1)});
}

Int Builder::select(Lit condition, const Int &x) {
  if (condition.is_false())
    return Int::constant(0);
  if (condition.is_true())
    return x;
  int lo = std::min(0, x.lo());
  int hi = std::max(0, x.hi());
  std::vector<Lit> levels;
  for (int k = lo + 1; k <= hi; ++k)
    levels.push_back(k <= 0 ? this->any({!condition, x.ge(k)})
                            : this->all({condition, x.ge(k)}));
  return {lo, std::move(levels)};
}

Int Builder::if_else(Lit condition, const Int &yes, const Int &no) {
  if (condition.is_true())
    return yes;
  if (condition.is_false())
    return no;
  int lo = std::min(yes.lo(), no.lo());
  int hi = std::max(yes.hi(), no.hi());
  std::vector<Lit> levels;
  for (int k = lo + 1; k <= hi; ++k)
    levels.push_back(this->any({this->all({condition, yes.ge(k)}),
                                this->all({!condition, no.ge(k)})}));
  return {lo, std::move(levels)};
}

Int Builder::max_of(const std::vector<Int> &xs) {
  if (xs.empty())
    return Int::constant(0);
  int lo = xs[0].lo();
  int hi = xs[0].hi();
  for (const Int &x : xs) {
    lo = std::max(lo, x.lo());
    hi = std::max(hi, x.hi());
  }
  std::vector<Lit> levels;
  for (int k = lo + 1; k <= hi; ++k) {
    std::vector<Lit> any;
    for (const Int &x : xs)
      any.push_back(x.ge(k));
    levels.push_back(this->any(std::move(any)));
  }
  return {lo, std::move(levels)};
}

Int Builder::count_for_upper_bound(const std::vector<Lit> &lits) {
  std::vector<Int> layer;
  int constant = 0;
  for (const Lit &lit : lits) {
    if (lit.is_constant())
      constant += lit.is_true() ? 1 : 0;
    else
      layer.push_back(Int(0, {lit}));
  }
  if (layer.empty())
    return Int::constant(constant);
  while (layer.size() > 1) {
    std::vector<Int> next;
    for (std::size_t i = 0; i + 1 < layer.size(); i += 2) {
      const Int &a = layer[i];
      const Int &b = layer[i + 1];
      Int sum = this->new_int(0, a.hi() + b.hi());
      for (int i_a = 0; i_a <= a.hi(); ++i_a)
        for (int i_b = 0; i_b <= b.hi(); ++i_b)
          if (i_a + i_b > 0)
            this->require(sum.ge(i_a + i_b), {a.ge(i_a), b.ge(i_b)});
      next.push_back(std::move(sum));
    }
    if (layer.size() % 2 == 1)
      next.push_back(layer.back());
    layer = std::move(next);
  }
  return layer[0].plus(constant);
}

void Builder::exactly_one(const std::vector<Lit> &lits) {
  this->clause(lits);
  for (std::size_t i = 0; i < lits.size(); ++i)
    for (std::size_t j = i + 1; j < lits.size(); ++j)
      this->clause({!lits[i], !lits[j]});
}

void Builder::require(Lit lit, const std::vector<Lit> &when) {
  std::vector<Lit> c;
  for (const Lit &w : when)
    c.push_back(!w);
  c.push_back(lit);
  this->clause(c);
}

void Builder::require_equal(const Int &a, const Int &b,
                            const std::vector<Lit> &when) {
  int lo = std::min(a.lo(), b.lo());
  int hi = std::max(a.hi(), b.hi());
  for (int k = lo + 1; k <= hi; ++k) {
    this->require(b.ge(k), [&] {
      std::vector<Lit> w = when;
      w.push_back(a.ge(k));
      return w;
    }());
    this->require(a.ge(k), [&] {
      std::vector<Lit> w = when;
      w.push_back(b.ge(k));
      return w;
    }());
  }
}

void Builder::require_equal(Lit a, Lit b, const std::vector<Lit> &when) {
  std::vector<Lit> w = when;
  w.push_back(a);
  this->require(b, w);
  w.back() = b;
  this->require(a, w);
}

void Builder::require_in_range(const Int &x, int lo, int hi,
                               const std::vector<Lit> &when) {
  this->require(x.ge(lo), when);
  this->require(!x.ge(hi + 1), when);
}

void Builder::require_gap(const Int &a, const Int &b, int gap,
                          const std::vector<Lit> &when) {
  for (int k = b.lo(); k <= b.hi(); ++k) {
    std::vector<Lit> w = when;
    w.push_back(b.ge(k));
    this->require(a.ge(k + gap), w);
  }
}

void Builder::require_equal_max(const Int &result,
                                const std::vector<Int> &terms,
                                const std::vector<Lit> &when) {
  int hi = result.hi();
  for (const Int &term : terms)
    hi = std::max(hi, term.hi());
  for (int k = std::max(result.lo(), 0) + 1; k <= hi; ++k) {
    std::vector<Lit> some_term;
    for (const Int &term : terms) {
      std::vector<Lit> w = when;
      w.push_back(term.ge(k));
      this->require(result.ge(k), w);
      some_term.push_back(term.ge(k));
    }
    for (const Lit &w : when)
      some_term.push_back(!w);
    some_term.push_back(!result.ge(k));
    this->require_any(some_term);
  }
}

void Builder::require_difference(const Int &result, const Int &a, const Int &b,
                                 const std::vector<Lit> &when) {
  int lo = std::min(result.lo(), 0) + 1;
  int hi = std::max(result.hi(), a.hi() - b.lo()) + 1;
  for (int k = lo; k <= hi; ++k)
    for (int j = b.lo(); j <= b.hi(); ++j) {
      // a >= j + k and b <= j  ->  result >= k
      std::vector<Lit> w = when;
      w.push_back(a.ge(j + k));
      w.push_back(!b.ge(j + 1));
      this->require(result.ge(k), w);
      // result >= k and b >= j  ->  a >= j + k. Beyond a's range only the
      // smallest such j matters (a larger b implies it).
      if (j + k > a.hi() + 1 && j > b.lo())
        continue;
      w = when;
      w.push_back(result.ge(k));
      w.push_back(b.ge(j));
      this->require(a.ge(j + k), w);
    }
}

void Builder::require_any(const std::vector<Lit> &lits) { this->clause(lits); }

} // namespace rs::cp
