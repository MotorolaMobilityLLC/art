/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>

#include "logging.h"
#include "stringprintf.h"
#include "utils.h"

namespace art {

void LogMessage::LogLine(const LogMessageData& data, const char* message) {
  char severity = "VDIWEFF"[data.severity];
  fprintf(stderr, "%s %c %5d %5d %s:%d] %s\n",
          ProgramInvocationShortName(), severity, getpid(), ::art::GetTid(),
          data.file, data.line_number, message);
}

}  // namespace art
