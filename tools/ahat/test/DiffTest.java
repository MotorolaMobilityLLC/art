/*
 * Copyright (C) 2016 The Android Open Source Project
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

package com.android.ahat;

import com.android.ahat.heapdump.AhatHeap;
import com.android.ahat.heapdump.AhatInstance;
import com.android.ahat.heapdump.AhatSnapshot;
import com.android.ahat.heapdump.Diff;
import com.android.tools.perflib.heap.hprof.HprofClassDump;
import com.android.tools.perflib.heap.hprof.HprofConstant;
import com.android.tools.perflib.heap.hprof.HprofDumpRecord;
import com.android.tools.perflib.heap.hprof.HprofHeapDump;
import com.android.tools.perflib.heap.hprof.HprofInstanceDump;
import com.android.tools.perflib.heap.hprof.HprofInstanceField;
import com.android.tools.perflib.heap.hprof.HprofLoadClass;
import com.android.tools.perflib.heap.hprof.HprofPrimitiveArrayDump;
import com.android.tools.perflib.heap.hprof.HprofRecord;
import com.android.tools.perflib.heap.hprof.HprofRootDebugger;
import com.android.tools.perflib.heap.hprof.HprofStaticField;
import com.android.tools.perflib.heap.hprof.HprofStringBuilder;
import com.android.tools.perflib.heap.hprof.HprofType;
import com.google.common.io.ByteArrayDataOutput;
import com.google.common.io.ByteStreams;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

public class DiffTest {
  @Test
  public void diffMatchedHeap() throws IOException {
    TestDump dump = TestDump.getTestDump();
    AhatHeap a = dump.getAhatSnapshot().getHeap("app");
    assertNotNull(a);
    AhatHeap b = dump.getBaselineAhatSnapshot().getHeap("app");
    assertNotNull(b);
    assertEquals(a.getBaseline(), b);
    assertEquals(b.getBaseline(), a);
  }

  @Test
  public void diffUnchanged() throws IOException {
    TestDump dump = TestDump.getTestDump();

    AhatInstance a = dump.getDumpedAhatInstance("unchangedObject");
    assertNotNull(a);

    AhatInstance b = dump.getBaselineDumpedAhatInstance("unchangedObject");
    assertNotNull(b);
    assertEquals(a, b.getBaseline());
    assertEquals(b, a.getBaseline());
    assertEquals(a.getSite(), b.getSite().getBaseline());
    assertEquals(b.getSite(), a.getSite().getBaseline());
  }

  @Test
  public void diffAdded() throws IOException {
    TestDump dump = TestDump.getTestDump();

    AhatInstance a = dump.getDumpedAhatInstance("addedObject");
    assertNotNull(a);
    assertNull(dump.getBaselineDumpedAhatInstance("addedObject"));
    assertTrue(a.getBaseline().isPlaceHolder());
  }

  @Test
  public void diffRemoved() throws IOException {
    TestDump dump = TestDump.getTestDump();

    assertNull(dump.getDumpedAhatInstance("removedObject"));
    AhatInstance b = dump.getBaselineDumpedAhatInstance("removedObject");
    assertNotNull(b);
    assertTrue(b.getBaseline().isPlaceHolder());
  }

  @Test
  public void diffClassRemoved() throws IOException {
    TestDump dump = TestDump.getTestDump("O.hprof", "L.hprof", null);
    AhatHandler handler = new ObjectsHandler(dump.getAhatSnapshot());
    TestHandler.testNoCrash(handler, "http://localhost:7100/objects?class=java.lang.Class");
  }

  @Test
  public void nullClassObj() throws IOException {
    // Set up a heap dump that has a null classObj.
    // The heap dump is derived from the InstanceTest.asStringEmbedded test.
    HprofStringBuilder strings = new HprofStringBuilder(0);
    List<HprofRecord> records = new ArrayList<HprofRecord>();
    List<HprofDumpRecord> dump = new ArrayList<HprofDumpRecord>();

    final int stringClassObjectId = 1;
    records.add(new HprofLoadClass(0, 0, stringClassObjectId, 0, strings.get("java.lang.String")));
    dump.add(new HprofClassDump(stringClassObjectId, 0, 0, 0, 0, 0, 0, 0, 0,
          new HprofConstant[0], new HprofStaticField[0],
          new HprofInstanceField[]{
            new HprofInstanceField(strings.get("count"), HprofType.TYPE_INT),
            new HprofInstanceField(strings.get("hashCode"), HprofType.TYPE_INT),
            new HprofInstanceField(strings.get("offset"), HprofType.TYPE_INT),
            new HprofInstanceField(strings.get("value"), HprofType.TYPE_OBJECT)}));

    dump.add(new HprofPrimitiveArrayDump(0x41, 0, HprofType.TYPE_CHAR,
          new long[]{'n', 'o', 't', ' ', 'h', 'e', 'l', 'l', 'o', 'o', 'p'}));

    ByteArrayDataOutput values = ByteStreams.newDataOutput();
    values.writeInt(5);     // count
    values.writeInt(0);     // hashCode
    values.writeInt(4);     // offset
    values.writeInt(0x41);  // value
    dump.add(new HprofInstanceDump(0x42, 0, stringClassObjectId, values.toByteArray()));
    dump.add(new HprofRootDebugger(stringClassObjectId));
    dump.add(new HprofRootDebugger(0x42));

    records.add(new HprofHeapDump(0, dump.toArray(new HprofDumpRecord[0])));
    AhatSnapshot snapshot = SnapshotBuilder.makeSnapshot(strings, records);

    // Diffing should not crash.
    Diff.snapshots(snapshot, snapshot);
  }
}
