#
# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Known broken tests for the ARM VIXL backend.
TEST_ART_BROKEN_OPTIMIZING_ARM_VIXL_RUN_TESTS := \
  103-string-append \
  137-cfi \
  488-checker-inline-recursive-calls \
  552-checker-sharpening \
  562-checker-no-intermediate \
  602-deoptimizeable \
