#ifndef _THREAD_H_
#define _THREAD_H_


#if !defined(__OS_WINDOWS__) && !defined(__OS_UNIX__)
    #if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
        #define __OS_WINDOWS__
    #elif defined(__linux__) || defined(__APPLE__)
        #define __OS_UNIX__
        #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
        #endif
    #else
        #error "Unsupported platforms."
    #endif
#endif


#include <stdlib.h>
#include <stdint.h>


#if defined(__OS_WINDOWS__)
    #include <windows.h>
    #include <process.h>
#elif defined(__OS_UNIX__)
    #include <pthread.h>
#endif


#if defined(__OS_UNIX__)
    typedef pthread_t Thread;
    typedef pthread_mutex_t Mutex;
    typedef pthread_cond_t ThreadCondition;
#elif defined(__OS_WINDOWS__)
    typedef HANDLE Thread;
    typedef struct Mutex {
        CRITICAL_SECTION cs;
        int status;
        int recursive;
    } Mutex;
    typedef struct ThreadCondition {
        HANDLE events[2];
        unsigned int waiter_count;
        CRITICAL_SECTION cs;
    } ThreadCondition;
#endif


typedef int (*_ThreadFunction)(void *);


typedef struct _ThreadInformation {
    _ThreadFunction ptr;
    void *args;
} _ThreadInformation;


#if defined(__OS_UNIX__)
    void *__thread_wrapper__(void *args);
#elif defined(__OS_WINDOWS__)
    unsigned WINAPI __thread_wrapper__(void *args);
    int __condition_timedwait_win32__(ThreadCondition *condition, Mutex *mutex, DWORD timeout);
#endif


/**
 * @brief Create a thread.
 * @param thread The pointer of thread.
 * @param func The pointer of thread function. `int func(void *args);`
 * @param args The arguments of thread function.
 * @return `0` for success.
**/
int thread_create(Thread *thread, _ThreadFunction func, void *args);


/**
 * @brief Wait for the thread to complete with blocking.
 * @param thread The pointer of thread.
 * @param result The return value of thread function (`NULL` for default).
 * @return `0` for success.
**/
int thread_join(Thread *thread, int *result);


/**
 * @brief Detach the thread without blocking.
 * @param thread The pointer of thread.
 * @return `0` for success.
**/
int thread_detach(Thread *thread);


/**
 * @brief Exit the current thread.
**/
void thread_exit(void);


/**
 * @brief Create a mutex object.
 * @param mutex The pointer of mutex object.
 * @param type The type of mutex lock (`1` for default).
 * @return `0` for success.
**/
int mutex_create(Mutex *mutex, int type);


/**
 * @brief Release any resources used by the given mutex.
**/
void mutex_destroy(Mutex *mutex);


/**
 * @brief Lock the mutex with blocking.
 * @param mutex The pointer of mutex object.
 * @return `0` for success.
**/
int mutex_lock(Mutex *mutex);


/**
 * @brief Lock the mutex without blocking (so it is possible to lock successfully).
 * @param mutex The pointer of mutex object.
 * @return `0` for success.
**/
int mutex_trylock(Mutex *mutex);


/**
 * @brief Unlock the mutex.
 * @param mutex The pointer of mutex object.
 * @return `0` for success.
**/
int mutex_unlock(Mutex *mutex);


/**
 * @brief Initialize the condition variable.
 * @param condition The pointer of condition variable object.
 * @return `0` for success, `1` for failure.
**/
int condition_init(ThreadCondition *condition);


/**
 * @brief Destroy the condition variable.
 * @param condition The pointer of condition variable object.
**/
void condition_destroy(ThreadCondition *condition);


/**
 * @brief Wait for the condition variable.
 * @param condition The pointer of condition variable object.
 * @param mutex The pointer of the associated mutex (should be locked before calling).
 * @return `0` for success, `1` for failure.
**/
int condition_wait(ThreadCondition *condition, Mutex *mutex);


/**
 * @brief Signal the condition variable to wake up one waiting thread.
 * @param condition The pointer of condition variable object.
 * @return `0` for success, `1` for failure.
**/
int condition_signal(ThreadCondition *condition);


/**
 * @brief Broadcast the condition variable to wake up all waiting threads.
 * @param condition The pointer of condition variable object.
 * @return `0` for success, `1` for failure.
**/
int condition_broadcast(ThreadCondition *condition);


#endif
