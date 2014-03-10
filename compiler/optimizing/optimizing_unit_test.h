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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_

#define NUM_INSTRUCTIONS(...)  \
  (sizeof((uint16_t[]) {__VA_ARGS__}) /sizeof(uint16_t))

#define ZERO_REGISTER_CODE_ITEM(...)                                       \
    { 0, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define ONE_REGISTER_CODE_ITEM(...)                                        \
    { 1, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
