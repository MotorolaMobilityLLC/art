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
import static com.android.server.art.model.OptimizeResult.DexContainerFileOptimizeResult;
import static com.android.server.art.testing.TestingUtils.deepEq;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyInt;
import static org.mockito.Mockito.argThat;
import static org.mockito.Mockito.doAnswer;
import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.inOrder;
import static org.mockito.Mockito.isNull;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.same;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import android.os.Process;
import android.os.ServiceSpecificException;
import android.os.UserHandle;

import androidx.test.filters.SmallTest;
import androidx.test.runner.AndroidJUnit4;

import com.android.server.art.model.ArtFlags;
import com.android.server.art.model.OptimizeParams;
import com.android.server.art.model.OptimizeResult;
import com.android.server.art.testing.TestingUtils;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InOrder;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;

@SmallTest
@RunWith(AndroidJUnit4.class)
public class PrimaryDexOptimizerTest extends PrimaryDexOptimizerTestBase {
    private final String mDexPath = "/data/app/foo/base.apk";
    private final ProfilePath mRefProfile =
            AidlUtils.buildProfilePathForPrimaryRef(PKG_NAME, "primary");
    private final ProfilePath mPrebuiltProfile = AidlUtils.buildProfilePathForPrebuilt(mDexPath);
    private final ProfilePath mDmProfile = AidlUtils.buildProfilePathForDm(mDexPath);
    private final DexMetadataPath mDmFile = AidlUtils.buildDexMetadataPath(mDexPath);
    private final OutputProfile mPublicOutputProfile = AidlUtils.buildOutputProfileForPrimary(
            PKG_NAME, "primary", Process.SYSTEM_UID, SHARED_GID, true /* isOtherReadable */);
    private final OutputProfile mPrivateOutputProfile = AidlUtils.buildOutputProfileForPrimary(
            PKG_NAME, "primary", Process.SYSTEM_UID, SHARED_GID, false /* isOtherReadable */);

    private final String mSplit0DexPath = "/data/app/foo/split_0.apk";
    private final ProfilePath mSplit0RefProfile =
            AidlUtils.buildProfilePathForPrimaryRef(PKG_NAME, "split_0.split");

    private final int mDefaultDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
            | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
    private final int mBetterOrSameDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
            | DexoptTrigger.COMPILER_FILTER_IS_SAME
            | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
    private final int mForceDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
            | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE
            | DexoptTrigger.COMPILER_FILTER_IS_SAME | DexoptTrigger.COMPILER_FILTER_IS_WORSE;

    private final MergeProfileOptions mMergeProfileOptions = new MergeProfileOptions();

    private final DexoptResult mDexoptResult = createDexoptResult(false /* cancelled */);

    private OptimizeParams mOptimizeParams =
            new OptimizeParams.Builder("install").setCompilerFilter("speed-profile").build();

    private PrimaryDexOptimizer mPrimaryDexOptimizer;

    private List<ProfilePath> mUsedProfiles;

    @Before
    public void setUp() throws Exception {
        super.setUp();

        // By default, none of the profiles are usable.
        lenient().when(mArtd.isProfileUsable(any(), any())).thenReturn(false);
        lenient().when(mArtd.copyAndRewriteProfile(any(), any(), any())).thenReturn(false);

        // By default, no DM file exists.
        lenient().when(mArtd.getDmFileVisibility(any())).thenReturn(FileVisibility.NOT_FOUND);

        // Dexopt is by default needed and successful.
        lenient()
                .when(mArtd.getDexoptNeeded(any(), any(), any(), any(), anyInt()))
                .thenReturn(dexoptIsNeeded());
        lenient()
                .when(mArtd.dexopt(any(), any(), any(), any(), any(), any(), any(), any(), anyInt(),
                        any(), any()))
                .thenReturn(mDexoptResult);

        lenient()
                .when(mArtd.createCancellationSignal())
                .thenReturn(mock(IArtdCancellationSignal.class));

        mPrimaryDexOptimizer = new PrimaryDexOptimizer(
                mInjector, mPkgState, mPkg, mOptimizeParams, mCancellationSignal);

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
                .dexopt(any(), eq(mDexPath), eq("arm64"), any(), any(), any(), isNull(), any(),
                        anyInt(), any(), any());

        // ArtifactsPath, isInDalvikCache=true.
        doReturn(dexoptIsNeeded(ArtifactsLocation.DALVIK_CACHE))
                .when(mArtd)
                .getDexoptNeeded(eq(mDexPath), eq("arm"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mDexPath), eq("arm"), any(), any(), any(),
                        deepEq(VdexPath.artifactsPath(AidlUtils.buildArtifactsPath(
                                mDexPath, "arm", true /* isInDalvikCache */))),
                        any(), anyInt(), any(), any());

        // ArtifactsPath, isInDalvikCache=false.
        doReturn(dexoptIsNeeded(ArtifactsLocation.NEXT_TO_DEX))
                .when(mArtd)
                .getDexoptNeeded(eq(mSplit0DexPath), eq("arm64"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mSplit0DexPath), eq("arm64"), any(), any(), any(),
                        deepEq(VdexPath.artifactsPath(AidlUtils.buildArtifactsPath(
                                mSplit0DexPath, "arm64", false /* isInDalvikCache */))),
                        any(), anyInt(), any(), any());

        // DexMetadataPath.
        doReturn(dexoptIsNeeded(ArtifactsLocation.DM))
                .when(mArtd)
                .getDexoptNeeded(eq(mSplit0DexPath), eq("arm"), any(), any(), anyInt());
        doReturn(mDexoptResult)
                .when(mArtd)
                .dexopt(any(), eq(mSplit0DexPath), eq("arm"), any(), any(), any(), isNull(), any(),
                        anyInt(), any(), any());

        mPrimaryDexOptimizer.dexopt();
    }

    @Test
    public void testDexoptDm() throws Exception {
        lenient()
                .when(mArtd.getDmFileVisibility(deepEq(mDmFile)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd, times(2))
                .dexopt(any(), eq(mDexPath), any(), any(), any(), any(), any(), deepEq(mDmFile),
                        anyInt(),
                        argThat(dexoptOptions
                                -> dexoptOptions.compilationReason.equals("install-dm")),
                        any());
        verify(mArtd, times(2))
                .dexopt(any(), eq(mSplit0DexPath), any(), any(), any(), any(), any(), isNull(),
                        anyInt(),
                        argThat(dexoptOptions -> dexoptOptions.compilationReason.equals("install")),
                        any());
    }

    @Test
    public void testDexoptUsesRefProfile() throws Exception {
        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        // Other profiles are also usable, but they shouldn't be used.
        makeProfileUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm64", mRefProfile,
                false /* isOtherReadable */, true /* generateAppImage */);

        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm", mRefProfile,
                false /* isOtherReadable */, true /* generateAppImage */);

        // There is no profile for split 0, so it should fall back to "verify".
        verify(mArtd).getDexoptNeeded(
                eq(mSplit0DexPath), eq("arm64"), any(), eq("verify"), eq(mDefaultDexoptTrigger));
        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm64", "verify");

        verify(mArtd).getDexoptNeeded(
                eq(mSplit0DexPath), eq("arm"), any(), eq("verify"), eq(mDefaultDexoptTrigger));
        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm", "verify");

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

        mPrimaryDexOptimizer.dexopt();

        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm64", mRefProfile,
                true /* isOtherReadable */, true /* generateAppImage */);
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm", mRefProfile,
                true /* isOtherReadable */, true /* generateAppImage */);

        verifyProfileNotUsed(mPrebuiltProfile);
        verifyProfileNotUsed(mDmProfile);
    }

    @Test
    public void testDexoptUsesPrebuiltProfile() throws Exception {
        makeProfileNotUsable(mRefProfile);
        makeProfileUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt();

        InOrder inOrder = inOrder(mArtd);

        inOrder.verify(mArtd).copyAndRewriteProfile(
                deepEq(mPrebuiltProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        checkDexoptWithProfile(inOrder.verify(mArtd), mDexPath, "arm64",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);
        checkDexoptWithProfile(inOrder.verify(mArtd), mDexPath, "arm",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);

        inOrder.verify(mArtd).commitTmpProfile(deepEq(mPublicOutputProfile.profilePath));

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mDmProfile);
    }

    @Test
    public void testDexoptMergesProfiles() throws Exception {
        when(mPkgState.getStateForUser(eq(UserHandle.of(0)))).thenReturn(mPkgUserStateInstalled);
        when(mPkgState.getStateForUser(eq(UserHandle.of(2)))).thenReturn(mPkgUserStateInstalled);

        when(mArtd.mergeProfiles(any(), any(), any(), any(), any())).thenReturn(true);

        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt();

        InOrder inOrder = inOrder(mArtd);

        inOrder.verify(mArtd).mergeProfiles(
                deepEq(List.of(AidlUtils.buildProfilePathForPrimaryCur(
                                       0 /* userId */, PKG_NAME, "primary"),
                        AidlUtils.buildProfilePathForPrimaryCur(
                                2 /* userId */, PKG_NAME, "primary"))),
                deepEq(mRefProfile), deepEq(mPrivateOutputProfile), deepEq(List.of(mDexPath)),
                deepEq(mMergeProfileOptions));

        // It should use `mBetterOrSameDexoptTrigger` and the merged profile for both ISAs.
        inOrder.verify(mArtd).getDexoptNeeded(eq(mDexPath), eq("arm64"), any(), eq("speed-profile"),
                eq(mBetterOrSameDexoptTrigger));
        checkDexoptWithProfile(inOrder.verify(mArtd), mDexPath, "arm64",
                ProfilePath.tmpProfilePath(mPrivateOutputProfile.profilePath),
                false /* isOtherReadable */, true /* generateAppImage */);

        inOrder.verify(mArtd).getDexoptNeeded(eq(mDexPath), eq("arm"), any(), eq("speed-profile"),
                eq(mBetterOrSameDexoptTrigger));
        checkDexoptWithProfile(inOrder.verify(mArtd), mDexPath, "arm",
                ProfilePath.tmpProfilePath(mPrivateOutputProfile.profilePath),
                false /* isOtherReadable */, true /* generateAppImage */);

        inOrder.verify(mArtd).commitTmpProfile(deepEq(mPrivateOutputProfile.profilePath));

        inOrder.verify(mArtd).deleteProfile(deepEq(
                AidlUtils.buildProfilePathForPrimaryCur(0 /* userId */, PKG_NAME, "primary")));
        inOrder.verify(mArtd).deleteProfile(deepEq(
                AidlUtils.buildProfilePathForPrimaryCur(2 /* userId */, PKG_NAME, "primary")));
    }

    @Test
    public void testDexoptMergesProfilesMergeFailed() throws Exception {
        when(mPkgState.getStateForUser(eq(UserHandle.of(0)))).thenReturn(mPkgUserStateInstalled);
        when(mPkgState.getStateForUser(eq(UserHandle.of(2)))).thenReturn(mPkgUserStateInstalled);

        when(mArtd.mergeProfiles(any(), any(), any(), any(), any())).thenReturn(false);

        makeProfileUsable(mRefProfile);
        when(mArtd.getProfileVisibility(deepEq(mRefProfile)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt();

        // It should still use "speed-profile", but with the existing reference profile only.
        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm64", mRefProfile,
                true /* isOtherReadable */, true /* generateAppImage */);

        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm", mRefProfile,
                true /* isOtherReadable */, true /* generateAppImage */);

        verify(mArtd, never()).deleteProfile(any());
        verify(mArtd, never()).commitTmpProfile(any());
    }

    @Test
    public void testDexoptUsesDmProfile() throws Exception {
        makeProfileNotUsable(mRefProfile);
        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd).copyAndRewriteProfile(
                deepEq(mDmProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm64",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mPrebuiltProfile);
    }

    @Test
    public void testDexoptDeletesProfileOnFailure() throws Exception {
        makeProfileNotUsable(mRefProfile);
        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        when(mArtd.dexopt(any(), eq(mDexPath), any(), any(), any(), any(), any(), any(), anyInt(),
                     any(), any()))
                .thenThrow(ServiceSpecificException.class);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd).deleteProfile(
                deepEq(ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath)));
        verify(mArtd, never()).commitTmpProfile(deepEq(mPublicOutputProfile.profilePath));
    }

    @Test
    public void testDexoptNeedsToBeShared() throws Exception {
        when(mDexUseManager.isPrimaryDexUsedByOtherApps(eq(PKG_NAME), eq(mDexPath)))
                .thenReturn(true);
        when(mDexUseManager.isPrimaryDexUsedByOtherApps(eq(PKG_NAME), eq(mSplit0DexPath)))
                .thenReturn(true);

        // The ref profile is usable but shouldn't be used.
        makeProfileUsable(mRefProfile);

        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);

        // The existing artifacts are private.
        when(mArtd.getArtifactsVisibility(
                     argThat(artifactsPath -> artifactsPath.dexPath == mDexPath)))
                .thenReturn(FileVisibility.NOT_OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd).copyAndRewriteProfile(
                deepEq(mDmProfile), deepEq(mPublicOutputProfile), eq(mDexPath));

        // It should re-compile anyway.
        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm64"), any(), eq("speed-profile"), eq(mForceDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm64",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);

        verify(mArtd).getDexoptNeeded(
                eq(mDexPath), eq("arm"), any(), eq("speed-profile"), eq(mForceDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mDexPath, "arm",
                ProfilePath.tmpProfilePath(mPublicOutputProfile.profilePath),
                true /* isOtherReadable */, true /* generateAppImage */);

        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm64", "speed");
        checkDexoptWithNoProfile(verify(mArtd), mSplit0DexPath, "arm", "speed");

        verifyProfileNotUsed(mRefProfile);
        verifyProfileNotUsed(mPrebuiltProfile);
    }

    @Test
    public void testDexoptNeedsToBeSharedArtifactsArePublic() throws Exception {
        // Same setup as above, but the existing artifacts are public.
        when(mDexUseManager.isPrimaryDexUsedByOtherApps(eq(PKG_NAME), eq(mDexPath)))
                .thenReturn(true);
        when(mDexUseManager.isPrimaryDexUsedByOtherApps(eq(PKG_NAME), eq(mSplit0DexPath)))
                .thenReturn(true);

        makeProfileUsable(mRefProfile);
        makeProfileNotUsable(mPrebuiltProfile);
        makeProfileUsable(mDmProfile);
        when(mArtd.getArtifactsVisibility(
                     argThat(artifactsPath -> artifactsPath.dexPath == mDexPath)))
                .thenReturn(FileVisibility.OTHER_READABLE);

        mPrimaryDexOptimizer.dexopt();

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

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd).getDexoptNeeded(eq(mSplit0DexPath), eq("arm64"), any(), eq("speed-profile"),
                eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mSplit0DexPath, "arm64", mSplit0RefProfile,
                false /* isOtherReadable */, false /* generateAppImage */);

        verify(mArtd).getDexoptNeeded(eq(mSplit0DexPath), eq("arm"), any(), eq("speed-profile"),
                eq(mDefaultDexoptTrigger));
        checkDexoptWithProfile(verify(mArtd), mSplit0DexPath, "arm", mSplit0RefProfile,
                false /* isOtherReadable */, false /* generateAppImage */);
    }

    @Test
    public void testDexoptCancelledBeforeDexopt() throws Exception {
        mCancellationSignal.cancel();

        var artdCancellationSignal = mock(IArtdCancellationSignal.class);
        when(mArtd.createCancellationSignal()).thenReturn(artdCancellationSignal);

        doAnswer(invocation -> {
            verify(artdCancellationSignal).cancel();
            return createDexoptResult(true /* cancelled */);
        })
                .when(mArtd)
                .dexopt(any(), any(), any(), any(), any(), any(), any(), any(), anyInt(), any(),
                        same(artdCancellationSignal));

        // The result should only contain one element: the result of the first file with
        // OPTIMIZE_CANCELLED.
        assertThat(mPrimaryDexOptimizer.dexopt()
                           .stream()
                           .map(DexContainerFileOptimizeResult::getStatus)
                           .collect(Collectors.toList()))
                .containsExactly(OptimizeResult.OPTIMIZE_CANCELLED);

        // It shouldn't continue after being cancelled on the first file.
        verify(mArtd, times(1)).createCancellationSignal();
        verify(mArtd, times(1))
                .dexopt(any(), any(), any(), any(), any(), any(), any(), any(), anyInt(), any(),
                        any());
    }

    @Test
    public void testDexoptCancelledDuringDexopt() throws Exception {
        Semaphore dexoptStarted = new Semaphore(0);
        Semaphore dexoptCancelled = new Semaphore(0);
        final long TIMEOUT_SEC = 1;

        var artdCancellationSignal = mock(IArtdCancellationSignal.class);
        when(mArtd.createCancellationSignal()).thenReturn(artdCancellationSignal);

        doAnswer(invocation -> {
            dexoptStarted.release();
            assertThat(dexoptCancelled.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();
            return createDexoptResult(true /* cancelled */);
        })
                .when(mArtd)
                .dexopt(any(), any(), any(), any(), any(), any(), any(), any(), anyInt(), any(),
                        same(artdCancellationSignal));
        doAnswer(invocation -> {
            dexoptCancelled.release();
            return null;
        })
                .when(artdCancellationSignal)
                .cancel();

        Future<List<DexContainerFileOptimizeResult>> results =
                Executors.newSingleThreadExecutor().submit(
                        () -> { return mPrimaryDexOptimizer.dexopt(); });

        assertThat(dexoptStarted.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();

        mCancellationSignal.cancel();

        assertThat(results.get()
                           .stream()
                           .map(DexContainerFileOptimizeResult::getStatus)
                           .collect(Collectors.toList()))
                .containsExactly(OptimizeResult.OPTIMIZE_CANCELLED);

        // It shouldn't continue after being cancelled on the first file.
        verify(mArtd, times(1)).createCancellationSignal();
        verify(mArtd, times(1))
                .dexopt(any(), any(), any(), any(), any(), any(), any(), any(), anyInt(), any(),
                        any());
    }

    @Test
    public void testDexoptBaseApk() throws Exception {
        mOptimizeParams =
                new OptimizeParams.Builder("install")
                        .setCompilerFilter("speed-profile")
                        .setFlags(ArtFlags.FLAG_FOR_PRIMARY_DEX | ArtFlags.FLAG_FOR_SINGLE_SPLIT)
                        .setSplitName(null)
                        .build();
        mPrimaryDexOptimizer = new PrimaryDexOptimizer(
                mInjector, mPkgState, mPkg, mOptimizeParams, mCancellationSignal);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd, times(2))
                .dexopt(any(), eq(mDexPath), any(), any(), any(), any(), any(), any(), anyInt(),
                        any(), any());
        verify(mArtd, never())
                .dexopt(any(), eq(mSplit0DexPath), any(), any(), any(), any(), any(), any(),
                        anyInt(), any(), any());
    }

    @Test
    public void testDexoptSplitApk() throws Exception {
        mOptimizeParams =
                new OptimizeParams.Builder("install")
                        .setCompilerFilter("speed-profile")
                        .setFlags(ArtFlags.FLAG_FOR_PRIMARY_DEX | ArtFlags.FLAG_FOR_SINGLE_SPLIT)
                        .setSplitName("split_0")
                        .build();
        mPrimaryDexOptimizer = new PrimaryDexOptimizer(
                mInjector, mPkgState, mPkg, mOptimizeParams, mCancellationSignal);

        mPrimaryDexOptimizer.dexopt();

        verify(mArtd, never())
                .dexopt(any(), eq(mDexPath), any(), any(), any(), any(), any(), any(), anyInt(),
                        any(), any());
        verify(mArtd, times(2))
                .dexopt(any(), eq(mSplit0DexPath), any(), any(), any(), any(), any(), any(),
                        anyInt(), any(), any());
    }

    private void checkDexoptWithProfile(IArtd artd, String dexPath, String isa, ProfilePath profile,
            boolean isOtherReadable, boolean generateAppImage) throws Exception {
        artd.dexopt(argThat(artifacts
                            -> artifacts.permissionSettings.fileFsPermission.isOtherReadable
                                    == isOtherReadable),
                eq(dexPath), eq(isa), any(), eq("speed-profile"), deepEq(profile), any(), any(),
                anyInt(),
                argThat(dexoptOptions -> dexoptOptions.generateAppImage == generateAppImage),
                any());
    }

    private void checkDexoptWithNoProfile(
            IArtd artd, String dexPath, String isa, String compilerFilter) throws Exception {
        artd.dexopt(
                argThat(artifacts
                        -> artifacts.permissionSettings.fileFsPermission.isOtherReadable == true),
                eq(dexPath), eq(isa), any(), eq(compilerFilter), isNull(), any(), any(), anyInt(),
                argThat(dexoptOptions -> dexoptOptions.generateAppImage == false), any());
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
