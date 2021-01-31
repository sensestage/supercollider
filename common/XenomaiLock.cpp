#include "XenomaiLock.h"

#include <pthread.h>
#include <error.h>
#include <string.h>
#include <cobalt/sys/cobalt.h>
#include <xenomai/init.h>
#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __COBALT__
#    error This should be compiled with __COBALT__ and the appropriate Xenomai headers in the #include path
#endif // __COBALT__
// All __wrap_* calls below are Xenomai libcobalt calls

//#define PRINT_XENO_LOCK

#ifdef PRINT_XENO_LOCK
#    define xprintf printf
#    define xfprintf fprintf
#else // PRINT_XENO_LOCK
#    define xprintf(...)
#    define xfprintf(...)
#endif // PRINT_XENO_LOCK

static inline pid_t get_tid() {
    pid_t tid = syscall(SYS_gettid);
    return tid;
}

// throughout, we use heuristics to check whether Xenomai needs to be
// initialised and whether the current thread is a Xenomai thread.
// See https://www.xenomai.org/pipermail/xenomai/2019-January/040203.html
static void initialize_xenomai() {
    xprintf("Initialize_xenomai\n");
    int argc = 2;
    char blankOpt[] = "";
#ifdef PRINT_XENO_LOCK
    char traceOpt[] = "--trace";
#else // PRINT_XENO_LOCK
    char traceOpt[] = "";
#endif // PRINT_XENO_LOCK

    char* const argv[3] = { blankOpt, traceOpt, blankOpt };
    char* const* argvPtrs[3] = { &argv[0], &argv[1], &argv[2] };
    xenomai_init(&argc, argvPtrs);
}

static int turn_into_cobalt_thread(bool recurred = false) {
    int current_mode = cobalt_thread_mode();
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    int policy;
    int ret = pthread_getschedparam(pthread_self(), &policy, &param);
    pid_t tid = get_tid();

    if (int ret = __wrap_sched_setscheduler(tid, policy, &param)) {
        fprintf(stderr, "Warning: unable to turn current thread into a Xenomai thread : (%d) %s\n", -ret,
                strerror(-ret));
        initialize_xenomai();
        if (!recurred)
            return turn_into_cobalt_thread(1);
        else
            return -1;
    }
    xprintf("Turned thread %d into a Cobalt thread %s\n", tid, recurred ? "with recursion" : "");
    return 0;
}

XenomaiInitializer::XenomaiInitializer() { initialize_xenomai(); }

XenomaiMutex::XenomaiMutex() {
    xprintf("Construct mutex\n");
    if (EPERM == __wrap_pthread_mutex_init(&mutex, NULL)) {
        xprintf("mutex init returned EPERM\n");
        initialize_xenomai();
        if (int ret = __wrap_pthread_mutex_init(&mutex, NULL)) {
            fprintf(stderr, "Error: unable to initialize mutex : (%d) %s\n", ret, strerror(-ret));
            return;
        }
    }
    enabled = true;
}

XenomaiMutex::~XenomaiMutex() {
    xprintf("Destroy mutex %p\n", &mutex);
    if (enabled)
        __wrap_pthread_mutex_destroy(&mutex);
}

// a helper function to try
// - call func()
// - if it fails, try to turn the current thread into a cobalt thread and call func() again
// - if it fails again, just fail
// - return true if it succeeds, or false if it fails
// id and name are just for debugging purposes, while enabled is there because it saves duplicating some lines
template <typename T> static int try_or_retry(std::function<int()> func, T* id, const char* name, bool enabled) {
    xprintf("tid: %d ", get_tid());
    int ret;
    if (!enabled) {
        xfprintf(stderr, "%s %d disabled %p\n", name, id);
        return false;
    }
    xprintf("%s %p\n", name, id);
    if (EPERM != (ret = func())) // "success" (or at least meaningful failure)
        return ret;
    // if we got EPERM, we are not a Xenomai thread
    if (turn_into_cobalt_thread()) {
        xfprintf(stderr, "%s %p could not turn into cobalt\n", name, id);
        return EPERM;
    }
    if (EPERM == (ret = func()))
        xfprintf(stderr, "%s %p failed after having turned into cobalt: %d\n", name, id, ret);
    return ret;
}

// condition resource_deadlock_would_occur instead of deadlocking. https://en.cppreference.com/w/cpp/thread/mutex/lock
bool XenomaiMutex::try_lock(bool recurred) {
    const char name[] = "try_lock";
    return 0 == try_or_retry([this]() { return __wrap_pthread_mutex_trylock(&this->mutex); }, &mutex, name, enabled);
    // TODO: An implementation that can detect the invalid usage is encouraged to throw a std::system_error with error
    // condition resource_deadlock_would_occur instead of deadlocking.
}

void XenomaiMutex::lock(bool recurred) {
    const char name[] = "lock";
    try_or_retry([this]() { return __wrap_pthread_mutex_lock(&this->mutex); }, &mutex, name, enabled);
}

void XenomaiMutex::unlock(bool recurred) {
    const char name[] = "unlock";
    try_or_retry([this]() { return __wrap_pthread_mutex_unlock(&mutex); }, &mutex, name, enabled);
}

XenomaiConditionVariable::XenomaiConditionVariable() {
    xprintf("Construct CondictionVariable\n");
    if (int ret = __wrap_pthread_cond_init(&cond, NULL)) {
        if (EPERM == ret) {
            xprintf("mutex init returned EPERM\n");
            initialize_xenomai();
            if (int ret = __wrap_pthread_cond_init(&cond, NULL)) {
                fprintf(stderr, "Error: unable to create condition variable : (%d) %s\n", ret, strerror(ret));
                return;
            }
        }
        return;
    }
    enabled = true;
}

XenomaiConditionVariable::~XenomaiConditionVariable() {
    if (enabled) {
        notify_all();
        __wrap_pthread_cond_destroy(&cond);
    }
}

void XenomaiConditionVariable::wait(std::unique_lock<XenomaiMutex>& lck, bool recurred) {
    // If any parameter has a value that is not valid for this function (such as if lck's mutex object is not locked by
    // the calling thread), it causes undefined behavior.

    // Otherwise, if an exception is thrown, both the condition_variable_any object and the arguments are in a valid
    // state (basic guarantee). Additionally, on exception, the state of lck is attempted to be restored before exiting
    // the function scope (by calling lck.lock()).

    // It may throw system_error in case of failure (transmitting any error condition from the respective call to lock
    // or unlock). The predicate version (2) may also throw exceptions thrown by pred.
    const char name[] = "wait";
    try_or_retry([this, &lck]() { return __wrap_pthread_cond_wait(&this->cond, &lck.mutex()->mutex); }, &cond, name,
                 enabled);
}

void XenomaiConditionVariable::notify_one(bool recurred) noexcept {
    const char name[] = "notify_one";
    try_or_retry([this]() { return __wrap_pthread_cond_signal(&this->cond); }, &cond, name, enabled);
}

void XenomaiConditionVariable::notify_all(bool recurred) noexcept {
    const char name[] = "notify_all";
    try_or_retry([this]() { return __wrap_pthread_cond_broadcast(&this->cond); }, &cond, name, enabled);
}
