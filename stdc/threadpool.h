#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_


#if defined(_WIN32)
    #if !defined(_WIN32_WINNT)
        #define _WIN32_WINNT 0x0600
    #elif _WIN32_WINNT < 0x0600
        #error "The thread pool requires Windows Vista or later."
    #endif
#endif


#include <limits.h>
#include <stdint.h>
#include <stdlib.h>


#include "type.h"
#include "thread.h"


#if defined(__OS_WINDOWS__)
    typedef CONDITION_VARIABLE _PoolCondition;
#else
    typedef ThreadCondition _PoolCondition;
#endif


typedef struct ThreadPool ThreadPool;


typedef struct ThreadPoolTask {
    void (*func)(void *args);
    void *args;
    void (*cleanup)(void *args);
} ThreadPoolTask;


typedef struct _ThreadPoolStats {
    int workers;
    int active_tasks;   // Includes callbacks currently running cleanup.
    int idle_workers;   // Workers currently waiting for tasks.
    unsigned int waiting_submitters;
    unsigned int waiting_clients;
    unsigned int pending_submissions;   // Submission calls in progress but not individual tasks.
    usize queued_tasks;
    usize queue_capacity;
    usize queue_peak;
    uint64 accepted;
    uint64 completed;   // Callback and cleanup have both returned.
    uint64 discarded;
    int closing;
    int destroyed;
} _ThreadPoolStats;


/**
 * @brief Create a thread pool.
 * @param n_workers The number of threads.
 * @param queue_capacity The maximum length of thread queue.
 * @return `NULL` for failure.
 * @note Windows builds require Windows Vista or later for native condition variables.
**/
ThreadPool *threadpool_create(int n_workers, int queue_capacity);


/**
 * @brief Add a task to the thread pool.
 * @param func The pointer of task function like `void func(void *args)`.
 * @param args The arguments of task function.
 * @param block `1` for blocking and waiting when the queue is full, `0` for returning an error immediately.
 * @param cleanup The thread is responsible for releasing the memory like `void cleanup(void *args)` and `NULL` for no cleaning. The caller retains ownership of `args` when this function fails. A task must return normally; it must not terminate, cancel, or non-locally exit its worker thread. The pool must remain alive for the duration of this call.
 * @return `0` for success, `1` for failure or a shutting-down pool, `2` for full queue.
 * @note A worker of this pool never blocks when the queue is full, to avoid a self-deadlock.
**/
int threadpool_add(ThreadPool *pool, void (*func)(void *args), void *args, int block, void (*cleanup)(void *args));


/**
 * @brief Submit multiple tasks to the thread pool at once.
 * @param pool The thread pool that will execute the tasks.
 * @param tasks An array of task descriptors. Every `func` must be not `NULL`.
 * @param count The number of descriptors. It must be between `1` and the pool's queue capacity, inclusive.
 * @param block `1`: when the queue is full, it waits until there is enough space to receive the task. `0`: return immediately if the queue space is insufficient; do not wait.
 * @return `0` if every task was accepted, `1` for invalid arguments or a shutting-down pool, or `2` when the complete batch cannot fit without waiting.
 * @example
 * @code
ThreadPoolTask tasks[] = {
    {process_item, &items[0], NULL},
    {process_item, &items[1], NULL},
    {process_item, &items[2], NULL}
};
usize count = sizeof(tasks) / sizeof(tasks[0]);
int result = threadpool_add_batch(pool, tasks, count, 1);
 * @endcode
**/
int threadpool_add_batch(ThreadPool *pool, ThreadPoolTask *tasks, usize count, int block);


/**
 * @brief Wait thread task in pool with blocking.
 * @param pool The pointer of thread pool.
 * @return `0` when all tasks and submissions already in progress complete, `1` for failure, shutdown, or a call from one of this pool's workers.
 * @note The pool must remain alive for the duration of this call.
**/
int threadpool_wait(ThreadPool *pool);


/**
 * @brief Obtain a consistent snapshot of the current state of the thread pool.
 * @param pool The pointer to the thread pool.
 * @param stats The structure that receives the current statistics.
 * @return Returns `0` for success, `1` for a `NULL` argument. 
**/
int threadpool_stats(ThreadPool *pool, _ThreadPoolStats *stats);


/**
 * @brief Destroy the thread pool and release the memory.
 * @param pool The pointer of thread pool.
 * @param safe_exit `1` drains accepted tasks; `0` discards queued tasks.
 * @return `0` for success, `1` for failure or a call from one of this pool's workers.
 * @note Both modes wait for running tasks and their cleanup; neither cancels a task. User code must stop or join every external thread that can access the pool before calling this function. On success, the pool handle must not be used again.
**/
int threadpool_destroy(ThreadPool *pool, int safe_exit);


/**
 * @brief When destroying the thread pool, return the number of queued tasks that were discarded.
 * @param pool The pointer of thread pool.
 * @param safe_exit `1` drains accepted tasks; `0` discards queued tasks.
 * @param discarded_tasks Receives the number of queued tasks removed when `safe_exit` is `0`; it may be `NULL`.
 * @return `0` for success, `1` for failure or a call from one of this pool's workers.
**/
int __threadpool_destroy__(ThreadPool *pool, int safe_exit, usize *discarded_tasks);


/**
 * @brief Acquire an internal reference to the thread pool.
 * @param pool The pointer to the thread pool.
 * @return `0` for success, `1` for failure or a closing / destroyed pool.
**/
int __threadpool_retain__(ThreadPool *pool);


/**
 * @brief Release an internal reference to the thread pool.
 * @param pool The pointer to the thread pool.
 * @return `0` for success, `1` for failure.
**/
int __threadpool_release__(ThreadPool *pool);


#endif
