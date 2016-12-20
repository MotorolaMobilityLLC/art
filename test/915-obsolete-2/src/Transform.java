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

class Transform {
  private void Start() {
    System.out.println("hello - private");
  }

  private void Finish() {
    System.out.println("goodbye - private");
  }

  public void sayHi(Runnable r) {
    System.out.println("Pre Start private method call");
    Start();
    System.out.println("Post Start private method call");
    r.run();
    System.out.println("Pre Finish private method call");
    Finish();
    System.out.println("Post Finish private method call");
  }
}
