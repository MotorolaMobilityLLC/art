/*
 * Copyright (C) 2022 The Android Open Source Project
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

package com.android.server.art.wrapper;

import android.annotation.NonNull;
import android.annotation.Nullable;

import java.util.List;
import java.util.stream.Collectors;

/** @hide */
public class SharedLibraryInfo {
    private final Object mInfo;

    SharedLibraryInfo(@NonNull Object info) {
        mInfo = info;
    }

    @NonNull
    public List<String> getAllCodePaths() {
        try {
            return (List<String>) mInfo.getClass().getMethod("getAllCodePaths").invoke(mInfo);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    @Nullable
    public List<SharedLibraryInfo> getDependencies() {
        try {
            var list = (List<?>) mInfo.getClass().getMethod("getDependencies").invoke(mInfo);
            if (list == null) {
                return null;
            }
            return list.stream()
                    .map(obj -> new SharedLibraryInfo(obj))
                    .collect(Collectors.toList());
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }
}
