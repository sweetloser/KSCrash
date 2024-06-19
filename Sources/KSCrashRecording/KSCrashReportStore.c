//
//  KSCrashReportStore.c
//
//  Created by Karl Stenerud on 2012-02-05.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "KSCrashReportStore.h"
#include "KSLogger.h"
#include "KSFileUtils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * UniqueID 规则
 *  类型：int64_t
 *   -------------------------------------------------
 *   |      8bit   |      4bit     |    5 bit   |    5bit    |   6bit   |    6bit   |     30bit     |
 *   -------------------------------------------------
 *   |    year    |  month     |     day   |    hour   |   min  |    sec    |       id        |
 *   -------------------------------------------------
 */

static int g_maxReportCount = 5;
// Have to use max 32-bit atomics because of MIPS.
static _Atomic(uint32_t) g_nextUniqueIDLow;
static const char* g_appName;
static const char* g_reportsPath;
static const char* g_installPath;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int compareInt64(const void* a, const void* b)
{
    int64_t diff = *(int64_t*)a - *(int64_t*)b;
    if(diff < 0)
    {
        return -1;
    }
    else if(diff > 0)
    {
        return 1;
    }
    return 0;
}
// 获取下一个UniqueID
static inline uint64_t getNextUniqueID(void) {
    uint64_t uniqueID = 0;
    // 计算高34bit
    time_t rawTime;
    time(&rawTime);
    struct tm time;
    gmtime_r(&rawTime, &time);
    printf("year:%d", time.tm_year);
    uniqueID |= ((((uint64_t)time.tm_year) << (64-8)) &      0xFF00000000000000);
    printf("year:%llx", uniqueID);
    printf("mon:%d", time.tm_mon);
    uniqueID |= ((((uint64_t)time.tm_mon) << (64-12)) &      0x00F0000000000000);
    printf("mon:%llx", uniqueID);
    printf("day:%d", time.tm_mday);
    uniqueID |= ((((uint64_t)time.tm_mday) << (64-17)) &     0x000F800000000000);
    printf("mon:%llx", uniqueID);
    uniqueID |= ((((uint64_t)time.tm_hour) << (64-22)) &     0x00007C0000000000);
    uniqueID |= ((((uint64_t)time.tm_min) << (64-28)) &      0x000003F000000000);
    uniqueID |= ((((uint64_t)time.tm_sec) << (64-34)) &      0x0000000FC0000000);
    // 计算低30bit
    g_nextUniqueIDLow++;
    g_nextUniqueIDLow = g_nextUniqueIDLow & 0x3FFFFFFF;
    return (uniqueID | g_nextUniqueIDLow);
}

static void getCrashReportPathByID(int64_t id, char* pathBuffer)
{
    snprintf(pathBuffer, KSCRS_MAX_PATH_LENGTH, "%s/%s-report-%016llx.json", g_reportsPath, g_appName, id);
    
}

static int64_t getReportIDFromFilename(const char* filename)
{
    char scanFormat[100];
    sprintf(scanFormat, "%s-report-%%" PRIx64 ".json", g_appName);
    
    int64_t reportID = 0;
    sscanf(filename, scanFormat, &reportID);
    return reportID;
}

static int getReportCount(void)
{
    int count = 0;
    DIR* dir = opendir(g_reportsPath);
    if(dir == NULL)
    {
        KSLOG_ERROR("Could not open directory %s", g_reportsPath);
        goto done;
    }
    struct dirent* ent;
    while((ent = readdir(dir)) != NULL)
    {
        if(getReportIDFromFilename(ent->d_name) > 0)
        {
            count++;
        }
    }

done:
    if(dir != NULL)
    {
        closedir(dir);
    }
    return count;
}

static int getReportIDs(int64_t* reportIDs, int count)
{
    int index = 0;
    DIR* dir = opendir(g_reportsPath);
    if(dir == NULL)
    {
        KSLOG_ERROR("Could not open directory %s", g_reportsPath);
        goto done;
    }

    struct dirent* ent;
    while((ent = readdir(dir)) != NULL && index < count)
    {
        int64_t reportID = getReportIDFromFilename(ent->d_name);
        if(reportID > 0)
        {
            reportIDs[index++] = reportID;
        }
    }

    qsort(reportIDs, (unsigned)count, sizeof(reportIDs[0]), compareInt64);

done:
    if(dir != NULL)
    {
        closedir(dir);
    }
    return index;
}

static void pruneReports(void)
{
    int reportCount = getReportCount();
    if(reportCount > g_maxReportCount)
    {
        int64_t reportIDs[reportCount];
        reportCount = getReportIDs(reportIDs, reportCount);
        
        for(int i = 0; i < reportCount - g_maxReportCount; i++)
        {
            kscrs_deleteReportWithID(reportIDs[i]);
        }
    }
}

// 初始化id
static void initializeIDs(void) {
    time_t rawTime;
    time(&rawTime);
    struct tm time;
    gmtime_r(&rawTime, &time);
    int64_t baseID = (int64_t)time.tm_sec
                   + (int64_t)time.tm_min * 61
                   + (int64_t)time.tm_hour * 61 * 60
                   + (int64_t)time.tm_yday * 61 * 60 * 24
                   + (int64_t)time.tm_year * 61 * 60 * 24 * 366;
    g_nextUniqueIDLow = (uint32_t)(baseID & 0x3FFFFFFF);
}


// Public API

void kscrs_initialize(const char* appName, const char* installPath, const char* reportsPath)
{
    pthread_mutex_lock(&g_mutex);
    g_appName = strdup(appName);
    g_installPath = strdup(installPath);
    ksfu_makePath(installPath);
    g_reportsPath = strdup(reportsPath);
    ksfu_makePath(reportsPath);
    pruneReports();
    initializeIDs();
    pthread_mutex_unlock(&g_mutex);
}

int64_t kscrs_getNextCrashReport(char* crashReportPathBuffer)
{
    int64_t nextID = getNextUniqueID();
    if(crashReportPathBuffer)
    {
        getCrashReportPathByID(nextID, crashReportPathBuffer);
    }
    return nextID;
}

int kscrs_getReportCount(void)
{
    pthread_mutex_lock(&g_mutex);
    int count = getReportCount();
    pthread_mutex_unlock(&g_mutex);
    return count;
}

int kscrs_getReportIDs(int64_t* reportIDs, int count)
{
    pthread_mutex_lock(&g_mutex);
    count = getReportIDs(reportIDs, count);
    pthread_mutex_unlock(&g_mutex);
    return count;
}

char* kscrs_readReportAtPath(const char *path)
{
    pthread_mutex_lock(&g_mutex);
    char* result;
    ksfu_readEntireFile(path, &result, NULL, 2000000);
    pthread_mutex_unlock(&g_mutex);
    return result;
}

char* kscrs_readReport(int64_t reportID)
{
    pthread_mutex_lock(&g_mutex);
    char path[KSCRS_MAX_PATH_LENGTH];
    getCrashReportPathByID(reportID, path);
    char* result;
    ksfu_readEntireFile(path, &result, NULL, 2000000);
    pthread_mutex_unlock(&g_mutex);
    return result;
}

int64_t kscrs_addUserReport(const char* report, int reportLength)
{
    pthread_mutex_lock(&g_mutex);
    int64_t currentID = getNextUniqueID();
    char crashReportPath[KSCRS_MAX_PATH_LENGTH];
    getCrashReportPathByID(currentID, crashReportPath);

    int fd = open(crashReportPath, O_WRONLY | O_CREAT, 0644);
    if(fd < 0)
    {
        KSLOG_ERROR("Could not open file %s: %s", crashReportPath, strerror(errno));
        goto done;
    }

    int bytesWritten = (int)write(fd, report, (unsigned)reportLength);
    if(bytesWritten < 0)
    {
        KSLOG_ERROR("Could not write to file %s: %s", crashReportPath, strerror(errno));
        goto done;
    }
    else if(bytesWritten < reportLength)
    {
        KSLOG_ERROR("Expected to write %d bytes to file %s, but only wrote %d", crashReportPath, reportLength, bytesWritten);
    }

done:
    if(fd >= 0)
    {
        close(fd);
    }
    pthread_mutex_unlock(&g_mutex);

    return currentID;
}

void kscrs_deleteAllReports(void)
{
    pthread_mutex_lock(&g_mutex);
    ksfu_deleteContentsOfPath(g_reportsPath);
    pthread_mutex_unlock(&g_mutex);
}

void kscrs_deleteReportWithID(int64_t reportID)
{
    char path[KSCRS_MAX_PATH_LENGTH];
    getCrashReportPathByID(reportID, path);
    ksfu_removeFile(path, true);
}

void kscrs_setMaxReportCount(int maxReportCount)
{
    g_maxReportCount = maxReportCount;
}
