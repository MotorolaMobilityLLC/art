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

import static com.android.server.art.AidlUtils.buildFsPermission;
import static com.android.server.art.AidlUtils.buildOutputArtifacts;
import static com.android.server.art.AidlUtils.buildPermissionSettings;
import static com.android.server.art.OutputArtifacts.PermissionSettings;
import static com.android.server.art.model.OptimizeResult.DexContainerFileOptimizeResult;
import static com.android.server.art.testing.TestingUtils.deepEq;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyBoolean;
import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.isNull;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.verifyNoMoreInteractions;
import static org.mockito.Mockito.when;

import android.os.Process;
import android.os.ServiceSpecificException;
import android.os.SystemProperties;

import androidx.test.filters.SmallTest;

import com.android.server.art.model.ArtFlags;
import com.android.server.art.model.OptimizeParams;
import com.android.server.art.model.OptimizeResult;
import com.android.server.art.testing.OnSuccessRule;
import com.android.server.art.testing.TestingUtils;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;
import org.junit.runners.Parameterized.Parameter;
import org.junit.runners.Parameterized.Parameters;

import java.util.ArrayList;
import java.util.List;

@SmallTest
@RunWith(Parameterized.class)
public class PrimaryDexOptimizerParameterizedTest extends PrimaryDexOptimizerTestBase {
    @Rule
    public OnSuccessRule onSuccessRule = new OnSuccessRule(() -> {
        // Don't do this on failure because it will make the failure hard to understand.
        verifyNoMoreInteractions(mArtd);
    });

    private OptimizeParams mOptimizeParams;

    private PrimaryDexOptimizer mPrimaryDexOptimizer;

    @Parameter(0) public Params mParams;

    @Parameters(name = "{0}")
    public static Iterable<Params> data() {
        List<Params> list = new ArrayList<>();
        Params params;

        // Baseline.
        params = new Params();
        list.add(params);

        params = new Params();
        params.mRequestedCompilerFilter = "speed";
        params.mExpectedCompilerFilter = "speed";
        list.add(params);

        params = new Params();
        params.mIsSystem = true;
        params.mExpectedIsInDalvikCache = true;
        list.add(params);

        params = new Params();
        params.mIsSystem = true;
        params.mIsUpdatedSystemApp = true;
        list.add(params);

        params = new Params();
        params.mIsSystem = true;
        params.mIsUsesNonSdkApi = true;
        params.mExpectedIsInDalvikCache = true;
        params.mExpectedIsHiddenApiPolicyEnabled = false;
        list.add(params);

        params = new Params();
        params.mIsUpdatedSystemApp = true;
        params.mIsUsesNonSdkApi = true;
        params.mExpectedIsHiddenApiPolicyEnabled = false;
        list.add(params);

        params = new Params();
        params.mIsSignedWithPlatformKey = true;
        params.mExpectedIsHiddenApiPolicyEnabled = false;
        list.add(params);

        params = new Params();
        params.mIsDebuggable = true;
        params.mRequestedCompilerFilter = "speed";
        params.mExpectedCompilerFilter = "verify";
        params.mExpectedIsDebuggable = true;
        list.add(params);

        params = new Params();
        params.mIsVmSafeMode = true;
        params.mRequestedCompilerFilter = "speed";
        params.mExpectedCompilerFilter = "verify";
        list.add(params);

        params = new Params();
        params.mAlwaysDebuggable = true;
        params.mExpectedIsDebuggable = true;
        list.add(params);

        params = new Params();
        params.mIsSystemUi = true;
        params.mExpectedCompilerFilter = "speed";
        list.add(params);

        params = new Params();
        params.mForce = true;
        params.mShouldDowngrade = false;
        params.mExpectedDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
                | DexoptTrigger.COMPILER_FILTER_IS_SAME | DexoptTrigger.COMPILER_FILTER_IS_WORSE
                | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
        list.add(params);

        params = new Params();
        params.mForce = true;
        params.mShouldDowngrade = true;
        params.mExpectedDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
                | DexoptTrigger.COMPILER_FILTER_IS_SAME | DexoptTrigger.COMPILER_FILTER_IS_WORSE
                | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
        list.add(params);

        params = new Params();
        params.mShouldDowngrade = true;
        params.mExpectedDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_WORSE;
        list.add(params);

        return list;
    }

    @Before
    public void setUp() throws Exception {
        super.setUp();

        lenient().when(mInjector.isSystemUiPackage(any())).thenReturn(mParams.mIsSystemUi);

        lenient()
                .when(SystemProperties.getBoolean(eq("dalvik.vm.always_debuggable"), anyBoolean()))
                .thenReturn(mParams.mAlwaysDebuggable);

        lenient().when(mPkg.isVmSafeMode()).thenReturn(mParams.mIsVmSafeMode);
        lenient().when(mPkg.isDebuggable()).thenReturn(mParams.mIsDebuggable);
        lenient().when(mPkg.getTargetSdkVersion()).thenReturn(123);
        lenient().when(mPkg.isSignedWithPlatformKey()).thenReturn(mParams.mIsSignedWithPlatformKey);
        lenient().when(mPkg.isUsesNonSdkApi()).thenReturn(mParams.mIsUsesNonSdkApi);
        lenient().when(mPkgState.isSystem()).thenReturn(mParams.mIsSystem);
        lenient().when(mPkgState.isUpdatedSystemApp()).thenReturn(mParams.mIsUpdatedSystemApp);

        mOptimizeParams =
                new OptimizeParams.Builder("install")
                        .setCompilerFilter(mParams.mRequestedCompilerFilter)
                        .setPriorityClass(ArtFlags.PRIORITY_INTERACTIVE)
                        .setFlags(mParams.mForce ? ArtFlags.FLAG_FORCE : 0, ArtFlags.FLAG_FORCE)
                        .setFlags(mParams.mShouldDowngrade ? ArtFlags.FLAG_SHOULD_DOWNGRADE : 0,
                                ArtFlags.FLAG_SHOULD_DOWNGRADE)
                        .build();

        mPrimaryDexOptimizer = new PrimaryDexOptimizer(
                mInjector, mPkgState, mPkg, mOptimizeParams, mCancellationSignal);
    }

    @Test
    public void testDexopt() throws Exception {
        PermissionSettings permissionSettings = buildPermissionSettings(
                buildFsPermission(Process.SYSTEM_UID, Process.SYSTEM_UID,
                        false /* isOtherReadable */, true /* isOtherExecutable */),
                buildFsPermission(Process.SYSTEM_UID, SHARED_GID, true /* isOtherReadable */),
                null /* seContext */);
        DexoptOptions dexoptOptions = new DexoptOptions();
        dexoptOptions.compilationReason = "install";
        dexoptOptions.targetSdkVersion = 123;
        dexoptOptions.debuggable = mParams.mExpectedIsDebuggable;
        dexoptOptions.generateAppImage = false;
        dexoptOptions.hiddenApiPolicyEnabled = mParams.mExpectedIsHiddenApiPolicyEnabled;

        when(mArtd.createCancellationSignal()).thenReturn(mock(IArtdCancellationSignal.class));
        when(mArtd.getDmFileVisibility(any())).thenReturn(FileVisibility.NOT_FOUND);

        // The first one is normal.
        doReturn(dexoptIsNeeded())
                .when(mArtd)
                .getDexoptNeeded("/data/app/foo/base.apk", "arm64", "PCL[]",
                        mParams.mExpectedCompilerFilter, mParams.mExpectedDexoptTrigger);
        doReturn(createDexoptResult(
                         false /* cancelled */, 100 /* wallTimeMs */, 400 /* cpuTimeMs */))
                .when(mArtd)
                .dexopt(deepEq(buildOutputArtifacts("/data/app/foo/base.apk", "arm64",
                                mParams.mExpectedIsInDalvikCache, permissionSettings)),
                        eq("/data/app/foo/base.apk"), eq("arm64"), eq("PCL[]"),
                        eq(mParams.mExpectedCompilerFilter), isNull() /* profile */,
                        isNull() /* inputVdex */, isNull() /* dmFile */,
                        eq(PriorityClass.INTERACTIVE), deepEq(dexoptOptions), any());

        // The second one fails on `dexopt`.
        doReturn(dexoptIsNeeded())
                .when(mArtd)
                .getDexoptNeeded("/data/app/foo/base.apk", "arm", "PCL[]",
                        mParams.mExpectedCompilerFilter, mParams.mExpectedDexoptTrigger);
        doThrow(ServiceSpecificException.class)
                .when(mArtd)
                .dexopt(deepEq(buildOutputArtifacts("/data/app/foo/base.apk", "arm",
                                mParams.mExpectedIsInDalvikCache, permissionSettings)),
                        eq("/data/app/foo/base.apk"), eq("arm"), eq("PCL[]"),
                        eq(mParams.mExpectedCompilerFilter), isNull() /* profile */,
                        isNull() /* inputVdex */, isNull() /* dmFile */,
                        eq(PriorityClass.INTERACTIVE), deepEq(dexoptOptions), any());

        // The third one doesn't need dexopt.
        doReturn(dexoptIsNotNeeded())
                .when(mArtd)
                .getDexoptNeeded("/data/app/foo/split_0.apk", "arm64", "PCL[base.apk]",
                        mParams.mExpectedCompilerFilter, mParams.mExpectedDexoptTrigger);

        // The fourth one is normal.
        doReturn(dexoptIsNeeded())
                .when(mArtd)
                .getDexoptNeeded("/data/app/foo/split_0.apk", "arm", "PCL[base.apk]",
                        mParams.mExpectedCompilerFilter, mParams.mExpectedDexoptTrigger);
        doReturn(createDexoptResult(
                         false /* cancelled */, 200 /* wallTimeMs */, 200 /* cpuTimeMs */))
                .when(mArtd)
                .dexopt(deepEq(buildOutputArtifacts("/data/app/foo/split_0.apk", "arm",
                                mParams.mExpectedIsInDalvikCache, permissionSettings)),
                        eq("/data/app/foo/split_0.apk"), eq("arm"), eq("PCL[base.apk]"),
                        eq(mParams.mExpectedCompilerFilter), isNull() /* profile */,
                        isNull() /* inputVdex */, isNull() /* dmFile */,
                        eq(PriorityClass.INTERACTIVE), deepEq(dexoptOptions), any());

        assertThat(mPrimaryDexOptimizer.dexopt())
                .comparingElementsUsing(TestingUtils.<DexContainerFileOptimizeResult>deepEquality())
                .containsExactly(
                        new DexContainerFileOptimizeResult("/data/app/foo/base.apk",
                                true /* isPrimaryAbi */, "arm64-v8a",
                                mParams.mExpectedCompilerFilter, OptimizeResult.OPTIMIZE_PERFORMED,
                                100 /* dex2oatWallTimeMillis */, 400 /* dex2oatCpuTimeMillis */),
                        new DexContainerFileOptimizeResult("/data/app/foo/base.apk",
                                false /* isPrimaryAbi */, "armeabi-v7a",
                                mParams.mExpectedCompilerFilter, OptimizeResult.OPTIMIZE_FAILED,
                                0 /* dex2oatWallTimeMillis */, 0 /* dex2oatCpuTimeMillis */),
                        new DexContainerFileOptimizeResult("/data/app/foo/split_0.apk",
                                true /* isPrimaryAbi */, "arm64-v8a",
                                mParams.mExpectedCompilerFilter, OptimizeResult.OPTIMIZE_SKIPPED,
                                0 /* dex2oatWallTimeMillis */, 0 /* dex2oatCpuTimeMillis */),
                        new DexContainerFileOptimizeResult("/data/app/foo/split_0.apk",
                                false /* isPrimaryAbi */, "armeabi-v7a",
                                mParams.mExpectedCompilerFilter, OptimizeResult.OPTIMIZE_PERFORMED,
                                200 /* dex2oatWallTimeMillis */, 200 /* dex2oatCpuTimeMillis */));
    }

    private static class Params {
        // Package information.
        public boolean mIsSystem = false;
        public boolean mIsUpdatedSystemApp = false;
        public boolean mIsSignedWithPlatformKey = false;
        public boolean mIsUsesNonSdkApi = false;
        public boolean mIsVmSafeMode = false;
        public boolean mIsDebuggable = false;
        public boolean mIsSystemUi = false;

        // Options.
        public String mRequestedCompilerFilter = "verify";
        public boolean mForce = false;
        public boolean mShouldDowngrade = false;

        // System properties.
        public boolean mAlwaysDebuggable = false;

        // Expectations.
        public String mExpectedCompilerFilter = "verify";
        public int mExpectedDexoptTrigger = DexoptTrigger.COMPILER_FILTER_IS_BETTER
                | DexoptTrigger.PRIMARY_BOOT_IMAGE_BECOMES_USABLE;
        public boolean mExpectedIsInDalvikCache = false;
        public boolean mExpectedIsDebuggable = false;
        public boolean mExpectedIsHiddenApiPolicyEnabled = true;

        public String toString() {
            return String.format("isSystem=%b,isUpdatedSystemApp=%b,isSignedWithPlatformKey=%b,"
                            + "isUsesNonSdkApi=%b,isVmSafeMode=%b,isDebuggable=%b,isSystemUi=%b,"
                            + "requestedCompilerFilter=%s,force=%b,shouldDowngrade=%b,"
                            + "alwaysDebuggable=%b => targetCompilerFilter=%s,"
                            + "expectedDexoptTrigger=%d,expectedIsInDalvikCache=%b,"
                            + "expectedIsDebuggable=%b,expectedIsHiddenApiPolicyEnabled=%b",
                    mIsSystem, mIsUpdatedSystemApp, mIsSignedWithPlatformKey, mIsUsesNonSdkApi,
                    mIsVmSafeMode, mIsDebuggable, mIsSystemUi, mRequestedCompilerFilter, mForce,
                    mShouldDowngrade, mAlwaysDebuggable, mExpectedCompilerFilter,
                    mExpectedDexoptTrigger, mExpectedIsInDalvikCache, mExpectedIsDebuggable,
                    mExpectedIsHiddenApiPolicyEnabled);
        }
    }
}
