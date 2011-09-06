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

LogMessage::LogMessage(const char* file, int line, LogSeverity severity, int error)
: line_number_(line), severity_(severity), errno_(error)
{
  const char* last_slash = strrchr(file, '/');
  file_ = (last_slash == NULL) ? file : last_slash + 1;
}

void LogMessage::LogLine(const char* line) {
  std::cerr << "IWEF"[severity_] << ' ' << StringPrintf("%5d %5d", getpid(), ::art::GetTid()) << ' '
            << file_ << ':' << line_number_ << "] " << line << std::endl;
}
