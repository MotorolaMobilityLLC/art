/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_SRC_SIGNAL_CATCHER_H_
#define ART_SRC_SIGNAL_CATCHER_H_

#include "mutex.h"

namespace art {

class Runtime;
class Thread;

/*
 * A thread that catches signals and does something useful. For
 * example, when a SIGQUIT (Ctrl-\) arrives, we suspend and dump the
 * status of all threads.
 */
class SignalCatcher {
 public:
  SignalCatcher();
  ~SignalCatcher();

  static void HandleSigQuit();

 private:
  static void* Run(void* arg);
  static void HandleSigUsr1();

  void SetHaltFlag(bool new_value);
  bool ShouldHalt();

  mutable Mutex lock_;
  bool halt_;
  pthread_cond_t cond_;
  pthread_t pthread_;
  Thread* thread_;
};

}  // namespace art

#endif  // ART_SRC_SIGNAL_CATCHER_H_
