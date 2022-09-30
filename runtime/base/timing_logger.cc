/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <stdio.h>

#include "timing_logger.h"

#include <android-base/logging.h>

#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "gc/heap.h"
#include "runtime.h"
#include "thread-current-inl.h"

#include <cmath>
#include <iomanip>

namespace art {

CumulativeLogger::CumulativeLogger(const std::string& name)
    : name_(name),
      lock_name_("CumulativeLoggerLock" + name),
      lock_(new Mutex(lock_name_.c_str(), kDefaultMutexLevel, true)) {
  Reset();
}

CumulativeLogger::~CumulativeLogger() {
  cumulative_timers_.clear();
}

void CumulativeLogger::SetName(const std::string& name) {
  MutexLock mu(Thread::Current(), *GetLock());
  name_.assign(name);
}

void CumulativeLogger::Start() {
}

void CumulativeLogger::End() {
  MutexLock mu(Thread::Current(), *GetLock());
  ++iterations_;
}

void CumulativeLogger::Reset() {
  MutexLock mu(Thread::Current(), *GetLock());
  iterations_ = 0;
  total_time_ = 0;
  cumulative_timers_.clear();
}

void CumulativeLogger::AddLogger(const TimingLogger &logger) {
  MutexLock mu(Thread::Current(), *GetLock());
  TimingLogger::TimingData timing_data(logger.CalculateTimingData());
  const std::vector<TimingLogger::Timing>& timings = logger.GetTimings();
  for (size_t i = 0; i < timings.size(); ++i) {
    if (timings[i].IsStartTiming()) {
      AddPair(timings[i].GetName(), timing_data.GetExclusiveTime(i));
    }
  }
  ++iterations_;
}

size_t CumulativeLogger::GetIterations() const {
  MutexLock mu(Thread::Current(), *GetLock());
  return iterations_;
}

void CumulativeLogger::Dump(std::ostream &os) const {
  MutexLock mu(Thread::Current(), *GetLock());
  DumpAverages(os);
}

void CumulativeLogger::AddPair(const char* label, uint64_t delta_time) {
  // Convert delta time to microseconds so that we don't overflow our counters.
  delta_time /= kAdjust;
  total_time_ += delta_time;
  CumulativeTime candidate(label, delta_time);
  auto it = std::lower_bound(cumulative_timers_.begin(), cumulative_timers_.end(), candidate);
  // Maintain the vector sorted so that lookup above, which is more frequent can
  // happen in log(n).
  if (it == cumulative_timers_.end() || it->Name() != label) {
    cumulative_timers_.insert(it, candidate);
  } else {
    it->Add(delta_time);
  }
}

void CumulativeLogger::DumpAverages(std::ostream &os) const {
  os << "Start Dumping Averages for " << iterations_ << " iterations"
     << " for " << name_ << "\n";
  const size_t timers_sz = cumulative_timers_.size();
  // Create an array of pointers to cumulative timers on stack and sort it in
  // decreasing order of accumulated timer so that the most time consuming
  // timer is printed first.
  const CumulativeTime* sorted_timers[timers_sz];
  for (size_t i = 0; i < timers_sz; i++) {
    sorted_timers[i] = cumulative_timers_.data() + i;
  }
  std::sort(sorted_timers,
            sorted_timers + timers_sz,
            [](const CumulativeTime* a, const CumulativeTime* b) { return a->Sum() > b->Sum(); });
  for (size_t i = 0; i < timers_sz; i++) {
    const CumulativeTime *timer = sorted_timers[i];
    uint64_t total_time_ns = timer->Sum() * kAdjust;
    os << timer->Name()
       << ":\tSum: " << PrettyDuration(total_time_ns)
       << " Avg: " << PrettyDuration(total_time_ns / iterations_) << "\n";
  }
  os << "Done Dumping Averages\n";
}

TimingLogger::TimingLogger(const char* name,
                           bool precise,
                           bool verbose,
                           TimingLogger::TimingKind kind)
    : name_(name), precise_(precise), verbose_(verbose), kind_(kind) {
}

void TimingLogger::Reset() {
  timings_.clear();
}

void TimingLogger::StartTiming(const char* label) {
  DCHECK(label != nullptr);
  timings_.push_back(Timing(kind_, label));
  ATraceBegin(label);
}

void TimingLogger::EndTiming() {
  timings_.push_back(Timing(kind_, nullptr));
  ATraceEnd();
}

uint64_t TimingLogger::GetTotalNs() const {
  if (timings_.size() < 2) {
    return 0;
  }
  return timings_.back().GetTime() - timings_.front().GetTime();
}

size_t TimingLogger::FindTimingIndex(const char* name, size_t start_idx) const {
  DCHECK_LT(start_idx, timings_.size());
  for (size_t i = start_idx; i < timings_.size(); ++i) {
    if (timings_[i].IsStartTiming() && strcmp(timings_[i].GetName(), name) == 0) {
      return i;
    }
  }
  return kIndexNotFound;
}

TimingLogger::TimingData TimingLogger::CalculateTimingData() const {
  TimingLogger::TimingData ret;
  ret.data_.resize(timings_.size());
  std::vector<size_t> open_stack;
  for (size_t i = 0; i < timings_.size(); ++i) {
    if (timings_[i].IsEndTiming()) {
      CHECK(!open_stack.empty()) << "No starting split for ending split at index " << i;
      size_t open_idx = open_stack.back();
      uint64_t time = timings_[i].GetTime() - timings_[open_idx].GetTime();
      ret.data_[open_idx].exclusive_time += time;
      DCHECK_EQ(ret.data_[open_idx].total_time, 0U);
      ret.data_[open_idx].total_time += time;
      // Each open split has exactly one end.
      open_stack.pop_back();
      // If there is a parent node, subtract from the exclusive time.
      if (!open_stack.empty()) {
        // Note this may go negative, but will work due to 2s complement when we add the value
        // total time value later.
        ret.data_[open_stack.back()].exclusive_time -= time;
      }
    } else {
      open_stack.push_back(i);
    }
  }
  CHECK(open_stack.empty()) << "Missing ending for timing "
      << timings_[open_stack.back()].GetName() << " at index " << open_stack.back();
  return ret;  // No need to fear, C++11 move semantics are here.
}

void TimingLogger::Dump(std::ostream &os, const char* indent_string) const {
  static constexpr size_t kFractionalDigits = 3;
  TimingLogger::TimingData timing_data(CalculateTimingData());
  uint64_t longest_split = 0;
  for (size_t i = 0; i < timings_.size(); ++i) {
    longest_split = std::max(longest_split, timing_data.GetTotalTime(i));
  }
  // Compute which type of unit we will use for printing the timings.
  TimeUnit tu = GetAppropriateTimeUnit(longest_split);
  uint64_t divisor = GetNsToTimeUnitDivisor(tu);
  uint64_t mod_fraction = divisor >= 1000 ? divisor / 1000 : 1;
  // Print formatted splits.
  size_t tab_count = 1;
  os << name_ << " [Exclusive time] [Total time]\n";
  for (size_t i = 0; i < timings_.size(); ++i) {
    if (timings_[i].IsStartTiming()) {
      uint64_t exclusive_time = timing_data.GetExclusiveTime(i);
      uint64_t total_time = timing_data.GetTotalTime(i);
      if (!precise_) {
        // Make the fractional part 0.
        exclusive_time -= exclusive_time % mod_fraction;
        total_time -= total_time % mod_fraction;
      }
      for (size_t j = 0; j < tab_count; ++j) {
        os << indent_string;
      }
      os << FormatDuration(exclusive_time, tu, kFractionalDigits);
      // If they are the same, just print one value to prevent spam.
      if (exclusive_time != total_time) {
        os << "/" << FormatDuration(total_time, tu, kFractionalDigits);
      }
      os << " " << timings_[i].GetName() << "\n";
      ++tab_count;
    } else {
      --tab_count;
    }
  }
  os << name_ << ": end, " << PrettyDuration(GetTotalNs()) << "\n";
}

void TimingLogger::Verify() {
  size_t counts[2] = { 0 };
  for (size_t i = 0; i < timings_.size(); ++i) {
    if (i > 0) {
      CHECK_LE(timings_[i - 1].GetTime(), timings_[i].GetTime());
    }
    ++counts[timings_[i].IsStartTiming() ? 0 : 1];
  }
  CHECK_EQ(counts[0], counts[1]) << "Number of StartTiming and EndTiming doesn't match";
}

TimingLogger::~TimingLogger() {
  if (kIsDebugBuild) {
    Verify();
  }
}

}  // namespace art
