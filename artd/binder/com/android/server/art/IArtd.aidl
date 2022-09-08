/*
 * Copyright (C) 2021 The Android Open Source Project
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

package com.android.server.art;

/** @hide */
interface IArtd {
    // Test to see if the artd service is available.
    boolean isAlive();

    /**
     * Deletes artifacts and returns the released space, in bytes.
     *
     * Throws fatal errors. Logs and ignores non-fatal errors.
     */
    long deleteArtifacts(in com.android.server.art.ArtifactsPath artifactsPath);

    /**
     * Returns the optimization status of a dex file.
     *
     * Throws fatal and non-fatal errors.
     */
    com.android.server.art.GetOptimizationStatusResult getOptimizationStatus(
            @utf8InCpp String dexFile, @utf8InCpp String instructionSet,
            @utf8InCpp String classLoaderContext);

    /**
     * Returns true if the profile exists and contains entries for the given dex file.
     *
     * Throws fatal and non-fatal errors.
     */
    boolean isProfileUsable(in com.android.server.art.ProfilePath profile,
            @utf8InCpp String dexFile);

    /**
     * Copies the profile. Throws if `src` does not exist. Fills `dst.profilePath.id` on success.
     *
     * Does not operate on a DM file.
     *
     * Throws fatal and non-fatal errors.
     */
    void copyProfile(in com.android.server.art.ProfilePath src,
            inout com.android.server.art.OutputProfile dst);

    /**
     * Copies the profile and rewrites it for the given dex file. Returns true and fills
     * `dst.profilePath.id` if the operation succeeds and `src` exists and contains entries that
     * match the given dex file.
     *
     * Throws fatal and non-fatal errors.
     */
    boolean copyAndRewriteProfile(in com.android.server.art.ProfilePath src,
            inout com.android.server.art.OutputProfile dst, @utf8InCpp String dexFile);

    /**
     * Moves the temporary profile to the permanent location.
     *
     * Throws fatal and non-fatal errors.
     */
    void commitTmpProfile(in com.android.server.art.ProfilePath.TmpRefProfilePath profile);

    /**
     * Deletes the profile.
     *
     * Operates on the whole DM file if given one.
     *
     * Throws fatal errors. Logs and ignores non-fatal errors.
     */
    void deleteProfile(in com.android.server.art.ProfilePath profile);

    /**
     * Returns the visibility of the profile.
     *
     * Operates on the whole DM file if given one.
     *
     * Throws fatal and non-fatal errors.
     */
    com.android.server.art.FileVisibility getProfileVisibility(
            in com.android.server.art.ProfilePath profile);

    /**
     * Returns the visibility of the artifacts.
     *
     * Throws fatal and non-fatal errors.
     */
    com.android.server.art.FileVisibility getArtifactsVisibility(
            in com.android.server.art.ArtifactsPath artifactsPath);

    /**
     * Returns true if dexopt is needed. `dexoptTrigger` is a bit field that consists of values
     * defined in `com.android.server.art.DexoptTrigger`.
     *
     * Throws fatal and non-fatal errors.
     */
    com.android.server.art.GetDexoptNeededResult getDexoptNeeded(
            @utf8InCpp String dexFile, @utf8InCpp String instructionSet,
            @utf8InCpp String classLoaderContext, @utf8InCpp String compilerFilter,
            int dexoptTrigger);

    /**
     * Dexopts a dex file for the given instruction set. Returns true on success, or false if
     * cancelled.
     *
     * Throws fatal and non-fatal errors.
     */
    boolean dexopt(in com.android.server.art.OutputArtifacts outputArtifacts,
            @utf8InCpp String dexFile, @utf8InCpp String instructionSet,
            @utf8InCpp String classLoaderContext, @utf8InCpp String compilerFilter,
            in @nullable com.android.server.art.ProfilePath profile,
            in @nullable com.android.server.art.VdexPath inputVdex,
            com.android.server.art.PriorityClass priorityClass,
            in com.android.server.art.DexoptOptions dexoptOptions);
}
