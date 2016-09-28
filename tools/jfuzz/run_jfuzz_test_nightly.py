#!/usr/bin/env python3.4
#
# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import os
import re
import shutil
import subprocess
import sys

from glob import glob

from tempfile import mkdtemp
from tempfile import TemporaryFile

# Default arguments for run_jfuzz_test.py.
DEFAULT_ARGS = ['--num_tests=20000']

# run_jfuzz_test.py success string.
SUCCESS_STRING = 'success (no divergences)'

# Constant returned by string find() method when search fails.
NOT_FOUND = -1

def main(argv):
  cwd = os.path.dirname(os.path.realpath(__file__))
  cmd = [cwd + '/run_jfuzz_test.py'] + DEFAULT_ARGS
  parser = argparse.ArgumentParser()
  parser.add_argument('--num_proc', default=8,
                      type=int, help='number of processes to run')
  # Unknown arguments are passed to run_jfuzz_test.py.
  (args, unknown_args) = parser.parse_known_args()
  output_files = [TemporaryFile('wb+') for _ in range(args.num_proc)]
  processes = []
  for output_file in output_files:
    processes.append(subprocess.Popen(cmd + unknown_args, stdout=output_file,
                                      stderr=subprocess.STDOUT))
  try:
    # Wait for processes to terminate.
    for proc in processes:
      proc.wait()
  except KeyboardInterrupt:
    for proc in processes:
      proc.kill()
  # Output results.
  output_dirs = []
  for i, output_file in enumerate(output_files):
    output_file.seek(0)
    output_str = output_file.read().decode('ascii')
    output_file.close()
    # Extract output directory. Example match: 'Directory : /tmp/tmp8ltpfjng'.
    directory_match = re.search(r'Directory[^:]*: ([^\n]+)\n', output_str)
    if directory_match:
      output_dirs.append(directory_match.group(1))
    print('Tester', i)
    if output_str.find(SUCCESS_STRING) == NOT_FOUND:
      print(output_str)
    else:
      print(SUCCESS_STRING)
  # Gather divergences.
  global_out_dir = mkdtemp('jfuzz_nightly')
  divergence_nr = 1
  for out_dir in output_dirs:
    for divergence_dir in glob(out_dir + '/divergence*/'):
      shutil.copytree(divergence_dir,
                      global_out_dir + '/divergence' + str(divergence_nr))
      divergence_nr += 1
  print('Global output directory:', global_out_dir)

if __name__ == '__main__':
  main(sys.argv)
