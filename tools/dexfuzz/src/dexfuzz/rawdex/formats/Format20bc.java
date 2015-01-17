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

package dexfuzz.rawdex.formats;

import dexfuzz.rawdex.DexRandomAccessFile;
import dexfuzz.rawdex.Instruction;

import java.io.IOException;

// NB: This format is only used for statically determined verification errors,
// so we shouldn't encounter it in ART. (Or even before, as they are only written in during
// verification, which comes after our fuzzing...)
// Therefore, no need to say this implements ContainsPoolIndex, even though it is a *c format
public class Format20bc extends Format2 {
  @Override
  public void writeToFile(DexRandomAccessFile file, Instruction insn) throws IOException {
    file.writeByte((byte) insn.info.value);
    file.writeByte((byte) insn.vregA);
    file.writeUShort((short) insn.vregB);
    return;
  }

  @Override
  public long getA(byte[] raw) throws IOException {
    return RawInsnHelper.getUnsignedByteFromByte(raw, 1);
  }

  @Override
  public long getB(byte[] raw) throws IOException {
    return RawInsnHelper.getUnsignedShortFromTwoBytes(raw, 1);
  }

  @Override
  public long getC(byte[] raw) throws IOException {
    return (long) 0;
  }
}
