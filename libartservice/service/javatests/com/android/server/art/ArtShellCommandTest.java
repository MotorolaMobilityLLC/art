/*
 * Copyright (C) 2025 The Android Open Source Project
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

import static com.android.server.art.PreRebootDexoptJob.JOB_ID;
import static com.android.server.art.prereboot.PreRebootDriver.PreRebootResult;

import static com.google.common.truth.Truth.assertThat;
import static com.google.common.truth.Truth.assertWithMessage;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyBoolean;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.isNull;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.os.CancellationSignal;
import android.os.Process;
import android.os.SystemProperties;

import androidx.test.filters.SmallTest;

import com.android.server.art.prereboot.PreRebootDriver;
import com.android.server.art.prereboot.PreRebootStatsReporter;
import com.android.server.art.testing.CommandExecution;
import com.android.server.art.testing.StaticMockitoRule;
import com.android.server.pm.PackageManagerLocal;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;
import java.util.stream.Stream;

@SmallTest
@RunWith(MockitoJUnitRunner.StrictStubs.class)
public class ArtShellCommandTest {
    private static final long TIMEOUT_SEC = 10;

    @Rule
    public StaticMockitoRule mockitoRule = new StaticMockitoRule(
            SystemProperties.class, BackgroundDexoptJobService.class, ArtJni.class);

    @Mock private BackgroundDexoptJobService mJobService;
    @Mock private PreRebootDriver mPreRebootDriver;
    @Mock private PreRebootStatsReporter mPreRebootStatsReporter;
    @Mock private JobScheduler mJobScheduler;
    @Mock private PreRebootDexoptJob.Injector mPreRebootDexoptJobInjector;
    @Mock private ArtManagerLocal.Injector mArtManagerLocalInjector;
    @Mock private PackageManagerLocal mPackageManagerLocal;
    @Mock private ArtShellCommand.Injector mInjector;

    private PreRebootDexoptJob mPreRebootDexoptJob;
    private ArtManagerLocal mArtManagerLocal;
    private JobInfo mJobInfo;
    private JobParameters mJobParameters;

    @Before
    public void setUp() throws Exception {
        lenient()
                .when(SystemProperties.getBoolean(eq("dalvik.vm.enable_pr_dexopt"), anyBoolean()))
                .thenReturn(true);

        lenient().when(mJobScheduler.schedule(any())).thenAnswer(invocation -> {
            mJobInfo = invocation.<JobInfo>getArgument(0);
            mJobParameters = mock(JobParameters.class);
            assertThat(mJobInfo.getId()).isEqualTo(JOB_ID);
            lenient().when(mJobParameters.getExtras()).thenReturn(mJobInfo.getExtras());
            return JobScheduler.RESULT_SUCCESS;
        });

        lenient()
                .doAnswer(invocation -> {
                    mJobInfo = null;
                    mJobParameters = null;
                    return null;
                })
                .when(mJobScheduler)
                .cancel(JOB_ID);

        lenient().when(mJobScheduler.getPendingJob(JOB_ID)).thenAnswer(invocation -> {
            return mJobInfo;
        });

        lenient()
                .when(mPreRebootDexoptJobInjector.getPreRebootDriver())
                .thenReturn(mPreRebootDriver);
        lenient()
                .when(mPreRebootDexoptJobInjector.getStatsReporter())
                .thenReturn(mPreRebootStatsReporter);
        lenient().when(mPreRebootDexoptJobInjector.getJobScheduler()).thenReturn(mJobScheduler);
        mPreRebootDexoptJob = new PreRebootDexoptJob(mPreRebootDexoptJobInjector);

        lenient().when(BackgroundDexoptJobService.getJob(JOB_ID)).thenReturn(mPreRebootDexoptJob);

        lenient()
                .when(mArtManagerLocalInjector.getPreRebootDexoptJob())
                .thenReturn(mPreRebootDexoptJob);
        mArtManagerLocal = new ArtManagerLocal(mArtManagerLocalInjector);

        lenient().when(mInjector.getArtManagerLocal()).thenReturn(mArtManagerLocal);
        lenient().when(mInjector.getPackageManagerLocal()).thenReturn(mPackageManagerLocal);
    }

    @Test
    public void testOnOtaStagedPermission() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(-1);
            assertThat(outputs).contains("Only root can call 'on-ota-staged'");
        }
    }

    @Test
    public void testOnOtaStagedSync() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(mPreRebootDriver.run(eq("_b"), eq(false) /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job finished. See logs for details");
        }
    }

    @Test
    public void testOnOtaStagedSyncFatalError() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(mPreRebootDriver.run(eq("_b"), eq(false) /* mapSnapshotsForOta */, any()))
                .thenThrow(RuntimeException.class);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job encountered a fatal error");
        }
    }

    @Test
    public void testOnOtaStagedSyncCancelledByCommand() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(mPreRebootDriver.run(eq("_b"), eq(false) /* mapSnapshotsForOta */, any()))
                .thenAnswer(invocation -> {
                    Semaphore dexoptCancelled = new Semaphore(0 /* permits */);
                    var cancellationSignal = invocation.<CancellationSignal>getArgument(2);
                    cancellationSignal.setOnCancelListener(() -> dexoptCancelled.release());
                    assertThat(dexoptCancelled.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();
                    return new PreRebootResult(true /* success */);
                });

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            assertThat(execution.getStdout().readLine()).contains("Job running...");

            try (var execution2 = new CommandExecution(
                         createHandler(), "art", "pr-dexopt-job", "--cancel")) {
                int exitCode2 = execution2.waitAndGetExitCode();
                String outputs2 = getOutputs(execution2);
                assertWithMessage(outputs2).that(exitCode2).isEqualTo(0);
                assertThat(outputs2).contains("Pre-reboot Dexopt job cancelled");
            }

            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job finished. See logs for details");
        }
    }

    @Test
    public void testOnOtaStagedSyncCancelledByBrokenPipe() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(mPreRebootDriver.run(eq("_b"), eq(false) /* mapSnapshotsForOta */, any()))
                .thenAnswer(invocation -> {
                    Semaphore dexoptCancelled = new Semaphore(0 /* permits */);
                    var cancellationSignal = invocation.<CancellationSignal>getArgument(2);
                    cancellationSignal.setOnCancelListener(() -> dexoptCancelled.release());
                    assertThat(dexoptCancelled.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();
                    return new PreRebootResult(true /* success */);
                });

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            assertThat(execution.getStdout().readLine()).contains("Job running...");

            execution.closeStdin();

            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job finished. See logs for details");
        }
    }

    @Test
    public void testOnOtaStagedAsyncLegacy() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(SystemProperties.getBoolean(eq("dalvik.vm.pr_dexopt_async_for_ota"), anyBoolean()))
                .thenReturn(true);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "on-ota-staged", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Pre-reboot Dexopt job scheduled");
        }

        when(mPreRebootDriver.run(eq("_b"), eq(true) /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        mPreRebootDexoptJob.onStartJobImpl(mJobService, mJobParameters);
        mPreRebootDexoptJob.waitForRunningJob();
    }

    @Test
    public void testPrDexoptJobRunMainline() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);

        when(mPreRebootDriver.run(
                     isNull() /* otaSlot */, anyBoolean() /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        try (var execution =
                        new CommandExecution(createHandler(), "art", "pr-dexopt-job", "--run")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job finished. See logs for details");
        }
    }

    @Test
    public void testPrDexoptJobRunOtaPermission() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "pr-dexopt-job", "--run", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(-1);
            assertThat(outputs).contains("Only root can specify '--slot'");
        }
    }

    @Test
    public void testPrDexoptJobRunOta() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        when(mPreRebootDriver.run(eq("_b"), eq(true) /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        try (var execution = new CommandExecution(
                     createHandler(), "art", "pr-dexopt-job", "--run", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Job finished. See logs for details");
        }
    }

    @Test
    public void testPrDexoptJobScheduleMainline() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "pr-dexopt-job", "--schedule")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Pre-reboot Dexopt job scheduled");
        }

        when(mPreRebootDriver.run(
                     isNull() /* otaSlot */, anyBoolean() /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        mPreRebootDexoptJob.onStartJobImpl(mJobService, mJobParameters);
        mPreRebootDexoptJob.waitForRunningJob();
    }

    @Test
    public void testPrDexoptJobScheduleOtaPermission() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "pr-dexopt-job", "--schedule", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(-1);
            assertThat(outputs).contains("Only root can specify '--slot'");
        }
    }

    @Test
    public void testPrDexoptJobScheduleOta() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        try (var execution = new CommandExecution(
                     createHandler(), "art", "pr-dexopt-job", "--schedule", "--slot", "_b")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Pre-reboot Dexopt job scheduled");
        }

        when(mPreRebootDriver.run(eq("_b"), eq(true) /* mapSnapshotsForOta */, any()))
                .thenReturn(new PreRebootResult(true /* success */));

        mPreRebootDexoptJob.onStartJobImpl(mJobService, mJobParameters);
        mPreRebootDexoptJob.waitForRunningJob();
    }

    @Test
    public void testPrDexoptJobCancelJobNotFound() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        try (var execution =
                        new CommandExecution(createHandler(), "art", "pr-dexopt-job", "--cancel")) {
            int exitCode = execution.waitAndGetExitCode();
            String outputs = getOutputs(execution);
            assertWithMessage(outputs).that(exitCode).isEqualTo(0);
            assertThat(outputs).contains("Pre-reboot Dexopt job cancelled");
        }
    }

    private ArtShellCommand createHandler() {
        return new ArtShellCommand(mInjector);
    }

    private String getOutputs(CommandExecution execution) {
        return Stream.concat(execution.getStdout().lines(), execution.getStderr().lines())
                .collect(Collectors.joining("\n"));
    }
}
