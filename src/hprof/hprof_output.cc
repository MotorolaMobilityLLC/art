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
#include <sys/time.h>
#include <cutils/open_memstream.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include "hprof.h"
#include "stringprintf.h"
#include "logging.h"

namespace art {

namespace hprof {

#define HPROF_MAGIC_STRING  "JAVA PROFILE 1.0.3"

#define U2_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint16_t value_ = (uint16_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 1] = (unsigned char)(value_      ); \
    } while (0)

#define U4_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint32_t value_ = (uint32_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >> 24); \
        buf_[offset_ + 1] = (unsigned char)(value_ >> 16); \
        buf_[offset_ + 2] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 3] = (unsigned char)(value_      ); \
    } while (0)

#define U8_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint64_t value_ = (uint64_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >> 56); \
        buf_[offset_ + 1] = (unsigned char)(value_ >> 48); \
        buf_[offset_ + 2] = (unsigned char)(value_ >> 40); \
        buf_[offset_ + 3] = (unsigned char)(value_ >> 32); \
        buf_[offset_ + 4] = (unsigned char)(value_ >> 24); \
        buf_[offset_ + 5] = (unsigned char)(value_ >> 16); \
        buf_[offset_ + 6] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 7] = (unsigned char)(value_      ); \
    } while (0)

/*
 * Initialize an hprof context struct.
 *
 * This will take ownership of "fileName".
 */
void hprofContextInit(hprof_context_t *ctx, char *fileName, int fd,
                      bool writeHeader, bool directToDdms)
{
    memset(ctx, 0, sizeof (*ctx));

    /*
     * Have to do this here, because it must happen after we
     * memset the struct (want to treat fileDataPtr/fileDataSize
     * as read-only while the file is open).
     */
    FILE *fp = open_memstream(&ctx->fileDataPtr, &ctx->fileDataSize);
    if (fp == NULL) {
        /* not expected */
        LOG(ERROR) << StringPrintf("hprof: open_memstream failed: %s", strerror(errno));
        CHECK(false);
    }

    ctx->directToDdms = directToDdms;
    ctx->fileName = fileName;
    ctx->memFp = fp;
    ctx->fd = fd;

    ctx->curRec.allocLen = 128;
    ctx->curRec.body = (unsigned char *)malloc(ctx->curRec.allocLen);
//xxx check for/return an error

    if (writeHeader) {
        char magic[] = HPROF_MAGIC_STRING;
        unsigned char buf[4];
        struct timeval now;
        uint64_t nowMs;

        /* Write the file header.
         *
         * [uint8_t]*: NUL-terminated magic string.
         */
        fwrite(magic, 1, sizeof(magic), fp);

        /* uint32_t: size of identifiers.  We're using addresses
         *     as IDs, so make sure a pointer fits.
         */
        U4_TO_BUF_BE(buf, 0, sizeof(void *));
        fwrite(buf, 1, sizeof(uint32_t), fp);

        /* The current time, in milliseconds since 0:00 GMT, 1/1/70.
         */
        if (gettimeofday(&now, NULL) < 0) {
            nowMs = 0;
        } else {
            nowMs = (uint64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
        }

        /* uint32_t: high word of the 64-bit time.
         */
        U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs >> 32));
        fwrite(buf, 1, sizeof(uint32_t), fp);

        /* uint32_t: low word of the 64-bit time.
         */
        U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs & 0xffffffffULL));
        fwrite(buf, 1, sizeof(uint32_t), fp); //xxx fix the time
    }
}

int hprofFlushRecord(hprof_record_t *rec, FILE *fp)
{
    if (rec->dirty) {
        unsigned char headBuf[sizeof (uint8_t) + 2 * sizeof (uint32_t)];
        int nb;

        headBuf[0] = rec->tag;
        U4_TO_BUF_BE(headBuf, 1, rec->time);
        U4_TO_BUF_BE(headBuf, 5, rec->length);

        nb = fwrite(headBuf, 1, sizeof(headBuf), fp);
        if (nb != sizeof(headBuf)) {
            return UNIQUE_ERROR();
        }
        nb = fwrite(rec->body, 1, rec->length, fp);
        if (nb != (int)rec->length) {
            return UNIQUE_ERROR();
        }

        rec->dirty = false;
    }
//xxx if we used less than half (or whatever) of allocLen, shrink the buffer.

    return 0;
}

int hprofFlushCurrentRecord(hprof_context_t *ctx)
{
    return hprofFlushRecord(&ctx->curRec, ctx->memFp);
}

int hprofStartNewRecord(hprof_context_t *ctx, uint8_t tag, uint32_t time)
{
    hprof_record_t *rec = &ctx->curRec;
    int err;

    err = hprofFlushRecord(rec, ctx->memFp);
    if (err != 0) {
        return err;
    } else if (rec->dirty) {
        return UNIQUE_ERROR();
    }

    rec->dirty = true;
    rec->tag = tag;
    rec->time = time;
    rec->length = 0;

    return 0;
}

static inline int guaranteeRecordAppend(hprof_record_t *rec, size_t nmore)
{
    size_t minSize;

    minSize = rec->length + nmore;
    if (minSize > rec->allocLen) {
        unsigned char *newBody;
        size_t newAllocLen;

        newAllocLen = rec->allocLen * 2;
        if (newAllocLen < minSize) {
            newAllocLen = rec->allocLen + nmore + nmore/2;
        }
        newBody = (unsigned char *)realloc(rec->body, newAllocLen);
        if (newBody != NULL) {
            rec->body = newBody;
            rec->allocLen = newAllocLen;
        } else {
//TODO: set an error flag so future ops will fail
            return UNIQUE_ERROR();
        }
    }

    CHECK(rec->length + nmore <= rec->allocLen);
    return 0;
}

int hprofAddU1ListToRecord(hprof_record_t *rec, const uint8_t *values,
                           size_t numValues)
{
    int err;

    err = guaranteeRecordAppend(rec, numValues);
    if (err != 0) {
        return err;
    }

    memcpy(rec->body + rec->length, values, numValues);
    rec->length += numValues;

    return 0;
}

int hprofAddU1ToRecord(hprof_record_t *rec, uint8_t value)
{
    int err;

    err = guaranteeRecordAppend(rec, 1);
    if (err != 0) {
        return err;
    }

    rec->body[rec->length++] = value;

    return 0;
}

int hprofAddUtf8StringToRecord(hprof_record_t *rec, const char *str)
{
    /* The terminating NUL character is NOT written.
     */
//xxx don't do a strlen;  add and grow as necessary, until NUL
    return hprofAddU1ListToRecord(rec, (const uint8_t *)str, strlen(str));
}

int hprofAddU2ListToRecord(hprof_record_t *rec, const uint16_t *values,
                           size_t numValues)
{
    int err = guaranteeRecordAppend(rec, numValues * 2);
    if (err != 0) {
        return err;
    }

//xxx this can be way smarter
//xxx also, don't do this bytewise if aligned and on a matching-endian arch
    unsigned char *insert = rec->body + rec->length;
    for (size_t i = 0; i < numValues; i++) {
        U2_TO_BUF_BE(insert, 0, *values++);
        insert += sizeof(*values);
    }
    rec->length += numValues * 2;

    return 0;
}

int hprofAddU2ToRecord(hprof_record_t *rec, uint16_t value)
{
    return hprofAddU2ListToRecord(rec, &value, 1);
}

int hprofAddU4ListToRecord(hprof_record_t *rec, const uint32_t *values,
                           size_t numValues)
{
    int err = guaranteeRecordAppend(rec, numValues * 4);
    if (err != 0) {
        return err;
    }

//xxx this can be way smarter
//xxx also, don't do this bytewise if aligned and on a matching-endian arch
    unsigned char *insert = rec->body + rec->length;
    for (size_t i = 0; i < numValues; i++) {
        U4_TO_BUF_BE(insert, 0, *values++);
        insert += sizeof(*values);
    }
    rec->length += numValues * 4;

    return 0;
}

int hprofAddU4ToRecord(hprof_record_t *rec, uint32_t value)
{
    return hprofAddU4ListToRecord(rec, &value, 1);
}

int hprofAddU8ListToRecord(hprof_record_t *rec, const uint64_t *values,
                           size_t numValues)
{
    int err = guaranteeRecordAppend(rec, numValues * 8);
    if (err != 0) {
        return err;
    }

//xxx this can be way smarter
//xxx also, don't do this bytewise if aligned and on a matching-endian arch
    unsigned char *insert = rec->body + rec->length;
    for (size_t i = 0; i < numValues; i++) {
        U8_TO_BUF_BE(insert, 0, *values++);
        insert += sizeof(*values);
    }
    rec->length += numValues * 8;

    return 0;
}

int hprofAddU8ToRecord(hprof_record_t *rec, uint64_t value)
{
    return hprofAddU8ListToRecord(rec, &value, 1);
}

}  // namespace hprof

}  // namespace art
