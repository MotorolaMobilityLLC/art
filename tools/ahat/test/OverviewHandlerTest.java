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

import com.android.ahat.heapdump.AhatSnapshot;
import java.io.File;
import java.io.IOException;
import org.junit.Test;

public class OverviewHandlerTest {

  @Test
  public void noCrash() throws IOException {
    AhatSnapshot snapshot = TestDump.getTestDump().getAhatSnapshot();
    AhatHandler handler = new OverviewHandler(snapshot,
        new File("my.hprof.file"),
        new File("my.base.hprof.file"));
    TestHandler.testNoCrash(handler, "http://localhost:7100");
  }
}
