#!/system/bin/sh

# Copyright (C) 2019 The Android Open Source Project
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

. `dirname $0`/art_prepostinstall_utils || exit 100

log_info "=== ART runtime post-install ==="

# Check for OTA base folder.
if [ ! -d /data/ota/dalvik-cache ] ; then
  log_error "Postinstall dalvik-cache does not exist or is not a directory"
  exit 101
fi

log_info "Moving dalvik-cache"

rm -rf /data/dalvik-cache/* || exit 102
mv /data/ota/dalvik-cache/* /data/dalvik-cache/ || exit 103
restorecon -R -F /data/dalvik-cache/* || exit 104
