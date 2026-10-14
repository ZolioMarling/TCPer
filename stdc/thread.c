#include "thread.h"


#if defined(__OS_UNIX__)
    void *__thread_wrapper__(void *args) {
        _ThreadInformation *info = (_ThreadInformation *)args;
        _ThreadFunction f = info->ptr;
        void *x = info->args;
        free(info);
        return (void *)(intptr_t)f(x);
    }
#elif defined(__OS_WINDOWS__)
    unsigned WINAPI __thread_wrapper__(void *args) {
        _ThreadInformation *info = (_ThreadInformation *)args;
        _ThreadFunction f = info->ptr;
        void *x = info->args;
        free(info);
        return f(x);
    }
#endif


int thread_create(Thread *thread, _ThreadFunction func, void *args) {
    if (thread == NULL || func == NULL) return 1;

    _ThreadInformation *info = malloc(sizeof(*info));
    if (info == NULL) return 1;

    info->ptr = func;
    info->args = args;

    #if defined(__OS_UNIX__)
        if (pthread_create(thread, NULL, __thread_wrapper__, (void *)info) != 0) {
            free(info);
            return 1;
        }
    #elif defined(__OS_WINDOWS__)
        *thread = (HANDLE)_beginthreadex(NULL, 0, __thread_wrapper__, (void *)info, 0, NULL);
        if (!*thread) {
            free(info);
            return 1;
        }
    #endif
    return 0;
}


int thread_join(Thread *thread, int *result) {
    if (thread == NULL) return 1;

    #if defined(__OS_UNIX__)
        void *u;
        if (pthread_join(*thread, &u) != 0) return 1;
        if (result != NULL) *result = (int)(intptr_t)u;
    #elif defined(__OS_WINDOWS__)
        if (*thread == NULL || WaitForSingleObject(*thread, INFINITE) != WAIT_OBJECT_0) return 1;

        int failed = 0;
        if (result != NULL) {
            DWORD exit_code;
            if (GetExitCodeThread(*thread, &exit_code) == 0) failed = 1;
            else *result = (int)exit_code;
        }
        if (CloseHandle(*thread) == 0) failed = 1;
        else *thread = NULL;
        if (failed) return 1;
    #endif
    return 0;
}


int thread_detach(Thread *thread) {
    if (thread == NULL) return 1;

    #if defined(__OS_UNIX__)
        return pthread_detach(*thread) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        if (*thread == NULL || CloseHandle(*thread) == 0) return 1;
        *thread = NULL;
        return 0;
    #endif
}


void thread_exit(void) {
    #if defined(__OS_UNIX__)
        pthread_exit(NULL);
    #elif defined(__OS_WINDOWS__)
        _endthreadex(0);
    #endif
}


int mutex_create(Mutex *mutex, int type) {
    if (mutex == NULL) return 1;

    #if defined(__OS_UNIX__)
        pthread_mutexattr_t t;
        if (pthread_mutexattr_init(&t) != 0) return 1;
        if ((type & 8) && pthread_mutexattr_settype(&t, PTHREAD_MUTEX_RECURSIVE) != 0) {
            pthread_mutexattr_destroy(&t);
            return 1;
        }
        int res = pthread_mutex_init(mutex, &t);
        pthread_mutexattr_destroy(&t);
        return res == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        mutex->status = 0;
        mutex->recursive = type & 8;
        InitializeCriticalSection(&mutex->cs);
        return 0;
    #endif
}


void mutex_destroy(Mutex *mutex) {
    #if defined(__OS_UNIX__)
        pthread_mutex_destroy(mutex);
    #elif defined(__OS_WINDOWS__)
        DeleteCriticalSection(&mutex->cs);
    #endif
}


int mutex_lock(Mutex *mutex) {
    #if defined(__OS_UNIX__)
        return pthread_mutex_lock(mutex) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        EnterCriticalSection(&mutex->cs);
        if (!mutex->recursive) {
            while (mutex->status) Sleep(1000);  // Simulate deadlock.
            mutex->status = 1;
        }
        return 0;
    #endif
}


int mutex_trylock(Mutex *mutex) {
    #if defined(__OS_UNIX__)
        return pthread_mutex_trylock(mutex) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        if (!TryEnterCriticalSection(&mutex->cs)) return 1;
        if (!mutex->recursive && mutex->status) {
            LeaveCriticalSection(&mutex->cs);
            return 1;
        }
        if (!mutex->recursive) mutex->status = 1;
        return 0;
    #endif
}


int mutex_unlock(Mutex *mutex) {
    #if defined(__OS_UNIX__)
        return pthread_mutex_unlock(mutex) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        mutex->status = 0;
        LeaveCriticalSection(&mutex->cs);
        return 0;
    #endif
}


int condition_init(ThreadCondition *condition) {
    if (condition == NULL) return 1;

    #if defined(__OS_UNIX__)
        return pthread_cond_init(condition, NULL) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        condition->waiter_count = 0;
        InitializeCriticalSection(&condition->cs);
        condition->events[0] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (condition->events[0] == NULL) {
            condition->events[1] = NULL;
            DeleteCriticalSection(&condition->cs);
            return 1;
        }
        condition->events[1] = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (condition->events[1] == NULL) {
            CloseHandle(condition->events[0]);
            condition->events[0] = NULL;
            DeleteCriticalSection(&condition->cs);
            return 1;
        }
        return 0;
    #endif
}


void condition_destroy(ThreadCondition *condition) {
    #if defined(__OS_UNIX__)
        pthread_cond_destroy(condition);
    #elif defined(__OS_WINDOWS__)
        if (condition->events[0] != NULL) CloseHandle(condition->events[0]);
        if (condition->events[1] != NULL) CloseHandle(condition->events[1]);
        DeleteCriticalSection(&condition->cs);
    #endif
}


int condition_wait(ThreadCondition *condition, Mutex *mutex) {
    #if defined(__OS_UNIX__)
        return pthread_cond_wait(condition, mutex) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        return __condition_timedwait_win32__(condition, mutex, INFINITE);
    #endif
}


int condition_signal(ThreadCondition *condition) {
    #if defined(__OS_UNIX__)
        return pthread_cond_signal(condition) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        EnterCriticalSection(&condition->cs);
        int have_waiters = (condition->waiter_count > 0);
        LeaveCriticalSection(&condition->cs);
        if (have_waiters) if (SetEvent(condition->events[0]) == 0) return 1;
        return 0;
    #endif
}


int condition_broadcast(ThreadCondition *condition) {
    #if defined(__OS_UNIX__)
        return pthread_cond_broadcast(condition) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        EnterCriticalSection(&condition->cs);
        int have_waiters = (condition->waiter_count > 0);
        LeaveCriticalSection(&condition->cs);
        if (have_waiters) if (SetEvent(condition->events[1]) == 0) return 1;
        return 0;
    #endif
}


#if defined(__OS_WINDOWS__)
    int __condition_timedwait_win32__(ThreadCondition *condition, Mutex *mutex, DWORD timeout) {
        EnterCriticalSection(&condition->cs);
        ++condition->waiter_count;
        LeaveCriticalSection(&condition->cs);

        if (mutex_unlock(mutex) != 0) {
            EnterCriticalSection(&condition->cs);
            --condition->waiter_count;
            LeaveCriticalSection(&condition->cs);
            return 1;
        }

        DWORD result = WaitForMultipleObjects(2, condition->events, FALSE, timeout);
        EnterCriticalSection(&condition->cs);
        --condition->waiter_count;
        int last_waiter = (result == (WAIT_OBJECT_0 + 1)) && (condition->waiter_count == 0);
        LeaveCriticalSection(&condition->cs);

        int failed = result != WAIT_OBJECT_0 && result != (WAIT_OBJECT_0 + 1);
        if (last_waiter && ResetEvent(condition->events[1]) == 0) failed = 1;
        if (mutex_lock(mutex) != 0) failed = 1;
        return failed;
    }
#endif
