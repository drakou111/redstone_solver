#include "redstone/solve_job.hpp"

namespace rs {

SolveJob::SolveJob(Spec spec, SolverOptions options)
    : spec_(std::move(spec)), options_(options),
      start_(std::chrono::steady_clock::now()) {
  this->options_.cancel = &this->cancel_;
  this->thread_ = std::thread(&SolveJob::run, this);
}

SolveJob::~SolveJob() {
  this->cancel_ = true;
  if (this->thread_.joinable())
    this->thread_.join();
}

double SolveJob::seconds() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       this->start_)
      .count();
}

std::vector<std::string> SolveJob::log() const {
  std::lock_guard lock(this->mutex_);
  return this->log_;
}

std::optional<SolveReport> SolveJob::report() const {
  std::lock_guard lock(this->mutex_);
  return this->report_;
}

std::optional<std::string> SolveJob::error() const {
  std::lock_guard lock(this->mutex_);
  return this->error_;
}

void SolveJob::run() {
  try {
    SolveReport report =
        solve(this->spec_, this->options_, [this](std::string_view message) {
          std::lock_guard lock(this->mutex_);
          this->log_.emplace_back(message);
        });
    std::lock_guard lock(this->mutex_);
    this->report_ = std::move(report);
  } catch (const std::exception &error) {
    // Shown to the user by the UI; the thread must not let it escape.
    std::lock_guard lock(this->mutex_);
    this->error_ = error.what();
  }
  this->done_ = true;
}

} // namespace rs
