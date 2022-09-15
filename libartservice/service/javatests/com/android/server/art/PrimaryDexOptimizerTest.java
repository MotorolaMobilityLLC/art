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

package com.android.server.art;

import static com.android.server.art.GetDexoptNeededResult.ArtifactsLocation;
import static com.android.server.art.testing.TestingUtils.deepEq;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyInt;
import static org.mockito.Mockito.argThat;
import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.inOrder;
import static org.mockito.Mockito.isNull;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import android.os.ServiceSpecificException;

import androidx.test.filters.SmallTest;
import androidx.test.runner.AndroidJUnit4;

import com.android.server.art.model.OptimizeParams;
import com.android.server.art.testing.TestingUtils;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InOrder;

import java.util.ArrayList;
import java.util.List;

@SmallTest
@RunWith(AndroidJUnit4.class)
public class PrimaryDexOptimizerTest extends PrimaryDexOptimizerTestBase {
    private final OptimizeParams mOptimizeParams =
            new OptimizeParams.Builder("install").setCompilerFilter("speed-profile").build();

    private final String mDexPath = "/data/app/foo/base.apk";
    private final ProfilePath mRefProfile = AidlUtils.buildProfilePathForRef(PKG_NAME, "primary");
    private final ProfilePath mPrebuiltProfile = AidlUtils.buildProfilePathForPrebuilt(mDexPath);
    private final ProfilePath mDmProfile = AidlUtils.buildProfilePathForDm(mDexPath);
    private final OutputProfile mPublicOutputProfile = AidlUtils.buildOutputProfile(
            PKG_NAME, "primary", UID, SHARED_GID, true /* isOtherReadable */);
    private final OutputProfile mPrivateOutputProfile = AidlUtils.buildOutputProfile(
            PKG_NAME, "primary", UID, SHARED_GID, false /* isOtherReadable */);

    private final String mSplit0DexPath = "/data/app/foo/split_0.apk";
    private final ProfilePath mSplit0RefProfile =
            AidlUtils.buildProfilePathForRef(PKG_NAME, "split_0.split");
    private final OutputProfile mSplit0PrivateOutputProfile = AidlUtils.buildOutputProfile(
            PKG_NAME, "split_0.split", UID, SHARED_GID, false /* isOtherReadable */);

    private final int mDefaultDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
            | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
    private final int mForceDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
            | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE
            | DexoptTrigger.COMPILER_FILTER_IS_SAME | DexoptTrigger.COMPILER_FILTER_IS_WORSE;

    private final DexoptResult mDexoptResult =
            createDexoptResult(false /* cancelled */, 200 /* wallTimeMs */, 200 /* cpuTimeMs */);

    private List<ProfilePath> mUsedProfiles;

    @Before
    public void setUp() throws Exception {
        super.setUp();

        // By default, none of the profiles are usable.
        lenient().when(mArtd.isProfileUsable(any(), any())).thenReturn(false);
        lenient().when(mArtd.copyAndRewriteProfile(any(), any(), any())).thenReturn(false);

        // Dexopt is by default needed and successful.
        lenient()
                .when(mArtd.getDexoptNeeded(any(), any(), any(), any(), anyInt()))
                .thenReturn(dexoptIsNeeded());
        lenient()
                .when(mArtd.dexopt(
                        any(), any(), any(), any(), any(), any(), any(), anyInt(), any()))
                .thenReturn(mDexoptResult);

        mUsedProfiles = new ArrayList<>();
    }

    @Test
    public void testDexoptInputVdex() throws Exception {
        // null.
        doReturn(dexoptIsNeeded(ArtifactsLocation.NONE_OR_ERROR))
                .when(mArtd)
                .getDexoptNeeded(eq(mDexPath), eq("arm64"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mDexPath), eq("arm64"), any(), any(), any(), isNull(), anyInt(),
                        any());

        // ArtifactsPath, isInDalvikCache=true.
        doReturn(dexoptIsNeeded(ArtifactsLocation.DALVIK_CACHE))
                .when(mArtd)
                .getDexoptNeeded(eq(mDexPath), eq("arm"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mDexPath), eq("arm"), any(), any(), any(),
                        deepEq(VdexPath.artifactsPath(AidlUtils.buildArtifactsPath(
                                mDexPath, "arm", true /* isInDalvikCache */))),
                        anyInt(), any());

        // ArtifactsPath, isInDalvikCache=false.
        doReturn(dexoptIsNeeded(ArtifactsLocation.NEXT_TO_DEX))
                .when(mArtd)
                .getDexoptNeeded(eq(mSplit0DexPath), eq("arm64"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mSplit0DexPath), eq("arm64"), any(), any(), any(),
                        deepEq(VdexPath.artifactsPath(AidlUtils.buildArtifactsPath(
                                mSplit0DexPath, "arm64", false /* isInDalvikCache */))),
                        anyInt(), any());

        // DexMetadataPath.
        doReturn(dexoptIsNeeded(ArtifactsLocation.DM))
                .when(mArtd)
                .getDexoptNeeded(eq(mSplit0DexPath), eq("arm"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mSplit0DexPath), eq("arm"), any(), any(), any(),
                        deepEq(VdexPath.dexMetadataPath(
                                AidlUtils.buildDexMetadataPath(mSplit0DexPath))),
                        anyInt(), any());

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);
    }

    @Test
    public void testDexoptUsesRefProfile() throws Exception {
        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        // Other profiles are also usable, but they shouldn't be used.
        makeProfileUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        InOrder inOrder = inOrder(mArtd);

        inOrder.verify(mArtd).copyProfile(deepEq(mRefProfile), deepEq(mPrivateOutputProfile));

        inOrder.verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithPrivateProfile(
                inOrder.verify(mArtd), mDexPath, "arm64", mPrivateOutputProfile);

        inOrder.verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithPrivateProfile(
                inOrder.verify(mArtd), mDexPath, "arm", mPrivateOutputProfile);

        inOrder.verify(mArtd).commitTmpProfile(deepEq(mPrivateOutputProfile.profilePath));

        // There is no profile for split 0, so it should fall back to "verify".
        inOrder.verify(mArtd).getDexoptNeeded(
                eq(mSplit0DexPath), eq("arm64"), any(), eq("verify"), eq(mDefaultDexoptTrigger));
        checkDexoptWithNoProfile(inOrder.verify(mArtd), mSplit0DexPath, "arm64", "verify");

        inOrder.verify(mArtd).getDexoptNeeded(
                eq(mSplit0DexPath), eq("arm"), any(), eq("verify"), eq(mDefaultDexoptTrigger));
        checkDexoptWithNoProfile(inOrder.verify(mArtd), mSplit0DexPath, "arm", "verify");

        verifyProfileNotUsed(mPrebuiltProfile);
        verifyProfileNotUsed(mDmProfile);
    }

    @Test
    public void testDexoptUsesPublicRefProfile() throws Exception {
        // The ref profile is usable and public.
        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        // Other profiles are also usable, but they shouldn't be used.
        makeProfileUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).copyProfile(deepEq(mRefProfile), deepEq(mPublicOutputProfile));

        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm64", mPublicOutputProfile);
        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm", mPublicOutputProfile);

        verifyProfileNotUsed(mPrebuiltProfile);
        verifyProfileNotUsed(mDmProfile);
    }

    @Test
    public void testDexoptUsesPrebuiltProfile() throws Exception {
        makeProfileNotUsable(mRefProfile);
        makeProfileUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).copyAndRewriteProfile(
                deepEq(mPrebuiltProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm64", mPublicOutputProfile);
        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm", mPublicOutputProfile);

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mDmProfile);
    }

    @Test
    public void testDexoptUsesDmProfile() throws Exception {
        makeProfileNotUsable(mRefProfile);
        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).copyAndRewriteProfile(
                deepEq(mDmProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm64", mPublicOutputProfile);
        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm", mPublicOutputProfile);

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mPrebuiltProfile);
    }

    @Test
    public void testDexoptDeletesProfileOnFailure() throws Exception {
        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        when(mArtd.dexopt(any(), eq(mDexPath), any(), any(), any(), any(), any(), anyInt(), any()))
                .thenThrow(ServiceSpecificException.class);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).deleteProfile(
                deepEq(ProfilePath.tmpRefProfilePath(mPrivateOutputProfile.profilePath)));
        verify(mArtd, never()).commitTmpProfile(deepEq(mPrivateOutputProfile.profilePath));
    }

    @Test
    public void testDexoptNeedsToBeShared() throws Exception {
        when(mInjector.isUsedByOtherApps(PKG_NAME)).thenReturn(true);

        // The ref profile is usable but shouldn't be used.
        makeProfileUsable(mRefProfile);

        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        // The existing artifacts are private.
        when(mArtd.getArtifactsVisibility(
                     argThat(artifactsPath -> artifactsPath.dexPath == mDexPath)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).copyAndRewriteProfile(
                deepEq(mDmProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        // It should re-compile anyway.
        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mForceDexoptTrigger));
        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm64", mPublicOutputProfile);

        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mForceDexoptTrigger));
        checkDexoptWithPublicProfile(verify(mArtd), mDexPath, "arm", mPublicOutputProfile);

        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm64", "speed");
        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm", "speed");

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mPrebuiltProfile);
    }

    @Test
    public void testDexoptNeedsToBeSharedArtifactsArePublic() throws Exception {
        // Same setup as above, but the existing artifacts are public.
        when(mInjector.isUsedByOtherApps(PKG_NAME)).thenReturn(true);
        makeProfileUsable(mRefProfile);
        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);
        when(mArtd.getArtifactsVisibility(
                     argThat(artifactsPath -> artifactsPath.dexPath == mDexPath)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        // It should use the default dexopt trigger.
        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
    }

    @Test
    public void testDexoptUsesProfileForSplit() throws Exception {
        makeProfileUsable(mSplit0RefProfile);
        when(mArtd.getProfileVisibility(deepEq(mSplit0RefProfile)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt(mPkgState, mPkg, mOptimizeParams);

        verify(mArtd).copyProfile(deepEq(mSplit0RefProfile), deepEq(mSplit0PrivateOutputProfile));

        verify(mArtd).getDexoptNeeded(eq(mSplit0DexPath), eq("arm64"), any(), eq("speed-profile"),
                eq(mDefaultDexoptTrigger));
        checkDexoptWithPrivateProfile(
                verify(mArtd), mSplit0DexPath, "arm64", mSplit0PrivateOutputProfile);

        verify(mArtd).getDexoptNeeded(eq(mSplit0DexPath), eq("arm"), any(), eq("speed-profile"),
                eq(mDefaultDexoptTrigger));
        checkDexoptWithPrivateProfile(
                verify(mArtd), mSplit0DexPath, "arm", mSplit0PrivateOutputProfile);
    }

    private void checkDexoptWithPublicProfile(
            IArtd artd, String dexPath, String isa, OutputProfile profile) throws Exception {
        artd.dexopt(
                argThat(artifacts
                        -> artifacts.permissionSettings.fileFsPermission.isOtherReadable == true),
                eq(dexPath), eq(isa), any(), eq("speed-profile"),
                deepEq(ProfilePath.tmpRefProfilePath(profile.profilePath)), any(), anyInt(),
                argThat(dexoptOptions -> dexoptOptions.generateAppImage == true));
    }

    private void checkDexoptWithPrivateProfile(
            IArtd artd, String dexPath, String isa, OutputProfile profile) throws Exception {
        artd.dexopt(
                argThat(artifacts
                        -> artifacts.permissionSettings.fileFsPermission.isOtherReadable == false),
                eq(dexPath), eq(isa), any(), eq("speed-profile"),
                deepEq(ProfilePath.tmpRefProfilePath(profile.profilePath)), any(), anyInt(),
                argThat(dexoptOptions -> dexoptOptions.generateAppImage == true));
    }

    private void checkDexoptWithNoProfile(
            IArtd artd, String dexPath, String isa, String compilerFilter) throws Exception {
        artd.dexopt(
                argThat(artifacts
                        -> artifacts.permissionSettings.fileFsPermission.isOtherReadable == true),
                eq(dexPath), eq(isa), any(), eq(compilerFilter), isNull(), any(), anyInt(),
                argThat(dexoptOptions -> dexoptOptions.generateAppImage == false));
    }

    private void verifyProfileNotUsed(ProfilePath profile) throws Exception {
        assertThat(mUsedProfiles)
                .comparingElementsUsing(TestingUtils.<ProfilePath>deepEquality())
                .doesNotContain(profile);
    }

    private void makeProfileUsable(ProfilePath profile) throws Exception {
        lenient().when(mArtd.isProfileUsable(deepEq(profile), any())).thenAnswer(invocation -> {
            mUsedProfiles.add(invocation.<ProfilePath>getArgument(0));
            return true;
        });
        lenient()
                .when(mArtd.copyAndRewriteProfile(deepEq(profile), any(), any()))
                .thenAnswer(invocation -> {
                    mUsedProfiles.add(invocation.<ProfilePath>getArgument(0));
                    return true;
                });
    }

    private void makeProfileNotUsable(ProfilePath profile) throws Exception {
        lenient().when(mArtd.isProfileUsable(deepEq(profile), any())).thenReturn(false);
        lenient()
                .when(mArtd.copyAndRewriteProfile(deepEq(profile), any(), any()))
                .thenReturn(false);
    }
}
