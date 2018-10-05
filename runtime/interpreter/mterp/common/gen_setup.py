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

# Common global variables and helper methods for the in-memory python script.
# The script starts with this file and is followed by the code generated form
# the templated snippets. Those define all the helper functions used below.

import sys, re
from cStringIO import StringIO

out = StringIO()  # File-like in-memory buffer.
handler_size_bytes = "128"
handler_size_bits = "7"
opcode = ""
opnum = ""

def write_line(line):
  out.write(line + "\n")

def balign():
  write_line("    .balign {}".format(handler_size_bytes))

def write_opcode(num, name, write_method, is_alt):
  global opnum, opcode
  opnum, opcode = str(num), name
  if is_alt:
    name = "ALT_" + name
  write_line("/* ------------------------------ */")
  balign()
  write_line(".L_{1}: /* {0:#04x} */".format(num, name))
  if is_alt:
    alt_stub()
  else:
    opcode_start()
    write_method()
    opcode_end()
  write_line("")
  opnum, opcode = None, None

generated_helpers = list()

# This method generates a helper using the provided writer method.
# The output is temporarily redirected to in-memory buffer.
# It returns the symbol which can be used to jump to the helper.
def add_helper(name_suffix, write_helper):
  global out
  old_out = out
  out = StringIO()
  name = "Mterp_" + opcode + "_" + name_suffix
  helper_start(name)
  write_helper()
  helper_end(name)
  out.seek(0)
  generated_helpers.append(out.read())
  out = old_out
  return name

def generate(output_filename):
  out.seek(0)
  out.truncate()
  write_line("/* DO NOT EDIT: This file was generated by gen-mterp.py. */")
  header()
  entry()

  instruction_start()
  opcodes(is_alt = False)
  balign()
  instruction_end()

  for helper in generated_helpers:
    out.write(helper)
  helpers()

  instruction_start_alt()
  opcodes(is_alt = True)
  balign()
  instruction_end_alt()

  footer()

  out.seek(0)
  # Squash consequtive empty lines.
  text = re.sub(r"(\n\n)(\n)+", r"\1", out.read())
  with open(output_filename, 'w') as output_file:
    output_file.write(text)

