#include "redstone/solve_job.hpp"

namespace rs {

SolveJob::SolveJob(Spec spec, SolverOptions options)
    : SolveJob(spec, [spec, options](const JobContext &job) {
        SolverOptions o = options;
        o.cancel = job.cancel;
        o.preview = job.preview;
        return JobResult{solve(spec, o, job.progress), std::nullopt};
      }) {}

SolveJob::SolveJob(Spec spec, JobWork work)
    : spec_(std::move(spec)), work_(std::move(work)),
      start_(std::chrono::steady_clock::now()) {
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

std::optional<Spec> SolveJob::result_spec() const {
  std::lock_guard lock(this->mutex_);
  return this->result_spec_;
}

int SolveJob::preview_version() const {
  std::lock_guard lock(this->mutex_);
  return this->preview_version_;
}

std::optional<World> SolveJob::preview() const {
  std::lock_guard lock(this->mutex_);
  return this->preview_;
}

std::optional<std::string> SolveJob::error() const {
  std::lock_guard lock(this->mutex_);
  return this->error_;
}

void SolveJob::run() {
  try {
    JobContext job;
    job.progress = [this](std::string_view message) {
      std::lock_guard lock(this->mutex_);
      this->log_.emplace_back(message);
    };
    job.preview = [this](const World &layout) {
      std::lock_guard lock(this->mutex_);
      this->preview_ = layout;
      ++this->preview_version_;
    };
    job.cancel = &this->cancel_;
    JobResult result = this->work_(job);
    std::lock_guard lock(this->mutex_);
    this->report_ = std::move(result.report);
    this->result_spec_ = std::move(result.spec);
  } catch (const std::exception &error) {
    // Shown to the user by the UI; the thread must not let it escape.
    std::lock_guard lock(this->mutex_);
    this->error_ = error.what();
  }
  this->done_ = true;
}

} // namespace rs
