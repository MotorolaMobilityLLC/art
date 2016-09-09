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

package com.android.ahat.heapdump;

public class AhatHeap {
  private String mName;
  private long mSize = 0;
  private int mIndex;

  AhatHeap(String name, int index) {
    mName = name;
    mIndex = index;
  }

  void addToSize(long increment) {
    mSize += increment;
  }

  /**
   * Returns a unique instance for this heap between 0 and the total number of
   * heaps in this snapshot.
   */
  int getIndex() {
    return mIndex;
  }

  /**
   * Returns the name of this heap.
   */
  public String getName() {
    return mName;
  }

  /**
   * Returns the total number of bytes allocated on this heap.
   */
  public long getSize() {
    return mSize;
  }
}
