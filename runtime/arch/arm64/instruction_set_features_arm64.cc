/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "instruction_set_features_arm64.h"

#include <fstream>
#include <sstream>

#include "base/stringprintf.h"
#include "utils.h"  // For Trim.

namespace art {

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromVariant(
    const std::string& variant ATTRIBUTE_UNUSED, std::string* error_msg ATTRIBUTE_UNUSED) {
  if (variant != "default") {
    std::ostringstream os;
    os << "Unexpected CPU variant for Arm64: " << variant;
    *error_msg = os.str();
    return nullptr;
  }
  const bool smp = true;  // Conservative default.
  const bool is_a53 = true;  // Pessimistically assume all ARM64s are A53s.
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromBitmap(uint32_t bitmap) {
  bool smp = (bitmap & kSmpBitfield) != 0;
  bool is_a53 = (bitmap & kA53Bitfield) != 0;
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromCppDefines() {
#if defined(HAVE_ANDROID_OS) && (ANDROID_SMP == 0)
  const bool smp = false;
#else
  const bool smp = true;
#endif

  const bool is_a53 = true;  // Pessimistically assume all ARM64s are A53s.
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromCpuInfo() {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  bool smp = false;
  const bool is_a53 = true;  // Conservative default.

  std::ifstream in("/proc/cpuinfo");
  if (!in.fail()) {
    while (!in.eof()) {
      std::string line;
      std::getline(in, line);
      if (!in.eof()) {
        LOG(INFO) << "cpuinfo line: " << line;
        if (line.find("processor") != std::string::npos && line.find(": 1") != std::string::npos) {
          smp = true;
        }
      }
    }
    in.close();
  } else {
    LOG(ERROR) << "Failed to open /proc/cpuinfo";
  }
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromHwcap() {
  bool smp = sysconf(_SC_NPROCESSORS_CONF) > 1;
  const bool is_a53 = true;  // Pessimistically assume all ARM64s are A53s.
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

const Arm64InstructionSetFeatures* Arm64InstructionSetFeatures::FromAssembly() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

bool Arm64InstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (kArm64 != other->GetInstructionSet()) {
    return false;
  }
  const Arm64InstructionSetFeatures* other_as_arm = other->AsArm64InstructionSetFeatures();
  return fix_cortex_a53_835769_ == other_as_arm->fix_cortex_a53_835769_;
}

uint32_t Arm64InstructionSetFeatures::AsBitmap() const {
  return (IsSmp() ? kSmpBitfield : 0) | (fix_cortex_a53_835769_ ? kA53Bitfield : 0);
}

std::string Arm64InstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (IsSmp()) {
    result += "smp";
  } else {
    result += "-smp";
  }
  if (fix_cortex_a53_835769_) {
    result += ",a53";
  } else {
    result += ",-a53";
  }
  return result;
}

const InstructionSetFeatures* Arm64InstructionSetFeatures::AddFeaturesFromSplitString(
    const bool smp, const std::vector<std::string>& features, std::string* error_msg) const {
  bool is_a53 = fix_cortex_a53_835769_;
  for (auto i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "a53") {
      is_a53 = true;
    } else if (feature == "-a53") {
      is_a53 = false;
    } else {
      *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
      return nullptr;
    }
  }
  return new Arm64InstructionSetFeatures(smp, is_a53);
}

}  // namespace art
