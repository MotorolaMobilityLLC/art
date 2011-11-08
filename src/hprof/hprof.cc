/*
 * Copyright (C) 2008 The Android Open Source Project
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

/*
 * Preparation and completion of hprof data generation.  The output is
 * written into two files and then combined.  This is necessary because
 * we generate some of the data (strings and classes) while we dump the
 * heap, and some analysis tools require that the class and string data
 * appear first.
 */

#include "hprof.h"
#include "heap.h"
#include "debugger.h"
#include "stringprintf.h"
#include "thread_list.h"
#include "logging.h"

#include <sys/uio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

namespace art {

namespace hprof {

#define kHeadSuffix "-hptemp"

hprof_context_t* hprofStartup(const char *outputFileName, int fd,
                              bool directToDdms)
{
    hprofStartup_String();
    hprofStartup_Class();

    hprof_context_t *ctx = (hprof_context_t *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        LOG(ERROR) << "hprof: can't allocate context.";
        return NULL;
    }

    /* pass in name or descriptor of the output file */
    hprofContextInit(ctx, strdup(outputFileName), fd, false, directToDdms);

    CHECK(ctx->memFp != NULL);

    return ctx;
}

// TODO: use File::WriteFully
int sysWriteFully(int fd, const void* buf, size_t count, const char* logMsg)
{
    while (count != 0) {
        ssize_t actual = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (actual < 0) {
            int err = errno;
            LOG(ERROR) << StringPrintf("%s: write failed: %s", logMsg, strerror(err));
            return err;
        } else if (actual != (ssize_t) count) {
            LOG(DEBUG) << StringPrintf("%s: partial write (will retry): (%d of %zd)",
                logMsg, (int) actual, count);
            buf = (const void*) (((const uint8_t*) buf) + actual);
        }
        count -= actual;
    }

    return 0;
}

/*
 * Finish up the hprof dump.  Returns true on success.
 */
bool hprofShutdown(hprof_context_t *tailCtx)
{
    /* flush the "tail" portion of the output */
    hprofFlushCurrentRecord(tailCtx);

    /*
     * Create a new context struct for the start of the file.  We
     * heap-allocate it so we can share the "free" function.
     */
    hprof_context_t *headCtx = (hprof_context_t *)malloc(sizeof(*headCtx));
    if (headCtx == NULL) {
        LOG(ERROR) << "hprof: can't allocate context.";
        hprofFreeContext(tailCtx);
        return false;
    }
    hprofContextInit(headCtx, strdup(tailCtx->fileName), tailCtx->fd, true,
        tailCtx->directToDdms);

    LOG(INFO) << StringPrintf("hprof: dumping heap strings to \"%s\".", tailCtx->fileName);
    hprofDumpStrings(headCtx);
    hprofDumpClasses(headCtx);

    /* Write a dummy stack trace record so the analysis
     * tools don't freak out.
     */
    hprofStartNewRecord(headCtx, HPROF_TAG_STACK_TRACE, HPROF_TIME);
    hprofAddU4ToRecord(&headCtx->curRec, HPROF_NULL_STACK_TRACE);
    hprofAddU4ToRecord(&headCtx->curRec, HPROF_NULL_THREAD);
    hprofAddU4ToRecord(&headCtx->curRec, 0);    // no frames

    hprofFlushCurrentRecord(headCtx);

    hprofShutdown_Class();
    hprofShutdown_String();

    /* flush to ensure memstream pointer and size are updated */
    fflush(headCtx->memFp);
    fflush(tailCtx->memFp);

    if (tailCtx->directToDdms) {
        /* send the data off to DDMS */
        struct iovec iov[2];
        iov[0].iov_base = headCtx->fileDataPtr;
        iov[0].iov_len = headCtx->fileDataSize;
        iov[1].iov_base = tailCtx->fileDataPtr;
        iov[1].iov_len = tailCtx->fileDataSize;
        Dbg::DdmSendChunkV(CHUNK_TYPE("HPDS"), iov, 2);
    } else {
        /*
         * Open the output file, and copy the head and tail to it.
         */
        CHECK(headCtx->fd == tailCtx->fd);

        int outFd;
        if (headCtx->fd >= 0) {
            outFd = dup(headCtx->fd);
            if (outFd < 0) {
                LOG(ERROR) << StringPrintf("dup(%d) failed: %s", headCtx->fd, strerror(errno));
                /* continue to fail-handler below */
            }
        } else {
            outFd = open(tailCtx->fileName, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (outFd < 0) {
                LOG(ERROR) << StringPrintf("can't open %s: %s", headCtx->fileName, strerror(errno));
                /* continue to fail-handler below */
            }
        }
        if (outFd < 0) {
            hprofFreeContext(headCtx);
            hprofFreeContext(tailCtx);
            return false;
        }

        int result;
        result = sysWriteFully(outFd, headCtx->fileDataPtr,
            headCtx->fileDataSize, "hprof-head");
        result |= sysWriteFully(outFd, tailCtx->fileDataPtr,
            tailCtx->fileDataSize, "hprof-tail");
        close(outFd);
        if (result != 0) {
            hprofFreeContext(headCtx);
            hprofFreeContext(tailCtx);
            return false;
        }
    }

    /* throw out a log message for the benefit of "runhat" */
    LOG(INFO) << StringPrintf("hprof: heap dump completed (%dKB)",
        (headCtx->fileDataSize + tailCtx->fileDataSize + 1023) / 1024);

    hprofFreeContext(headCtx);
    hprofFreeContext(tailCtx);

    return true;
}

/*
 * Free any heap-allocated items in "ctx", and then free "ctx" itself.
 */
void hprofFreeContext(hprof_context_t *ctx)
{
    CHECK(ctx != NULL);

    /* we don't own ctx->fd, do not close */

    if (ctx->memFp != NULL)
        fclose(ctx->memFp);
    free(ctx->curRec.body);
    free(ctx->fileName);
    free(ctx->fileDataPtr);
    free(ctx);
}

/*
 * Visitor invoked on every root reference.
 */
void HprofRootVisitor(const Object* obj, void* arg) {
    uint32_t threadId = 0;  // TODO
    /*RootType */ size_t type = 0; // TODO

    static const hprof_heap_tag_t xlate[] = {
        HPROF_ROOT_UNKNOWN,
        HPROF_ROOT_JNI_GLOBAL,
        HPROF_ROOT_JNI_LOCAL,
        HPROF_ROOT_JAVA_FRAME,
        HPROF_ROOT_NATIVE_STACK,
        HPROF_ROOT_STICKY_CLASS,
        HPROF_ROOT_THREAD_BLOCK,
        HPROF_ROOT_MONITOR_USED,
        HPROF_ROOT_THREAD_OBJECT,
        HPROF_ROOT_INTERNED_STRING,
        HPROF_ROOT_FINALIZING,
        HPROF_ROOT_DEBUGGER,
        HPROF_ROOT_REFERENCE_CLEANUP,
        HPROF_ROOT_VM_INTERNAL,
        HPROF_ROOT_JNI_MONITOR,
    };
    hprof_context_t *ctx;

    CHECK(arg != NULL);
    CHECK(type < (sizeof(xlate) / sizeof(hprof_heap_tag_t)));
    if (obj == NULL) {
        return;
    }
    ctx = (hprof_context_t *)arg;
    ctx->gcScanState = xlate[type];
    ctx->gcThreadSerialNumber = threadId;
    hprofMarkRootObject(ctx, obj, 0);
    ctx->gcScanState = 0;
    ctx->gcThreadSerialNumber = 0;
}

/*
 * Visitor invoked on every heap object.
 */

static void HprofBitmapCallback(Object *obj, void *arg)
{
    CHECK(obj != NULL);
    CHECK(arg != NULL);
    hprof_context_t *ctx = (hprof_context_t *)arg;
    DumpHeapObject(ctx, obj);
}

/*
 * Walk the roots and heap writing heap information to the specified
 * file.
 *
 * If "fd" is >= 0, the output will be written to that file descriptor.
 * Otherwise, "fileName" is used to create an output file.
 *
 * If "directToDdms" is set, the other arguments are ignored, and data is
 * sent directly to DDMS.
 *
 * Returns 0 on success, or an error code on failure.
 */
int DumpHeap(const char* fileName, int fd, bool directToDdms)
{
    hprof_context_t *ctx;
    int success;

    CHECK(fileName != NULL);
    ScopedHeapLock lock;
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);

    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    thread_list->SuspendAll();

    ctx = hprofStartup(fileName, fd, directToDdms);
    if (ctx == NULL) {
        return -1;
    }
    Runtime::Current()->VisitRoots(HprofRootVisitor, ctx);
    Heap::GetLiveBits()->Walk(HprofBitmapCallback, ctx);
    hprofFinishHeapDump(ctx);
//TODO: write a HEAP_SUMMARY record
    success = hprofShutdown(ctx) ? 0 : -1;
    thread_list->ResumeAll();
    return success;
}

}  // namespace hprof

}  // namespace art
