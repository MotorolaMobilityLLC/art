#!/usr/bin/env python3
#
# Copyright (C) 2021 The Android Open Source Project
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

""" This script generates the Android.run-test.bp build file"""

import os, textwrap

def main():
  test_dir = os.path.dirname(__file__)
  with open(os.path.join(test_dir, "Android.run-test.bp"), mode="wt") as f:
    f.write(textwrap.dedent("""
      // This file was generated by {}
      // It is not necessary to regenerate it when tests are added/removed/modified.
    """.format(os.path.basename(__file__))).lstrip())
    for mode in ["host", "target", "jvm"]:
      names = []
      # Group the tests into shards based on the last two digits of the test number.
      # This keeps the number of generated genrules low so we don't overwhelm soong,
      # but it still allows iterating on single test without recompiling all tests.
      for shard in ["{:02}".format(i) for i in range(100)]:
        name = "art-run-test-{mode}-data-shard{shard}".format(mode=mode, shard=shard)
        names.append(name)
        f.write(textwrap.dedent("""
          java_genrule {{
              name: "{name}-tmp",
              out: ["{name}.zip"],
              srcs: ["?{shard}-*/**/*", "??{shard}-*/**/*"],
              defaults: ["art-run-test-{mode}-data-defaults"],
          }}

          // Install in the output directory to make it accessible for tests.
          prebuilt_etc_host {{
              name: "{name}",
              defaults: ["art_module_source_build_prebuilt_defaults"],
              src: ":{name}-tmp",
              sub_dir: "art",
              filename: "{name}.zip",
          }}
          """.format(name=name, mode=mode, shard=shard)))

      f.write(textwrap.dedent("""
        genrule_defaults {{
            name: "art-run-test-{mode}-data-defaults",
            defaults: [
                // Enable only in source builds, where com.android.art.testing is
                // available.
                "art_module_source_build_genrule_defaults",
            ],
            tool_files: [
                "run_test_build.py",
                ":art-run-test-bootclasspath",
            ],
            tools: [
                "d8",
                "hiddenapi",
                "jasmin",
                "smali",
                "soong_zip",
                "zipalign",
            ],
            cmd: "$(location run_test_build.py) --out $(out) --mode {mode} " +
                 "--bootclasspath $(location :art-run-test-bootclasspath) " +
                 "--d8 $(location d8) " +
                 "--hiddenapi $(location hiddenapi) " +
                 "--jasmin $(location jasmin) " +
                 "--smali $(location smali) " +
                 "--soong_zip $(location soong_zip) " +
                 "--zipalign $(location zipalign) " +
                 "$(in)",
        }}
        """).format(mode=mode))

      name = "art-run-test-{mode}-data-merged".format(mode=mode)
      srcs = ("\n"+" "*8).join('":{}-tmp",'.format(n) for n in names)
      deps = ("\n"+" "*8).join('"{}",'.format(n) for n in names)
      f.write(textwrap.dedent("""
        java_genrule {{
            name: "{name}-tmp",
            defaults: ["art_module_source_build_genrule_defaults"],
            out: ["{name}.zip"],
            srcs: [
                {srcs}
            ],
            tools: ["merge_zips"],
            cmd: "$(location merge_zips) $(out) $(in)",
        }}

        // Install in the output directory to make it accessible for tests.
        prebuilt_etc_host {{
            name: "{name}",
            defaults: ["art_module_source_build_prebuilt_defaults"],
            src: ":{name}-tmp",
            required: [
                {deps}
            ],
            sub_dir: "art",
            filename: "{name}.zip",
        }}
        """).format(name=name, srcs=srcs, deps=deps))

      name = "art-run-test-{mode}-data".format(mode=mode)
      srcs = ("\n"+" "*8).join('":{}-tmp",'.format(n) for n in names)
      deps = ("\n"+" "*8).join('"{}",'.format(n) for n in names)
      f.write(textwrap.dedent("""
        // Phony target used to build all shards
        java_genrule {{
            name: "{name}-tmp",
            defaults: ["art-run-test-data-defaults"],
            out: ["{name}.txt"],
            srcs: [
                {srcs}
            ],
            cmd: "echo $(in) > $(out)",
        }}

        // Phony target used to install all shards
        prebuilt_etc_host {{
            name: "{name}",
            defaults: ["art_module_source_build_prebuilt_defaults"],
            src: ":{name}-tmp",
            required: [
                {deps}
            ],
            sub_dir: "art",
            filename: "{name}.txt",
        }}
        """).format(name=name, srcs=srcs, deps=deps))

if __name__ == "__main__":
  main()
