/*
 * Copyright (c) 2015 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#elif defined(__NetBSD__)
#include <lwp.h>
#endif

#include <unistd.h>
#include "roc_core/errno_to_str.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/thread.h"
#include "roc_core/macro_helpers.h"

uint64_t NAME_LEN = 32;
const char* prepend = "roc-";

namespace roc {
namespace core {

bool Thread::set_name(char* new_name){


int rc; 

// edited my email.. . hoepe this 
// 


#if defined(__FreeBSD__)
pthread_set_name_np(get_tid(), new_name);
// No direct way of actually checking this works or not because FreeBSD and OpenBSD doesn't have get_name equivalent...
return true;

#elif defined(__OpenBSD__)
pthread_set_name_np(get_tid(), new_name);
// No direct way of actually checking this works or not because FreeBSD and OpenBSD doesn't have get_name equivalent...
return true;

#elif defined(__NetBSD__)
rc = pthread_setname_np(thread_, new_name, nullptr);

if (rc != 0){
    roc_log(LogError, "thread: unable to set new name: %s", new_name);
    return false;
}
else{
    return true;
}

#else
rc = pthread_setname_np(thread_, new_name);

    if (rc != 0){
        roc_log(LogError, "thread: unable to set new name: %s", new_name);
        return false;
    }
    else{

        // char *actual = strdup("");

        // pthread_getname_np(thread_, actual, NAME_LEN);
        // if (actual != bfr){
        //     roc_log(LogError, "thread: tried to set name as '%s' but instead was actually '%s'?", bfr, actual);
        //     return false;
        // }

        return true;
    }
#endif

    // char* bfr = strdup(new_name);
    // int rc = pthread_setname_np(thread_, bfr);

}

void Thread::get_name(char * buffer){


    int rc;

    rc = pthread_getname_np(thread_, buffer, NAME_LEN);

    if (rc != 0){
        roc_log(LogError, "thread: name of thread could not be obtained");
    }
}

uint64_t Thread::get_pid() {
    return (uint64_t)getpid();
}

uint64_t Thread::get_tid() {
#if defined(SYS_gettid)
    return (uint64_t)(pid_t)syscall(SYS_gettid);
#elif defined(__FreeBSD__)
    return (uint64_t)pthread_getthreadid_np();
#elif defined(__NetBSD__)
    return (uint64_t)_lwp_self();
#elif defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return tid;
#elif defined(__ANDROID__)
    return (uint64_t)gettid();
#else
    return (uint64_t)pthread_self();
#endif
}

bool Thread::set_realtime() {
    sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = sched_get_priority_max(SCHED_RR);

    if (int err = pthread_setschedparam(pthread_self(), SCHED_RR, &param)) {
        roc_log(LogDebug,
                "thread: can't set realtime priority: pthread_setschedparam(): %s",
                errno_to_str(err).c_str());
        return false;
    }

    return true;
}

Thread::Thread()
    : started_(0)
    , joinable_(0) {
}

Thread::~Thread() {
    if (joinable()) {
        roc_panic("thread: thread was not joined before calling destructor");
    }
}

bool Thread::joinable() const {
    return joinable_;
}

bool Thread::start() {
    Mutex::Lock lock(mutex_);

    if (started_) {
        roc_log(LogError, "thread: can't start thread more than once");
        return false;
    }

    if (int err = pthread_create(&thread_, NULL, &Thread::thread_runner_, this)) {
        roc_log(LogError, "thread: pthread_thread_create(): %s",
                errno_to_str(err).c_str());
        return false;
    }

    started_ = 1;
    joinable_ = 1;

    return true;
}

void Thread::join() {
    Mutex::Lock lock(mutex_);

    if (!joinable_) {
        return;
    }

    if (int err = pthread_join(thread_, NULL)) {
        roc_panic("thread: pthread_thread_join(): %s", errno_to_str(err).c_str());
    }

    joinable_ = 0;
}

void* Thread::thread_runner_(void* ptr) {
    static_cast<Thread*>(ptr)->run();
    return NULL;
}

} // namespace core
} // namespace roc
