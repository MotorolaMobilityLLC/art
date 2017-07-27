# Copyright 2017, The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import re
import tempfile
import subprocess

_env = dict(os.environ)

def _getEnvBoolean(var, default):
  val = _env.get(var)
  if val:
    if val == "True" or val == "true":
      return True
    if val == "False" or val == "false":
      return False
  return default

_DUMP_MANY_VARS_LIST = ['HOST_2ND_ARCH_PREFIX',
                        'TARGET_2ND_ARCH',
                        'TARGET_ARCH',
                        'HOST_PREFER_32_BIT',
                        'HOST_OUT_EXECUTABLES',
                        'ANDROID_JAVA_TOOLCHAIN']
_DUMP_MANY_VARS = None  # To be set to a dictionary with above list being the keys,
                        # and the build variable being the value.
def _dump_many_vars(var_name):
  """
  Reach into the Android build system to dump many build vars simultaneously.
  Since the make system is so slow, we want to avoid calling into build frequently.
  """
  global _DUMP_MANY_VARS
  global _DUMP_MANY_VARS_LIST

  # Look up var from cache.
  if _DUMP_MANY_VARS:
    return _DUMP_MANY_VARS[var_name]

  all_vars=" ".join(_DUMP_MANY_VARS_LIST)

  # The command is taken from build/envsetup.sh to fetch build variables.
  command = ("CALLED_FROM_SETUP=true "  # Enable the 'dump-many-vars' make target.
             "BUILD_SYSTEM=build/core " # Set up lookup path for make includes.
             "make --no-print-directory -C \"%s\" -f build/core/config.mk "
             "dump-many-vars DUMP_MANY_VARS=\"%s\"") % (ANDROID_BUILD_TOP, all_vars)

  config = subprocess.Popen(command,
                            stdout=subprocess.PIPE,
                            universal_newlines=True,
                            shell=True).communicate()[0] # read until EOF, select stdin
  # Prints out something like:
  # TARGET_ARCH='arm64'
  # HOST_ARCH='x86_64'
  _DUMP_MANY_VARS = {}
  for line in config.split("\n"):
    # Split out "$key='$value'" via regex.
    match = re.search("([^=]+)='([^']*)", line)
    if not match:
      continue
    key = match.group(1)
    value = match.group(2)
    _DUMP_MANY_VARS[key] = value

  return _DUMP_MANY_VARS[var_name]

def _get_build_var(var_name):
  return _dump_many_vars(var_name)

def get_env(key):
  return _env.get(key)

def _get_android_build_top():
  path_to_top = _env.get('ANDROID_BUILD_TOP')
  if not path_to_top:
    # nothing set. try to guess it based on the relative path of this env.py file.
    this_file_path = os.path.realpath(__file__)
    path_to_top = os.path.join(os.path.dirname(this_file_path), '../../../')
    path_to_top = os.path.realpath(path_to_top)

  if not os.path.exists(os.path.join(path_to_top, 'build/envsetup.sh')):
    raise AssertionError("env.py must be located inside an android source tree")

  return path_to_top

ANDROID_BUILD_TOP = _get_android_build_top()

# Compiling with jack? Possible values in (True, False, 'default')
ANDROID_COMPILE_WITH_JACK = _getEnvBoolean('ANDROID_COMPILE_WITH_JACK', 'default')

# Directory used for temporary test files on the host.
ART_HOST_TEST_DIR = tempfile.mkdtemp(prefix = 'test-art-')

# Keep going after encountering a test failure?
ART_TEST_KEEP_GOING = _getEnvBoolean('ART_TEST_KEEP_GOING', True)

# Do you want all tests, even those that are time consuming?
ART_TEST_FULL = _getEnvBoolean('ART_TEST_FULL', False)

# Do you want interpreter tests run?
ART_TEST_INTERPRETER = _getEnvBoolean('ART_TEST_INTERPRETER', ART_TEST_FULL)
ART_TEST_INTERPRETER_ACCESS_CHECKS = _getEnvBoolean('ART_TEST_INTERPRETER_ACCESS_CHECKS',
                                                   ART_TEST_FULL)

# Do you want JIT tests run?
ART_TEST_JIT = _getEnvBoolean('ART_TEST_JIT', ART_TEST_FULL)

# Do you want optimizing compiler tests run?
ART_TEST_OPTIMIZING = _getEnvBoolean('ART_TEST_OPTIMIZING', ART_TEST_FULL)

# Do you want to test the optimizing compiler with graph coloring register allocation?
ART_TEST_OPTIMIZING_GRAPH_COLOR = _getEnvBoolean('ART_TEST_OPTIMIZING_GRAPH_COLOR', ART_TEST_FULL)

# Do you want to do run-tests with profiles?
ART_TEST_SPEED_PROFILE = _getEnvBoolean('ART_TEST_SPEED_PROFILE', ART_TEST_FULL)

# Do we want to test PIC-compiled tests ("apps")?
ART_TEST_PIC_TEST = _getEnvBoolean('ART_TEST_PIC_TEST', ART_TEST_FULL)
# Do you want tracing tests run?
ART_TEST_TRACE = _getEnvBoolean('ART_TEST_TRACE', ART_TEST_FULL)

# Do you want tracing tests (streaming mode) run?
ART_TEST_TRACE_STREAM = _getEnvBoolean('ART_TEST_TRACE_STREAM', ART_TEST_FULL)

# Do you want tests with GC verification enabled run?
ART_TEST_GC_VERIFY = _getEnvBoolean('ART_TEST_GC_VERIFY', ART_TEST_FULL)

# Do you want tests with the GC stress mode enabled run?
ART_TEST_GC_STRESS = _getEnvBoolean('ART_TEST_GC_STRESS', ART_TEST_FULL)

# Do you want tests with the JNI forcecopy mode enabled run?
ART_TEST_JNI_FORCECOPY = _getEnvBoolean('ART_TEST_JNI_FORCECOPY', ART_TEST_FULL)

# Do you want run-tests with relocation disabled run?
ART_TEST_RUN_TEST_RELOCATE = _getEnvBoolean('ART_TEST_RUN_TEST_RELOCATE', ART_TEST_FULL)

# Do you want run-tests with prebuilding?
ART_TEST_RUN_TEST_PREBUILD = _getEnvBoolean('ART_TEST_RUN_TEST_PREBUILD', ART_TEST_FULL)

# Do you want run-tests with no prebuilding enabled run?
ART_TEST_RUN_TEST_NO_PREBUILD = _getEnvBoolean('ART_TEST_RUN_TEST_NO_PREBUILD', ART_TEST_FULL)

# Do you want run-tests with a pregenerated core.art?
ART_TEST_RUN_TEST_IMAGE = _getEnvBoolean('ART_TEST_RUN_TEST_IMAGE', ART_TEST_FULL)

# Do you want run-tests without a pregenerated core.art?
ART_TEST_RUN_TEST_NO_IMAGE = _getEnvBoolean('ART_TEST_RUN_TEST_NO_IMAGE', ART_TEST_FULL)

# Do you want run-tests with relocation enabled but patchoat failing?
ART_TEST_RUN_TEST_RELOCATE_NO_PATCHOAT = _getEnvBoolean('ART_TEST_RUN_TEST_RELOCATE_NO_PATCHOAT',
                                                       ART_TEST_FULL)

# Do you want run-tests without a dex2oat?
ART_TEST_RUN_TEST_NO_DEX2OAT = _getEnvBoolean('ART_TEST_RUN_TEST_NO_DEX2OAT', ART_TEST_FULL)

# Do you want run-tests with libartd.so?
ART_TEST_RUN_TEST_DEBUG = _getEnvBoolean('ART_TEST_RUN_TEST_DEBUG', ART_TEST_FULL)

# Do you want run-tests with libart.so?
ART_TEST_RUN_TEST_NDEBUG = _getEnvBoolean('ART_TEST_RUN_TEST_NDEBUG', ART_TEST_FULL)

# Do you want failed tests to have their artifacts cleaned up?
ART_TEST_RUN_TEST_ALWAYS_CLEAN = _getEnvBoolean('ART_TEST_RUN_TEST_ALWAYS_CLEAN', True)

# Do you want run-tests with the --debuggable flag
ART_TEST_RUN_TEST_DEBUGGABLE = _getEnvBoolean('ART_TEST_RUN_TEST_DEBUGGABLE', ART_TEST_FULL)

# Do you want to test multi-part boot-image functionality?
ART_TEST_RUN_TEST_MULTI_IMAGE = _getEnvBoolean('ART_TEST_RUN_TEST_MULTI_IMAGE', ART_TEST_FULL)

ART_TEST_DEBUG_GC = _getEnvBoolean('ART_TEST_DEBUG_GC', False)

ART_TEST_BISECTION = _getEnvBoolean('ART_TEST_BISECTION', False)

DEX2OAT_HOST_INSTRUCTION_SET_FEATURES = _env.get('DEX2OAT_HOST_INSTRUCTION_SET_FEATURES')

# Do you want run-tests with the host/target's second arch?
ART_TEST_RUN_TEST_2ND_ARCH = _getEnvBoolean('ART_TEST_RUN_TEST_2ND_ARCH', True)

HOST_2ND_ARCH_PREFIX = _get_build_var('HOST_2ND_ARCH_PREFIX')
HOST_2ND_ARCH_PREFIX_DEX2OAT_HOST_INSTRUCTION_SET_FEATURES = _env.get(
  HOST_2ND_ARCH_PREFIX + 'DEX2OAT_HOST_INSTRUCTION_SET_FEATURES')

ART_TEST_ANDROID_ROOT = _env.get('ART_TEST_ANDROID_ROOT')

ART_TEST_WITH_STRACE = _getEnvBoolean('ART_TEST_DEBUG_GC', False)

EXTRA_DISABLED_TESTS = set(_env.get("ART_TEST_RUN_TEST_SKIP", "").split())

ART_TEST_RUN_TEST_BUILD = _getEnvBoolean('ART_TEST_RUN_TEST_BUILD', False)

TARGET_2ND_ARCH = _get_build_var('TARGET_2ND_ARCH')
TARGET_ARCH = _get_build_var('TARGET_ARCH')

# Note: ART_2ND_PHONY_TEST_TARGET_SUFFIX is 2ND_ART_PHONY_TEST_TARGET_SUFFIX in .mk files
# Note: ART_2ND_PHONY_TEST_HOST_SUFFIX is 2ND_ART_PHONY_HOST_TARGET_SUFFIX in .mk files
# Python does not let us have variable names starting with a digit, so it has differ.

ART_TEST_RUN_TEST_JVMTI_STRESS = _getEnvBoolean('ART_TEST_RUN_TEST_JVMTI_STRESS', ART_TEST_FULL)

if TARGET_2ND_ARCH:
  if "64" in TARGET_ARCH:
    ART_PHONY_TEST_TARGET_SUFFIX = "64"
    ART_2ND_PHONY_TEST_TARGET_SUFFIX = "32"
  else:
    ART_PHONY_TEST_TARGET_SUFFIX = "32"
    ART_2ND_PHONY_TEST_TARGET_SUFFIX = ""
else:
  if "64" in TARGET_ARCH:
    ART_PHONY_TEST_TARGET_SUFFIX = "64"
    ART_2ND_PHONY_TEST_TARGET_SUFFIX = ""
  else:
    ART_PHONY_TEST_TARGET_SUFFIX = "32"
    ART_2ND_PHONY_TEST_TARGET_SUFFIX = ""

HOST_PREFER_32_BIT = _get_build_var('HOST_PREFER_32_BIT')
if HOST_PREFER_32_BIT == "true":
  ART_PHONY_TEST_HOST_SUFFIX = "32"
  ART_2ND_PHONY_TEST_HOST_SUFFIX = ""
else:
  ART_PHONY_TEST_HOST_SUFFIX = "64"
  ART_2ND_PHONY_TEST_HOST_SUFFIX = "32"

HOST_OUT_EXECUTABLES = os.path.join(ANDROID_BUILD_TOP,
                                    _get_build_var("HOST_OUT_EXECUTABLES"))

# Set up default values for $JACK, $DX, $SMALI, etc to the $HOST_OUT_EXECUTABLES/$name path.
for tool in ['jack', 'dx', 'smali', 'jasmin', 'dxmerger']:
  binary = tool if tool != 'dxmerger' else 'dexmerger'
  os.environ.setdefault(tool.upper(), HOST_OUT_EXECUTABLES + '/' + binary)

ANDROID_JAVA_TOOLCHAIN = os.path.join(ANDROID_BUILD_TOP,
                                     _get_build_var('ANDROID_JAVA_TOOLCHAIN'))

# include platform prebuilt java, javac, etc in $PATH.
os.environ['PATH'] = ANDROID_JAVA_TOOLCHAIN + ':' + os.environ['PATH']
