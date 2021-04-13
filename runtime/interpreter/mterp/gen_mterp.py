#!/usr/bin/env python3
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

import sys, re, os
from io import StringIO

SCRIPT_DIR = os.path.dirname(sys.argv[0])
# This file is included verbatim at the start of the in-memory python script.
SCRIPT_SETUP_CODE = SCRIPT_DIR + "/common/gen_setup.py"
INTERP_DEFS_FILE = SCRIPT_DIR + "/../../../libdexfile/dex/dex_instruction_list.h"
NUM_PACKED_OPCODES = 256

# Extract an ordered list of instructions from the VM sources.  We use the
# "goto table" definition macro, which has exactly NUM_PACKED_OPCODES entries.
def getOpcodeList():
  opcodes = []
  opcode_fp = open(INTERP_DEFS_FILE)
  opcode_re = re.compile(r"^\s*V\((....), (\w+),.*", re.DOTALL)
  for line in opcode_fp:
    match = opcode_re.match(line)
    if not match:
      continue
    opcodes.append("op_" + match.group(2).lower())
  opcode_fp.close()

  if len(opcodes) != NUM_PACKED_OPCODES:
    print("ERROR: found ", len(opcodes), " opcodes in Interp.h (expected ", NUM_PACKED_OPCODES, ")")
    raise SyntaxError("bad opcode count")
  return opcodes

indent_re = re.compile(r"^%( *)")

# Finds variable references in text: $foo or ${foo}
escape_re = re.compile(r'''
  (?<!\$)        # Look-back: must not be preceded by another $.
  \$
  (\{)?          # May be enclosed by { } pair.
  (?P<name>\w+)  # Save the symbol in named group.
  (?(1)\})       # Expect } if and only if { was present.
''', re.VERBOSE)

def generate_script(output_filename, input_filenames):
  # Create new python script and write the initial setup code.
  script = StringIO()  # File-like in-memory buffer.
  script.write("# DO NOT EDIT: This file was generated by gen-mterp.py.\n")
  script.write(open(SCRIPT_SETUP_CODE, "r").read())
  script.write("def opcodes():\n")
  for i, opcode in enumerate(getOpcodeList()):
    script.write('  write_opcode({0}, "{1}", {1})\n'.format(i, opcode))

  # Read all template files and translate them into python code.
  for input_filename in sorted(input_filenames):
    lines = open(input_filename, "r").readlines()
    indent = ""
    for line in lines:
      line = line.rstrip()
      if line.startswith("%"):
        script.write(line.lstrip("%") + "\n")
        indent = indent_re.match(line).group(1)
        if line.endswith(":"):
          indent += "  "
      else:
        line = escape_re.sub(r"''' + \g<name> + '''", line)
        line = line.replace("\\", "\\\\")
        line = line.replace("$$", "$")
        script.write(indent + "write_line('''" + line + "''')\n")
    script.write("\n")

  script.write("generate('''" + output_filename + "''')\n")
  script.seek(0)
  return script.read()

if len(sys.argv) <= 3:
  print("Usage: output_file input_file(s)")
  sys.exit(1)

# Generate the script and execute it.
output_filename = sys.argv[1]
input_filenames = sys.argv[2:]
script_filename = output_filename + ".py"
script = generate_script(output_filename, input_filenames)
with open(script_filename, "w") as script_file:
  script_file.write(script)  # Write to disk for debugging.
exec(compile(script, script_filename, mode='exec'))
