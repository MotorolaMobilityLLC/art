#!/bin/bash
#
# Copyright 2017 The Android Open Source Project
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


def run(ctx, args):
  # Ask for stack traces to be dumped to a file rather than to stdout.
  ctx.default_run(args, jvmti=True)

  # The RI sends an extra event that art doesn't.
  if args.jvm:
    ctx.expected_stdout = ctx.expected_stdout.with_suffix(".jvm.txt")
