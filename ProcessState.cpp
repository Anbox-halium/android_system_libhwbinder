/*
 * Copyright (C) 2005 The Android Open Source Project
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

#define LOG_TAG "hw-ProcessState"

#include <hwbinder/ProcessState.h>

#include <cutils/atomic.h>
#include <hwbinder/BpHwBinder.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/binder_kernel.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/threads.h>

#include <private/binder/binder_module.h>
#include <hwbinder/Static.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEFAULT_BINDER_VM_SIZE ((1 * 1024 * 1024) - sysconf(_SC_PAGE_SIZE) * 2)
#define DEFAULT_MAX_BINDER_THREADS 0

// -------------------------------------------------------------------------

namespace android {
namespace hardware {

class PoolThread : public Thread
{
public:
    explicit PoolThread(bool isMain, bool isHostHwBinder)
        : mIsMain(isMain), mIsHostHwBinder(isHostHwBinder)
    {
    }

protected:
    virtual bool threadLoop()
    {
        IPCThreadState::self(mIsHostHwBinder)->joinThreadPool(mIsMain);
        return false;
    }

    const bool mIsMain;
    const bool mIsHostHwBinder;
};

sp<ProcessState> ProcessState::self()
{
    return ProcessState::self(false);
}

sp<ProcessState> ProcessState::self(bool isHost)
{
    Mutex::Autolock _l(gProcessMutex);
    if (isHost) {
        if (gHostProcess != nullptr) {
            return gHostProcess;
        }
        gHostProcess = new ProcessState(DEFAULT_BINDER_VM_SIZE, isHost);
        return gHostProcess;
    } else {
        if (gProcess != nullptr) {
            return gProcess;
        }
        gProcess = new ProcessState(DEFAULT_BINDER_VM_SIZE, isHost);
        return gProcess;
    }
}

sp<ProcessState> ProcessState::selfOrNull(bool isHost) {
    Mutex::Autolock _l(gProcessMutex);
    if (isHost)
        return gHostProcess;
    else
        return gProcess;
}

sp<ProcessState> ProcessState::initWithMmapSize(size_t mmap_size, bool isHost) {
    Mutex::Autolock _l(gProcessMutex);
    if (isHost) {
        if (gHostProcess != nullptr) {
            LOG_ALWAYS_FATAL_IF(mmap_size != gHostProcess->getMmapSize(),
                    "ProcessState already initialized with a different mmap size.");
            return gHostProcess;
        }

        gHostProcess = new ProcessState(mmap_size, isHost);
        return gHostProcess;
    } else {
        if (gProcess != nullptr) {
            LOG_ALWAYS_FATAL_IF(mmap_size != gProcess->getMmapSize(),
                    "ProcessState already initialized with a different mmap size.");
            return gProcess;
        }

        gProcess = new ProcessState(mmap_size, isHost);
        return gProcess;
    }
}

bool ProcessState::isHostBinder() {
    return mIsHost;
}

void ProcessState::setContextObject(const sp<IBinder>& object)
{
    setContextObject(object, String16("default"));
}

sp<IBinder> ProcessState::getContextObject(const sp<IBinder>& /*caller*/)
{
    return getStrongProxyForHandle(0);
}

void ProcessState::setContextObject(const sp<IBinder>& object, const String16& name)
{
    AutoMutex _l(mLock);
    mContexts.add(name, object);
}

sp<IBinder> ProcessState::getContextObject(const String16& name, const sp<IBinder>& caller)
{
    mLock.lock();
    sp<IBinder> object(
        mContexts.indexOfKey(name) >= 0 ? mContexts.valueFor(name) : nullptr);
    mLock.unlock();

    //printf("Getting context object %s for %p\n", String8(name).string(), caller.get());

    if (object != nullptr) return object;

    // Don't attempt to retrieve contexts if we manage them
    if (mManagesContexts) {
        ALOGE("getContextObject(%s) failed, but we manage the contexts!\n",
            String8(name).string());
        return nullptr;
    }

    IPCThreadState* ipc = IPCThreadState::self(mIsHost);
    {
        Parcel data, reply;
        // no interface token on this magic transaction
        data.writeString16(name);
        data.writeStrongBinder(caller);
        status_t result = ipc->transact(0 /*magic*/, 0, data, &reply, 0);
        if (result == NO_ERROR) {
            object = reply.readStrongBinder();
        }
    }

    ipc->flushCommands();

    if (object != nullptr) setContextObject(object, name);
    return object;
}

void ProcessState::startThreadPool()
{
    AutoMutex _l(mLock);
    if (!mThreadPoolStarted) {
        mThreadPoolStarted = true;
        if (mSpawnThreadOnStart) {
            spawnPooledThread(true);
        }
    }
}

bool ProcessState::isContextManager(void) const
{
    return mManagesContexts;
}

bool ProcessState::becomeContextManager(context_check_func checkFunc, void* userData)
{
    if (!mManagesContexts) {
        AutoMutex _l(mLock);
        mBinderContextCheckFunc = checkFunc;
        mBinderContextUserData = userData;

        flat_binder_object obj {
            // Disabled for Anbox
            /*.flags = FLAT_BINDER_FLAG_TXN_SECURITY_CTX,*/
        };

        status_t result = ioctl(mDriverFD, BINDER_SET_CONTEXT_MGR_EXT, &obj);

        // fallback to original method
        if (result != 0) {
            android_errorWriteLog(0x534e4554, "121035042");

            int dummy = 0;
            result = ioctl(mDriverFD, BINDER_SET_CONTEXT_MGR, &dummy);
        }

        if (result == 0) {
            mManagesContexts = true;
        } else if (result == -1) {
            mBinderContextCheckFunc = nullptr;
            mBinderContextUserData = nullptr;
            ALOGE("Binder ioctl to become context manager failed: %s\n", strerror(errno));
        }
    }
    return mManagesContexts;
}

// Get references to userspace objects held by the kernel binder driver
// Writes up to count elements into buf, and returns the total number
// of references the kernel has, which may be larger than count.
// buf may be NULL if count is 0.  The pointers returned by this method
// should only be used for debugging and not dereferenced, they may
// already be invalid.
ssize_t ProcessState::getKernelReferences(size_t buf_count, uintptr_t* buf) {
    binder_node_debug_info info = {};

    uintptr_t* end = buf ? buf + buf_count : nullptr;
    size_t count = 0;

    do {
        status_t result = ioctl(mDriverFD, BINDER_GET_NODE_DEBUG_INFO, &info);
        if (result < 0) {
            return -1;
        }
        if (info.ptr != 0) {
            if (buf && buf < end) *buf++ = info.ptr;
            count++;
            if (buf && buf < end) *buf++ = info.cookie;
            count++;
        }
    } while (info.ptr != 0);

    return count;
}

// Queries the driver for the current strong reference count of the node
// that the handle points to. Can only be used by the servicemanager.
//
// Returns -1 in case of failure, otherwise the strong reference count.
ssize_t ProcessState::getStrongRefCountForNodeByHandle(int32_t handle) {
    binder_node_info_for_ref info;
    memset(&info, 0, sizeof(binder_node_info_for_ref));

    info.handle = handle;

    status_t result = ioctl(mDriverFD, BINDER_GET_NODE_INFO_FOR_REF, &info);

    if (result != OK) {
        return -1;
    }

    return info.strong_count;
}

size_t ProcessState::getMmapSize() {
    return mMmapSize;
}

void ProcessState::setCallRestriction(CallRestriction restriction) {
    LOG_ALWAYS_FATAL_IF(IPCThreadState::selfOrNull(mIsHost), "Call restrictions must be set before the threadpool is started.");

    mCallRestriction = restriction;
}

ProcessState::handle_entry* ProcessState::lookupHandleLocked(int32_t handle)
{
    const size_t N=mHandleToObject.size();
    if (N <= (size_t)handle) {
        handle_entry e;
        e.binder = nullptr;
        e.refs = nullptr;
        status_t err = mHandleToObject.insertAt(e, N, handle+1-N);
        if (err < NO_ERROR) return nullptr;
    }
    return &mHandleToObject.editItemAt(handle);
}

sp<IBinder> ProcessState::getStrongProxyForHandle(int32_t handle)
{
    sp<IBinder> result;

    AutoMutex _l(mLock);

    handle_entry* e = lookupHandleLocked(handle);

    if (e != nullptr) {
        // We need to create a new BpHwBinder if there isn't currently one, OR we
        // are unable to acquire a weak reference on this current one.  See comment
        // in getWeakProxyForHandle() for more info about this.
        IBinder* b = e->binder;
        if (b == nullptr || !e->refs->attemptIncWeak(this)) {
            b = new BpHwBinder(handle, mIsHost);
            e->binder = b;
            if (b) e->refs = b->getWeakRefs();
            result = b;
        } else {
            // This little bit of nastyness is to allow us to add a primary
            // reference to the remote proxy when this team doesn't have one
            // but another team is sending the handle to us.
            result.force_set(b);
            e->refs->decWeak(this);
        }
    }

    return result;
}

wp<IBinder> ProcessState::getWeakProxyForHandle(int32_t handle)
{
    wp<IBinder> result;

    AutoMutex _l(mLock);

    handle_entry* e = lookupHandleLocked(handle);

    if (e != nullptr) {
        // We need to create a new BpHwBinder if there isn't currently one, OR we
        // are unable to acquire a weak reference on this current one.  The
        // attemptIncWeak() is safe because we know the BpHwBinder destructor will always
        // call expungeHandle(), which acquires the same lock we are holding now.
        // We need to do this because there is a race condition between someone
        // releasing a reference on this BpHwBinder, and a new reference on its handle
        // arriving from the driver.
        IBinder* b = e->binder;
        if (b == nullptr || !e->refs->attemptIncWeak(this)) {
            b = new BpHwBinder(handle, mIsHost);
            result = b;
            e->binder = b;
            if (b) e->refs = b->getWeakRefs();
        } else {
            result = b;
            e->refs->decWeak(this);
        }
    }

    return result;
}

void ProcessState::expungeHandle(int32_t handle, IBinder* binder)
{
    AutoMutex _l(mLock);

    handle_entry* e = lookupHandleLocked(handle);

    // This handle may have already been replaced with a new BpHwBinder
    // (if someone failed the AttemptIncWeak() above); we don't want
    // to overwrite it.
    if (e && e->binder == binder) e->binder = nullptr;
}

String8 ProcessState::makeBinderThreadName() {
    int32_t s = android_atomic_add(1, &mThreadPoolSeq);
    pid_t pid = getpid();
    String8 name;
    name.appendFormat("HwBinder:%d_%X", pid, s);
    return name;
}

void ProcessState::spawnPooledThread(bool isMain)
{
    if (mThreadPoolStarted) {
        String8 name = makeBinderThreadName();
        ALOGV("Spawning new pooled thread, name=%s\n", name.string());
        sp<Thread> t = new PoolThread(isMain, mIsHost);
        t->run(name.string());
    }
}

status_t ProcessState::setThreadPoolConfiguration(size_t maxThreads, bool callerJoinsPool) {
    // if the caller joins the pool, then there will be one thread which is impossible.
    LOG_ALWAYS_FATAL_IF(maxThreads == 0 && callerJoinsPool,
           "Binder threadpool must have a minimum of one thread if caller joins pool.");

    size_t threadsToAllocate = maxThreads;

    // If the caller is going to join the pool it will contribute one thread to the threadpool.
    // This is part of the API's contract.
    if (callerJoinsPool) threadsToAllocate--;

    // If we can, spawn one thread from userspace when the threadpool is started. This ensures
    // that there is always a thread available to start more threads as soon as the threadpool
    // is started.
    bool spawnThreadOnStart = threadsToAllocate > 0;
    if (spawnThreadOnStart) threadsToAllocate--;

    // the BINDER_SET_MAX_THREADS ioctl really tells the kernel how many threads
    // it's allowed to spawn, *in addition* to any threads we may have already
    // spawned locally.
    size_t kernelMaxThreads = threadsToAllocate;

    AutoMutex _l(mLock);
    if (ioctl(mDriverFD, BINDER_SET_MAX_THREADS, &kernelMaxThreads) == -1) {
        ALOGE("Binder ioctl to set max threads failed: %s", strerror(errno));
        return -errno;
    }

    mMaxThreads = maxThreads;
    mSpawnThreadOnStart = spawnThreadOnStart;

    return NO_ERROR;
}

size_t ProcessState::getMaxThreads() {
    return mMaxThreads;
}

void ProcessState::giveThreadPoolName() {
    androidSetThreadName( makeBinderThreadName().string() );
}

static int open_driver(bool isHost)
{
    const char *driver = "/dev/hwbinder";
    if (isHost)
        driver = "/dev/host_hwbinder";
    int fd = open(driver, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        int vers = 0;
        status_t result = ioctl(fd, BINDER_VERSION, &vers);
        if (result == -1) {
            ALOGE("Binder ioctl to obtain version failed: %s", strerror(errno));
            close(fd);
            fd = -1;
        }
        if (result != 0 || vers != BINDER_CURRENT_PROTOCOL_VERSION) {
          ALOGE("Binder driver protocol(%d) does not match user space protocol(%d)!", vers, BINDER_CURRENT_PROTOCOL_VERSION);
            close(fd);
            fd = -1;
        }
        size_t maxThreads = DEFAULT_MAX_BINDER_THREADS;
        result = ioctl(fd, BINDER_SET_MAX_THREADS, &maxThreads);
        if (result == -1) {
            ALOGE("Binder ioctl to set max threads failed: %s", strerror(errno));
        }
    } else {
        ALOGW("Opening '%s' failed: %s\n", driver, strerror(errno));
    }
    return fd;
}

ProcessState::ProcessState(size_t mmap_size, bool isHost)
    : mDriverFD(open_driver(isHost))
    , mVMStart(MAP_FAILED)
    , mThreadCountLock(PTHREAD_MUTEX_INITIALIZER)
    , mThreadCountDecrement(PTHREAD_COND_INITIALIZER)
    , mExecutingThreadsCount(0)
    , mMaxThreads(DEFAULT_MAX_BINDER_THREADS)
    , mStarvationStartTimeMs(0)
    , mManagesContexts(false)
    , mBinderContextCheckFunc(nullptr)
    , mBinderContextUserData(nullptr)
    , mThreadPoolStarted(false)
    , mSpawnThreadOnStart(true)
    , mThreadPoolSeq(1)
    , mMmapSize(mmap_size)
    , mCallRestriction(CallRestriction::NONE)
    , mIsHost(isHost)
{
    if (mDriverFD >= 0) {
        // mmap the binder, providing a chunk of virtual address space to receive transactions.
        mVMStart = mmap(nullptr, mMmapSize, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, mDriverFD, 0);
        if (mVMStart == MAP_FAILED) {
            // *sigh*
            ALOGE("Mmapping /dev/hwbinder failed: %s\n", strerror(errno));
            close(mDriverFD);
            mDriverFD = -1;
        }
    }
    else {
        ALOGE("Binder driver could not be opened.  Terminating.");
    }
}

ProcessState::~ProcessState()
{
    if (mDriverFD >= 0) {
        if (mVMStart != MAP_FAILED) {
            munmap(mVMStart, mMmapSize);
        }
        close(mDriverFD);
    }
    mDriverFD = -1;
}

}; // namespace hardware
}; // namespace android
