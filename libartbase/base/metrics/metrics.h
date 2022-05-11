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

#ifndef ART_LIBARTBASE_BASE_METRICS_METRICS_H_
#define ART_LIBARTBASE_BASE_METRICS_METRICS_H_

#include <stdint.h>

#include <array>
#include <atomic>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "android-base/logging.h"
#include "base/bit_utils.h"
#include "base/time_utils.h"
#include "tinyxml2.h"

#pragma clang diagnostic push
#pragma clang diagnostic error "-Wconversion"

// See README.md in this directory for how to define metrics.
#define ART_METRICS(METRIC)                                             \
  METRIC(ClassLoadingTotalTime, MetricsCounter)                         \
  METRIC(ClassVerificationTotalTime, MetricsCounter)                    \
  METRIC(ClassVerificationCount, MetricsCounter)                        \
  METRIC(WorldStopTimeDuringGCAvg, MetricsAverage)                      \
  METRIC(YoungGcCount, MetricsCounter)                                  \
  METRIC(FullGcCount, MetricsCounter)                                   \
  METRIC(TotalBytesAllocated, MetricsCounter)                           \
  METRIC(TotalGcCollectionTime, MetricsCounter)                         \
  METRIC(YoungGcThroughputAvg, MetricsAverage)                          \
  METRIC(FullGcThroughputAvg, MetricsAverage)                           \
  METRIC(YoungGcTracingThroughputAvg, MetricsAverage)                   \
  METRIC(FullGcTracingThroughputAvg, MetricsAverage)                    \
  METRIC(JitMethodCompileTotalTime, MetricsCounter)                     \
  METRIC(JitMethodCompileCount, MetricsCounter)                         \
  METRIC(YoungGcCollectionTime, MetricsHistogram, 15, 0, 60'000)        \
  METRIC(FullGcCollectionTime, MetricsHistogram, 15, 0, 60'000)         \
  METRIC(YoungGcThroughput, MetricsHistogram, 15, 0, 10'000)            \
  METRIC(FullGcThroughput, MetricsHistogram, 15, 0, 10'000)             \
  METRIC(YoungGcTracingThroughput, MetricsHistogram, 15, 0, 10'000)     \
  METRIC(FullGcTracingThroughput, MetricsHistogram, 15, 0, 10'000)      \
  METRIC(GcWorldStopTime, MetricsCounter)                               \
  METRIC(GcWorldStopCount, MetricsCounter)                              \
  METRIC(YoungGcScannedBytes, MetricsCounter)                           \
  METRIC(YoungGcFreedBytes, MetricsCounter)                             \
  METRIC(YoungGcDuration, MetricsCounter)                               \
  METRIC(FullGcScannedBytes, MetricsCounter)                            \
  METRIC(FullGcFreedBytes, MetricsCounter)                              \
  METRIC(FullGcDuration, MetricsCounter)

// A lot of the metrics implementation code is generated by passing one-off macros into ART_COUNTERS
// and ART_HISTOGRAMS. This means metrics.h and metrics.cc are very #define-heavy, which can be
// challenging to read. The alternative was to require a lot of boilerplate code for each new metric
// added, all of which would need to be rewritten if the metrics implementation changed. Using
// macros lets us add new metrics by adding a single line to either ART_COUNTERS or ART_HISTOGRAMS,
// and modifying the implementation only requires changing the implementation once, instead of once
// per metric.

namespace art {

class Runtime;
struct RuntimeArgumentMap;

namespace metrics {

/**
 * An enumeration of all ART counters and histograms.
 */
enum class DatumId {
#define METRIC(name, type, ...) k##name,
  ART_METRICS(METRIC)
#undef METRIC
};

// Names come from PackageManagerServiceCompilerMapping.java
#define REASON_NAME_LIST(V) \
  V(kError, "error") \
  V(kUnknown, "unknown") \
  V(kFirstBoot, "first-boot") \
  V(kBootAfterOTA, "boot-after-ota") \
  V(kPostBoot, "post-boot") \
  V(kInstall, "install") \
  V(kInstallFast, "install-fast") \
  V(kInstallBulk, "install-bulk") \
  V(kInstallBulkSecondary, "install-bulk-secondary") \
  V(kInstallBulkDowngraded, "install-bulk-downgraded") \
  V(kInstallBulkSecondaryDowngraded, "install-bulk-secondary-downgraded") \
  V(kBgDexopt, "bg-dexopt") \
  V(kABOTA, "ab-ota") \
  V(kInactive, "inactive") \
  V(kShared, "shared") \
  V(kInstallWithDexMetadata, "install-with-dex-metadata") \
  V(kPrebuilt, "prebuilt") \
  V(kCmdLine, "cmdline") \
  V(kVdex, "vdex")

// We log compilation reasons as part of the metadata we report. Since elsewhere compilation reasons
// are specified as a string, we define them as an enum here which indicates the reasons that we
// support.
enum class CompilationReason {
#define REASON(kind, name) kind,
  REASON_NAME_LIST(REASON)
#undef REASON
};

#define REASON_NAME(kind, kind_name) \
    case CompilationReason::kind: return kind_name;
#define REASON_FROM_NAME(kind, kind_name) \
    if (name == kind_name) { return CompilationReason::kind; }

constexpr const char* CompilationReasonName(CompilationReason reason) {
  switch (reason) {
    REASON_NAME_LIST(REASON_NAME)
  }
}

constexpr CompilationReason CompilationReasonFromName(std::string_view name) {
  REASON_NAME_LIST(REASON_FROM_NAME)
  return CompilationReason::kError;
}

#undef REASON_NAME
#undef ReasonFromName

#define COMPILER_FILTER_REPORTING_LIST(V) \
  V(kError, "error") /* Error (invalid value) condition */ \
  V(kUnknown, "unknown") /* Unknown (not set) condition */ \
  V(kAssumeVerified, "assume-verified") /* Standard compiler filters */ \
  V(kExtract, "extract") \
  V(kVerify, "verify") \
  V(kSpaceProfile, "space-profile") \
  V(kSpace, "space") \
  V(kSpeedProfile, "speed-profile") \
  V(kSpeed, "speed") \
  V(kEverythingProfile, "everything-profile") \
  V(kEverything, "everything") \
  V(kRunFromApk, "run-from-apk") /* Augmented compiler filters as produces by OatFileAssistant#GetOptimizationStatus */ \
  V(kRunFromApkFallback, "run-from-apk-fallback")

// Augmented compiler filter enum, used in the reporting infra.
enum class CompilerFilterReporting {
#define FILTER(kind, name) kind,
  COMPILER_FILTER_REPORTING_LIST(FILTER)
#undef FILTER
};

#define FILTER_NAME(kind, kind_name) \
    case CompilerFilterReporting::kind: return kind_name;
#define FILTER_FROM_NAME(kind, kind_name) \
    if (name == kind_name) { return CompilerFilterReporting::kind; }

constexpr const char* CompilerFilterReportingName(CompilerFilterReporting filter) {
  switch (filter) {
    COMPILER_FILTER_REPORTING_LIST(FILTER_NAME)
  }
}

constexpr CompilerFilterReporting CompilerFilterReportingFromName(std::string_view name) {
  COMPILER_FILTER_REPORTING_LIST(FILTER_FROM_NAME)
  return CompilerFilterReporting::kError;
}

#undef FILTER_NAME
#undef FILTER_FROM_NAME

// SessionData contains metadata about a metrics session (basically the lifetime of an ART process).
// This information should not change for the lifetime of the session.
struct SessionData {
  static SessionData CreateDefault();

  static constexpr int64_t kInvalidSessionId = -1;
  static constexpr int32_t kInvalidUserId = -1;

  int64_t session_id;
  int32_t uid;
  CompilationReason compilation_reason;
  CompilerFilterReporting compiler_filter;
};

// MetricsBackends are used by a metrics reporter to write metrics to some external location. For
// example, a backend might write to logcat, or to a file, or to statsd.
class MetricsBackend {
 public:
  virtual ~MetricsBackend() {}

  // Begins an ART metrics session.
  //
  // This is called by the metrics reporter when the runtime is starting up. The session_data
  // includes a session id which is used to correlate any metric reports with the same instance of
  // the ART runtime. Additionally, session_data includes useful metadata such as the package name
  // for this process.
  //
  // It may also be called whenever there is an update to the session metadata (e.g. optimization
  // state).
  virtual void BeginOrUpdateSession(const SessionData& session_data) = 0;

 protected:
  // Called by the metrics reporter to indicate that a new metrics report is starting.
  virtual void BeginReport(uint64_t timestamp_since_start_ms) = 0;

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

  // Called by the metrics reporter to indicate that the current metrics report is complete.
  virtual void EndReport() = 0;

  template <DatumId counter_type, typename T>
  friend class MetricsCounter;
  template <DatumId histogram_type, size_t num_buckets, int64_t low_value, int64_t high_value>
  friend class MetricsHistogram;
  template <DatumId datum_id, typename T, const T& AccumulatorFunction(const T&, const T&)>
  friend class MetricsAccumulator;
  template <DatumId datum_id, typename T>
  friend class MetricsAverage;
  friend class ArtMetrics;
};

template <typename value_t>
class MetricsBase {
 public:
  virtual void Add(value_t value) = 0;
  virtual ~MetricsBase() { }
};

template <DatumId counter_type, typename T = uint64_t>
class MetricsCounter : public MetricsBase<T> {
 public:
  using value_t = T;
  explicit constexpr MetricsCounter(uint64_t value = 0) : value_{value} {
    // Ensure we do not have any unnecessary data in this class.
    // Adding intptr_t to accommodate vtable, and rounding up to incorporate
    // padding.
    static_assert(RoundUp(sizeof(*this), sizeof(uint64_t))
                  == RoundUp(sizeof(intptr_t) + sizeof(value_t), sizeof(uint64_t)));
  }

  void AddOne() { Add(1u); }
  void Add(value_t value) { value_.fetch_add(value, std::memory_order::memory_order_relaxed); }

  void Report(MetricsBackend* backend) const { backend->ReportCounter(counter_type, Value()); }

 protected:
  void Reset() {
    value_ = 0;
  }

  value_t Value() const { return value_.load(std::memory_order::memory_order_relaxed); }

 private:
  std::atomic<value_t> value_;
  static_assert(std::atomic<value_t>::is_always_lock_free);

  friend class ArtMetrics;
};

template <DatumId datum_id, typename T = uint64_t>
class MetricsAverage final : public MetricsCounter<datum_id, T> {
 public:
  using value_t = T;
  using count_t = T;
  explicit constexpr MetricsAverage(uint64_t value = 0, uint64_t count = 0) :
      MetricsCounter<datum_id, value_t>(value), count_(count) {
    // Ensure we do not have any unnecessary data in this class.
    // Adding intptr_t to accommodate vtable, and rounding up to incorporate
    // padding.
    static_assert(RoundUp(sizeof(*this), sizeof(uint64_t))
                  == RoundUp(sizeof(intptr_t) + sizeof(value_t) + sizeof(count_t),
                             sizeof(uint64_t)));
  }

  // We use release memory-order here and then acquire in Report() to ensure
  // that at least the non-racy reads/writes to this metric are consistent. This
  // doesn't guarantee the atomicity of the change to both fields, but that
  // may not be desired because:
  // 1. The metric eventually becomes consistent.
  // 2. For sufficiently large count_, a few data points which are off shouldn't
  // make a huge difference to the reporter.
  void Add(value_t value) {
    MetricsCounter<datum_id, value_t>::Add(value);
    count_.fetch_add(1, std::memory_order::memory_order_release);
  }

  void Report(MetricsBackend* backend) const {
    count_t count = count_.load(std::memory_order::memory_order_acquire);
    backend->ReportCounter(datum_id,
                           // Avoid divide-by-0.
                           count != 0 ? MetricsCounter<datum_id, value_t>::Value() / count : 0);
  }

 protected:
  void Reset() {
    count_ = 0;
    MetricsCounter<datum_id, value_t>::Reset();
  }

 private:
  std::atomic<count_t> count_;
  static_assert(std::atomic<count_t>::is_always_lock_free);

  friend class ArtMetrics;
};

template <DatumId histogram_type_,
          size_t num_buckets_,
          int64_t minimum_value_,
          int64_t maximum_value_>
class MetricsHistogram final : public MetricsBase<int64_t> {
  static_assert(num_buckets_ >= 1);
  static_assert(minimum_value_ < maximum_value_);

 public:
  using value_t = uint32_t;

  constexpr MetricsHistogram() : buckets_{} {
    // Ensure we do not have any unnecessary data in this class.
    // Adding intptr_t to accommodate vtable, and rounding up to incorporate
    // padding.
    static_assert(RoundUp(sizeof(*this), sizeof(uint64_t))
                  == RoundUp(sizeof(intptr_t) + sizeof(value_t) * num_buckets_, sizeof(uint64_t)));
  }

  void Add(int64_t value) {
    const size_t i = FindBucketId(value);
    buckets_[i].fetch_add(1u, std::memory_order::memory_order_relaxed);
  }

  void Report(MetricsBackend* backend) const {
    backend->ReportHistogram(histogram_type_, minimum_value_, maximum_value_, GetBuckets());
  }

 protected:
  void Reset() {
    for (auto& bucket : buckets_) {
      bucket = 0;
    }
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

  friend class ArtMetrics;
};

template <DatumId datum_id, typename T, const T& AccumulatorFunction(const T&, const T&)>
class MetricsAccumulator final : MetricsBase<T> {
 public:
  explicit constexpr MetricsAccumulator(T value = 0) : value_{value} {
    // Ensure we do not have any unnecessary data in this class.
    // Adding intptr_t to accommodate vtable, and rounding up to incorporate
    // padding.
    static_assert(RoundUp(sizeof(*this), sizeof(uint64_t)) ==
                  RoundUp(sizeof(intptr_t) + sizeof(T), sizeof(uint64_t)));
  }

  void Add(T value) {
    T current = value_.load(std::memory_order::memory_order_relaxed);
    T new_value;
    do {
      new_value = AccumulatorFunction(current, value);
      // If the value didn't change, don't bother storing it.
      if (current == new_value) {
        break;
      }
    } while (!value_.compare_exchange_weak(
        current, new_value, std::memory_order::memory_order_relaxed));
  }

  // Report the metric as a counter, since this has only a single value.
  void Report(MetricsBackend* backend) const {
    backend->ReportCounter(datum_id, static_cast<uint64_t>(Value()));
  }

 protected:
  void Reset() {
    value_ = 0;
  }

 private:
  T Value() const { return value_.load(std::memory_order::memory_order_relaxed); }

  std::atomic<T> value_;

  friend class ArtMetrics;
};

// Base class for formatting metrics into different formats
// (human-readable text, JSON, etc.)
class MetricsFormatter {
 public:
  virtual ~MetricsFormatter() = default;

  virtual void FormatBeginReport(uint64_t timestamp_since_start_ms,
                                 const std::optional<SessionData>& session_data) = 0;
  virtual void FormatEndReport() = 0;
  virtual void FormatReportCounter(DatumId counter_type, uint64_t value) = 0;
  virtual void FormatReportHistogram(DatumId histogram_type,
                                     int64_t low_value,
                                     int64_t high_value,
                                     const std::vector<uint32_t>& buckets) = 0;
  virtual std::string GetAndResetBuffer() = 0;

 protected:
  const std::string version = "1.0";
};

// Formatter outputting metrics in human-readable text format
class TextFormatter : public MetricsFormatter {
 public:
  TextFormatter() = default;

  void FormatBeginReport(uint64_t timestamp_millis,
                         const std::optional<SessionData>& session_data) override;

  void FormatReportCounter(DatumId counter_type, uint64_t value) override;

  void FormatReportHistogram(DatumId histogram_type,
                             int64_t low_value,
                             int64_t high_value,
                             const std::vector<uint32_t>& buckets) override;

  void FormatEndReport() override;

  std::string GetAndResetBuffer() override;

 private:
  std::ostringstream os_;
};

// Formatter outputting metrics in XML format
class XmlFormatter : public MetricsFormatter {
 public:
  XmlFormatter() = default;

  void FormatBeginReport(uint64_t timestamp_millis,
                         const std::optional<SessionData>& session_data) override;

  void FormatReportCounter(DatumId counter_type, uint64_t value) override;

  void FormatReportHistogram(DatumId histogram_type,
                             int64_t low_value,
                             int64_t high_value,
                             const std::vector<uint32_t>& buckets) override;

  void FormatEndReport() override;

  std::string GetAndResetBuffer() override;

 private:
  tinyxml2::XMLDocument document_;
};

// A backend that writes metrics to a string.
// The format of the metrics' output is delegated
// to the MetricsFormatter class.
//
// This is used as a base for LogBackend and FileBackend.
class StringBackend : public MetricsBackend {
 public:
  explicit StringBackend(std::unique_ptr<MetricsFormatter> formatter);

  void BeginOrUpdateSession(const SessionData& session_data) override;

  void BeginReport(uint64_t timestamp_millis) override;

  void ReportCounter(DatumId counter_type, uint64_t value) override;

  void ReportHistogram(DatumId histogram_type,
                       int64_t low_value,
                       int64_t high_value,
                       const std::vector<uint32_t>& buckets) override;

  void EndReport() override;

  std::string GetAndResetBuffer();

 private:
  std::unique_ptr<MetricsFormatter> formatter_;
  std::optional<SessionData> session_data_;
};

// A backend that writes metrics in human-readable format to the log (i.e. logcat).
class LogBackend : public StringBackend {
 public:
  explicit LogBackend(std::unique_ptr<MetricsFormatter> formatter,
                      android::base::LogSeverity level);

  void BeginReport(uint64_t timestamp_millis) override;
  void EndReport() override;

 private:
  android::base::LogSeverity level_;
};

// A backend that writes metrics to a file.
class FileBackend : public StringBackend {
 public:
  explicit FileBackend(std::unique_ptr<MetricsFormatter> formatter,
                       const std::string& filename);

  void BeginReport(uint64_t timestamp_millis) override;
  void EndReport() override;

 private:
  std::string filename_;
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

  // Resets all metrics to their initial value. This is intended to be used after forking from the
  // zygote so we don't attribute parent values to the child process.
  void Reset();

#define METRIC_ACCESSORS(name, Kind, ...)                                        \
  Kind<DatumId::k##name, ##__VA_ARGS__>* name() { return &name##_; } \
  const Kind<DatumId::k##name, ##__VA_ARGS__>* name() const { return &name##_; }
  ART_METRICS(METRIC_ACCESSORS)
#undef METRIC_ACCESSORS

 private:
  uint64_t beginning_timestamp_;

#define METRIC(name, Kind, ...) Kind<DatumId::k##name, ##__VA_ARGS__> name##_;
  ART_METRICS(METRIC)
#undef METRIC
};

// Returns a human readable name for the given DatumId.
std::string DatumName(DatumId datum);

// We also log the thread type for metrics so we can distinguish things that block the UI thread
// from things that happen on the background thread. This enum keeps track of what thread types we
// support.
enum class ThreadType {
  kMain,
  kBackground,
};

}  // namespace metrics
}  // namespace art

#pragma clang diagnostic pop  // -Wconversion

#endif  // ART_LIBARTBASE_BASE_METRICS_METRICS_H_
