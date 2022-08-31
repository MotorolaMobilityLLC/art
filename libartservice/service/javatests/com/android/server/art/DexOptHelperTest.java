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

import static com.android.server.art.model.OptimizeResult.DexFileOptimizeResult;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyLong;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.inOrder;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.same;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.verifyNoMoreInteractions;
import static org.mockito.Mockito.when;

import android.apphibernation.AppHibernationManager;
import android.os.PowerManager;

import androidx.test.filters.SmallTest;

import com.android.server.art.model.OptimizeOptions;
import com.android.server.art.model.OptimizeResult;
import com.android.server.art.testing.OnSuccessRule;
import com.android.server.art.wrapper.AndroidPackageApi;
import com.android.server.art.wrapper.PackageState;
import com.android.server.pm.snapshot.PackageDataSnapshot;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InOrder;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import java.util.List;

@SmallTest
@RunWith(MockitoJUnitRunner.StrictStubs.class)
public class DexOptHelperTest {
    private static final String PKG_NAME = "com.example.foo";

    @Mock private DexOptHelper.Injector mInjector;
    @Mock private PrimaryDexOptimizer mPrimaryDexOptimizer;
    @Mock private AppHibernationManager mAhm;
    @Mock private PowerManager mPowerManager;
    private PackageState mPkgState;
    private AndroidPackageApi mPkg;

    @Rule
    public OnSuccessRule onSuccessRule = new OnSuccessRule(() -> {
        // Don't do this on failure because it will make the failure hard to understand.
        verifyNoMoreInteractions(mPrimaryDexOptimizer);
    });

    private final OptimizeOptions mOptions =
            new OptimizeOptions.Builder("install").setCompilerFilter("speed-profile").build();
    private final List<DexFileOptimizeResult> mPrimaryResults =
            List.of(new DexFileOptimizeResult("/data/app/foo/base.apk", "arm64", "verify",
                            OptimizeResult.OPTIMIZE_PERFORMED),
                    new DexFileOptimizeResult("/data/app/foo/base.apk", "arm", "verify",
                            OptimizeResult.OPTIMIZE_FAILED));

    private DexOptHelper mDexOptHelper;

    @Before
    public void setUp() throws Exception {
        lenient().when(mInjector.getPrimaryDexOptimizer()).thenReturn(mPrimaryDexOptimizer);
        lenient().when(mInjector.getAppHibernationManager()).thenReturn(null);
        lenient().when(mInjector.getPowerManager()).thenReturn(null);

        mPkgState = createPackageState();
        mPkg = mPkgState.getAndroidPackage();

        mDexOptHelper = new DexOptHelper(mInjector);
    }

    @Test
    public void testDexopt() throws Exception {
        when(mPrimaryDexOptimizer.dexopt(same(mPkgState), same(mPkg), same(mOptions)))
                .thenReturn(mPrimaryResults);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        assertThat(result.getPackageName()).isEqualTo(PKG_NAME);
        assertThat(result.getRequestedCompilerFilter()).isEqualTo("speed-profile");
        assertThat(result.getReason()).isEqualTo("install");
        assertThat(result.getFinalStatus()).isEqualTo(OptimizeResult.OPTIMIZE_FAILED);
        assertThat(result.getDexFileOptimizeResults()).containsExactlyElementsIn(mPrimaryResults);
    }

    @Test
    public void testDexoptNoCode() throws Exception {
        when(mPkg.isHasCode()).thenReturn(false);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        assertThat(result.getFinalStatus()).isEqualTo(OptimizeResult.OPTIMIZE_SKIPPED);
        assertThat(result.getDexFileOptimizeResults()).isEmpty();
    }

    @Test
    public void testDexoptWithAppHibernationManager() throws Exception {
        when(mInjector.getAppHibernationManager()).thenReturn(mAhm);
        lenient().when(mAhm.isHibernatingGlobally(PKG_NAME)).thenReturn(false);
        lenient().when(mAhm.isOatArtifactDeletionEnabled()).thenReturn(true);

        when(mPrimaryDexOptimizer.dexopt(same(mPkgState), same(mPkg), same(mOptions)))
                .thenReturn(mPrimaryResults);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        assertThat(result.getDexFileOptimizeResults()).containsExactlyElementsIn(mPrimaryResults);
    }

    @Test
    public void testDexoptIsHibernating() throws Exception {
        when(mInjector.getAppHibernationManager()).thenReturn(mAhm);
        lenient().when(mAhm.isHibernatingGlobally(PKG_NAME)).thenReturn(true);
        lenient().when(mAhm.isOatArtifactDeletionEnabled()).thenReturn(true);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        assertThat(result.getFinalStatus()).isEqualTo(OptimizeResult.OPTIMIZE_SKIPPED);
        assertThat(result.getDexFileOptimizeResults()).isEmpty();
    }

    @Test
    public void testDexoptIsHibernatingButOatArtifactDeletionDisabled() throws Exception {
        when(mInjector.getAppHibernationManager()).thenReturn(mAhm);
        lenient().when(mAhm.isHibernatingGlobally(PKG_NAME)).thenReturn(true);
        lenient().when(mAhm.isOatArtifactDeletionEnabled()).thenReturn(false);

        when(mPrimaryDexOptimizer.dexopt(same(mPkgState), same(mPkg), same(mOptions)))
                .thenReturn(mPrimaryResults);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        assertThat(result.getDexFileOptimizeResults()).containsExactlyElementsIn(mPrimaryResults);
    }

    @Test
    public void testDexoptWithPowerManager() throws Exception {
        var wakeLock = mock(PowerManager.WakeLock.class);
        when(mInjector.getPowerManager()).thenReturn(mPowerManager);
        when(mPowerManager.newWakeLock(eq(PowerManager.PARTIAL_WAKE_LOCK), any()))
                .thenReturn(wakeLock);

        when(mPrimaryDexOptimizer.dexopt(same(mPkgState), same(mPkg), same(mOptions)))
                .thenReturn(mPrimaryResults);

        OptimizeResult result =
                mDexOptHelper.dexopt(mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);

        InOrder inOrder = inOrder(mPrimaryDexOptimizer, wakeLock);
        inOrder.verify(wakeLock).acquire(anyLong());
        inOrder.verify(mPrimaryDexOptimizer).dexopt(any(), any(), any());
        inOrder.verify(wakeLock).release();
    }

    @Test
    public void testDexoptAlwaysReleasesWakeLock() throws Exception {
        var wakeLock = mock(PowerManager.WakeLock.class);
        when(mInjector.getPowerManager()).thenReturn(mPowerManager);
        when(mPowerManager.newWakeLock(eq(PowerManager.PARTIAL_WAKE_LOCK), any()))
                .thenReturn(wakeLock);

        when(mPrimaryDexOptimizer.dexopt(same(mPkgState), same(mPkg), same(mOptions)))
                .thenThrow(IllegalStateException.class);

        try {
            OptimizeResult result = mDexOptHelper.dexopt(
                    mock(PackageDataSnapshot.class), mPkgState, mPkg, mOptions);
        } catch (Exception e) {
        }

        verify(wakeLock).release();
    }

    private AndroidPackageApi createPackage() {
        AndroidPackageApi pkg = mock(AndroidPackageApi.class);
        lenient().when(pkg.getUid()).thenReturn(12345);
        lenient().when(pkg.isHasCode()).thenReturn(true);
        return pkg;
    }

    private PackageState createPackageState() {
        PackageState pkgState = mock(PackageState.class);
        lenient().when(pkgState.getPackageName()).thenReturn(PKG_NAME);
        AndroidPackageApi pkg = createPackage();
        lenient().when(pkgState.getAndroidPackage()).thenReturn(pkg);
        return pkgState;
    }
}
