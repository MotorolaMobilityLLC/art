/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_LIBARTBASE_BASE_METRICS_H_
#define ART_LIBARTBASE_BASE_METRICS_H_

#include <stdint.h>

#include <array>
#include <atomic>
#include <ostream>
#include <string_view>
#include <vector>

#include "android-base/logging.h"
#include "base/time_utils.h"

#pragma clang diagnostic push
#pragma clang diagnostic error "-Wconversion"

// COUNTER(counter_name)
#define ART_COUNTERS(COUNTER) COUNTER(ClassVerificationTotalTime)

// HISTOGRAM(counter_name, num_buckets, minimum_value, maximum_value)
//
// The num_buckets parameter affects memory usage for the histogram and data usage for exported
// metrics. It is recommended to keep this below 16.
//
// The minimum_value and maximum_value parameters are needed because we need to know what range the
// fixed number of buckets cover. We could keep track of the observed ranges and try to rescale the
// buckets or allocate new buckets, but this would make incrementing them more expensive than just
// some index arithmetic and an add.
//
// Values outside the range get clamped to the nearest bucket (basically, the two buckets on either
// side are infinitely long). If we see those buckets being way taller than the others, it means we
// should consider expanding the range.
#define ART_HISTOGRAMS(HISTOGRAM) HISTOGRAM(JitMethodCompileTime, 15, 0, 1'000'000)

// A lot of the metrics implementation code is generated by passing one-off macros into ART_COUNTERS
// and ART_HISTOGRAMS. This means metrics.h and metrics.cc are very #define-heavy, which can be
// challenging to read. The alternative was to require a lot of boilerplate code for each new metric
// added, all of which would need to be rewritten if the metrics implementation changed. Using
// macros lets us add new metrics by adding a single line to either ART_COUNTERS or ART_HISTOGRAMS,
// and modifying the implementation only requires changing the implementation once, instead of once
// per metric.

namespace art {
namespace metrics {

/**
 * An enumeration of all ART counters and histograms.
 */
enum class DatumId {
#define ART_COUNTER(name) k##name,
  ART_COUNTERS(ART_COUNTER)
#undef ART_COUNTER

#define ART_HISTOGRAM(name, num_buckets, low_value, high_value) k##name,
  ART_HISTOGRAMS(ART_HISTOGRAM)
#undef ART_HISTOGRAM
};

struct SessionData {
  const uint64_t session_id;
  const std::string_view package_name;
  // TODO: compiler filter / dexopt state
};

// MetricsBackends are used by a metrics reporter to write metrics to some external location. For
// example, a backend might write to logcat, or to a file, or to statsd.
class MetricsBackend {
 public:
  virtual ~MetricsBackend() {}

 protected:
  // Begins an ART metrics session.
  //
  // This is called by the metrics reporter when the runtime is starting up. The session_data
  // includes a session id which is used to correlate any metric reports with the same instance of
  // the ART runtime. Additionally, session_data includes useful metadata such as the package name
  // for this process.
  virtual void BeginSession(const SessionData& session_data) = 0;

  // Marks the end of a metrics session.
  //
  // The metrics reporter will call this when metrics reported ends (e.g. when the runtime is
  // shutting down). No further metrics will be reported for this session. Note that EndSession is
  // not guaranteed to be called, since clean shutdowns for the runtime are quite rare in practice.
  virtual void EndSession() = 0;

  // Called by the metrics reporter to give the current value of the counter with id counter_type.
  //
  // This will be called multiple times for each counter based on when the metrics reporter chooses
  // to report metrics. For example, the metrics reporter may call this at shutdown or every N
  // minutes. Counters are not reset in between invocations, so the value should represent the
  // total count at the point this method is called.
  virtual void ReportCounter(DatumId counter_type, uint64_t value) = 0;

  // Called by the metrics reporter to report a histogram.
  //
  // This is called similarly to ReportCounter, but instead of receiving a single value, it receives
  // a vector of the value in each bucket. Additionally, the function receives the lower and upper
  // limit for the histogram. Note that these limits are the allowed limits, and not the observed
  // range. Values below the lower limit will be counted in the first bucket, and values above the
  // upper limit will be counted in the last bucket. Backends should store the minimum and maximum
  // values to allow comparisons across module versions, since the minimum and maximum values may
  // change over time.
  virtual void ReportHistogram(DatumId histogram_type,
                               int64_t minimum_value,
                               int64_t maximum_value,
                               const std::vector<uint32_t>& buckets) = 0;

  template <DatumId counter_type>
  friend class MetricsCounter;
  template <DatumId histogram_type, size_t num_buckets, int64_t low_value, int64_t high_value>
  friend class MetricsHistogram;
};

template <DatumId counter_type>
class MetricsCounter {
 public:
  using value_t = uint64_t;

  explicit constexpr MetricsCounter(uint64_t value = 0) : value_{value} {
    // Ensure we do not have any unnecessary data in this class.
    static_assert(sizeof(*this) == sizeof(uint64_t));
  }

  void AddOne() { Add(1u); }
  void Add(value_t value) { value_.fetch_add(value, std::memory_order::memory_order_relaxed); }

  void Report(MetricsBackend* backend) const { backend->ReportCounter(counter_type, Value()); }

 private:
  value_t Value() const { return value_.load(std::memory_order::memory_order_relaxed); }

  std::atomic<value_t> value_;
  static_assert(std::atomic<value_t>::is_always_lock_free);
};

template <DatumId histogram_type_,
          size_t num_buckets_,
          int64_t minimum_value_,
          int64_t maximum_value_>
class MetricsHistogram {
  static_assert(num_buckets_ >= 1);
  static_assert(minimum_value_ < maximum_value_);

 public:
  using value_t = uint32_t;

  constexpr MetricsHistogram() : buckets_{} {
    // Ensure we do not have any unnecessary data in this class.
    static_assert(sizeof(*this) == sizeof(uint32_t) * num_buckets_);
  }

  void Add(int64_t value) {
    const size_t i = FindBucketId(value);
    buckets_[i].fetch_add(1u, std::memory_order::memory_order_relaxed);
  }

  void Report(MetricsBackend* backend) const {
    backend->ReportHistogram(histogram_type_, minimum_value_, maximum_value_, GetBuckets());
  }

 private:
  inline constexpr size_t FindBucketId(int64_t value) const {
    // Values below the minimum are clamped into the first bucket.
    if (value <= minimum_value_) {
      return 0;
    }
    // Values above the maximum are clamped into the last bucket.
    if (value >= maximum_value_) {
      return num_buckets_ - 1;
    }
    // Otherise, linearly interpolate the value into the right bucket
    constexpr size_t bucket_width = maximum_value_ - minimum_value_;
    return static_cast<size_t>(value - minimum_value_) * num_buckets_ / bucket_width;
  }

  std::vector<value_t> GetBuckets() const {
    // The loads from buckets_ will all be memory_order_seq_cst, which means they will be acquire
    // loads. This is a stricter memory order than is needed, but this should not be a
    // performance-critical section of code.
    return std::vector<value_t>{buckets_.begin(), buckets_.end()};
  }

  std::array<std::atomic<value_t>, num_buckets_> buckets_;
  static_assert(std::atomic<value_t>::is_always_lock_free);
};

// A backend that writes metrics in a human-readable format to an std::ostream.
class StreamBackend : public MetricsBackend {
 public:
  explicit StreamBackend(std::ostream& os);

  void BeginSession(const SessionData& session_data) override;
  void EndSession() override;

  void ReportCounter(DatumId counter_type, uint64_t value) override;

  void ReportHistogram(DatumId histogram_type,
                       int64_t low_value,
                       int64_t high_value,
                       const std::vector<uint32_t>& buckets) override;

 private:
  std::ostream& os_;
};

/**
 * AutoTimer simplifies time-based metrics collection.
 *
 * Several modes are supported. In the default case, the timer starts immediately and stops when it
 * goes out of scope. Example:
 *
 *     {
 *       AutoTimer timer{metric};
 *       DoStuff();
 *       // timer stops and updates metric automatically here.
 *     }
 *
 * You can also stop the timer early:
 *
 *     timer.Stop();
 *
 * Finally, you can choose to not automatically start the timer at the beginning by passing false as
 * the second argument to the constructor:
 *
 *     AutoTimer timer{metric, false};
 *     DoNotTimeThis();
 *     timer.Start();
 *     TimeThis();
 *
 * Manually started timers will still automatically stop in the destructor, but they can be manually
 * stopped as well.
 *
 * Note that AutoTimer makes calls to MicroTime(), so this may not be suitable on critical paths, or
 * in cases where the counter needs to be started and stopped on different threads.
 */
template <typename Metric>
class AutoTimer {
 public:
  explicit AutoTimer(Metric* metric, bool autostart = true)
      : running_{false}, start_time_microseconds_{}, metric_{metric} {
    if (autostart) {
      Start();
    }
  }

  ~AutoTimer() {
    if (running_) {
      Stop();
    }
  }

  void Start() {
    DCHECK(!running_);
    running_ = true;
    start_time_microseconds_ = MicroTime();
  }

  // Stops a running timer. Returns the time elapsed since starting the timer in microseconds.
  uint64_t Stop() {
    DCHECK(running_);
    uint64_t stop_time_microseconds = MicroTime();
    running_ = false;

    uint64_t elapsed_time = stop_time_microseconds - start_time_microseconds_;
    metric_->Add(static_cast<typename Metric::value_t>(elapsed_time));
    return elapsed_time;
  }

 private:
  bool running_;
  uint64_t start_time_microseconds_;
  Metric* metric_;
};

/**
 * This struct contains all of the metrics that ART reports.
 */
class ArtMetrics {
 public:
  ArtMetrics();

  void ReportAllMetrics(MetricsBackend* backend) const;
  void DumpForSigQuit(std::ostream& os) const;

#define ART_COUNTER(name)                                       \
  MetricsCounter<DatumId::k##name>* name() { return &name##_; } \
  const MetricsCounter<DatumId::k##name>* name() const { return &name##_; }
  ART_COUNTERS(ART_COUNTER)
#undef ART_COUNTER

#define ART_HISTOGRAM(name, num_buckets, low_value, high_value)                                \
  MetricsHistogram<DatumId::k##name, num_buckets, low_value, high_value>* name() {             \
    return &name##_;                                                                           \
  }                                                                                            \
  const MetricsHistogram<DatumId::k##name, num_buckets, low_value, high_value>* name() const { \
    return &name##_;                                                                           \
  }
  ART_HISTOGRAMS(ART_HISTOGRAM)
#undef ART_HISTOGRAM

 private:
  // This field is only included to allow us expand the ART_COUNTERS and ART_HISTOGRAMS macro in
  // the initializer list in ArtMetrics::ArtMetrics. See metrics.cc for how it's used.
  //
  // It's declared as a zero-length array so it has no runtime space impact.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
  int unused_[0];
#pragma clang diagnostic pop  // -Wunused-private-field

#define ART_COUNTER(name) MetricsCounter<DatumId::k##name> name##_;
  ART_COUNTERS(ART_COUNTER)
#undef ART_COUNTER

#define ART_HISTOGRAM(name, num_buckets, low_value, high_value) \
  MetricsHistogram<DatumId::k##name, num_buckets, low_value, high_value> name##_;
  ART_HISTOGRAMS(ART_HISTOGRAM)
#undef ART_HISTOGRAM
};

// Returns a human readable name for the given DatumId.
std::string DatumName(DatumId datum);

struct ReportingConfig {
  bool dump_to_logcat;
  // TODO(eholk): this will grow to support other configurations, such as logging to a file, or
  // statsd. There will also be options for reporting after a period of time, or at certain events.
};

// MetricsReporter handles periodically reporting ART metrics.
class MetricsReporter {
 public:
  // Creates a MetricsReporter instance that matches the options selected in ReportingConfig.
  static std::unique_ptr<MetricsReporter> Create(ReportingConfig config, const ArtMetrics* metrics);

  ~MetricsReporter();

 private:
  explicit MetricsReporter(ReportingConfig config, const ArtMetrics* metrics);

  ReportingConfig config_;
  const ArtMetrics* metrics_;
};


}  // namespace metrics
}  // namespace art

#pragma clang diagnostic pop  // -Wconversion

#endif  // ART_LIBARTBASE_BASE_METRICS_H_
