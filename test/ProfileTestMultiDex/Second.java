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

class Second {
  public String getX() {
    return "X";
  }
  public String getY() {
    return "Y";
  }
  public String getZ() {
    return "Z";
  }
}

class SubC extends Super {
  int getValue() { return 24; }
}

class TestIntrinsicOatdump {
  Integer valueOf(int i) {
    // ProfileTestMultiDex is used also for testing oatdump for apps.
    // This is a regression test that oatdump can handle .data.bimg.rel.ro
    // entries pointing to the middle of the "boot image live objects" array.
    return Integer.valueOf(i);
  }
}
