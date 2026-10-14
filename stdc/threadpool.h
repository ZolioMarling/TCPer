#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_


#include "thread.h"


typedef struct ThreadPool ThreadPool;


/**
 * Typical ownership handoff:
 *
 *     ThreadPool *pool = threadpool_create(4, 64);
 *     if (!pool) return 1;
 *
 *     // Retain before giving `pool` to another thread.
 *     if (threadpool_retain(pool) != 0) {
 *         threadpool_destroy(pool, 0);
 *         return 1;
 *     }
 *     // The receiving thread uses `pool`, then calls threadpool_release(pool).
 *     // The creator joins that thread, then calls threadpool_destroy(pool, 1).
 *
 * Never call threadpool_retain() for the first time from a thread that only
 * received a raw pointer: another owner could destroy the pool first.
**/

/**
 * Private mutex, condition-variable, or worker failures are unrecoverable and
 * terminate the process with `abort()`. API return values only report normal
 * operational failures such as invalid arguments, a full queue, or shutdown.
**/


/**
 * @brief Create a thread pool.
 * @param n_workers The number of threads.
 * @param queue_capacity The maximum length of thread queue.
 * @return `NULL` for failure.
**/
ThreadPool *threadpool_create(int n_workers, int queue_capacity);


/**
 * @brief Acquire an additional ownership reference to a thread pool.
 * @param pool A pool for which the caller already owns a reference.
 * @return `0` for success, `1` for failure or a closing/destroyed pool.
 *
 * The creator owns the initial reference. A thread must acquire a reference
 * before another owner can call `threadpool_destroy`, then release it after
 * it has stopped using the handle. Retaining is rejected once destruction
 * starts. This makes concurrent API entry and destruction safe without
 * dereferencing a freed raw pointer.
**/
int threadpool_retain(ThreadPool *pool);


/**
 * @brief Release an ownership reference.
 * @param pool A pool reference owned by the caller.
 * @return `0` for success, `1` if this is the final reference before destruction.
 *
 * The creator must retain one reference until it calls `threadpool_destroy`.
 * That successful destroy consumes the creator reference. Each additional
 * reference acquired with `threadpool_retain` must be released exactly once.
**/
int threadpool_release(ThreadPool *pool);


/**
 * @brief Add a task to the thread pool.
 * @param func The pointer of task function like `void func(void *args)`.
 * @param args The arguments of task function.
 * @param block `1` for blocking and waiting when the queue is full, `0` for returning an error immediately.
 * A worker of this pool never blocks when the queue is full, to avoid a self-deadlock.
 * @param cleanup Called after an accepted task completes, or by an immediate destroy for a discarded task.
 * The caller retains ownership of `args` when this function fails.
 * A task must return normally; it must not terminate, cancel, or non-locally exit its worker thread.
 * The caller must own a pool reference, unless this is called by one of the pool's workers.
 * @return `0` for success, `1` for failure or a shutting-down pool, `2` for full queue.
**/
int threadpool_add(ThreadPool *pool, void (*func)(void *args), void *args, int block, void (*cleanup)(void *args));


/**
 * @brief Wait thread task in pool with blocking.
 * @param pool The pointer of thread pool.
 * The caller must own a pool reference.
 * @return `0` when all tasks and submissions already in progress complete, `1` for failure,
 * shutdown, or a call from one of this pool's workers.
**/
int threadpool_wait(ThreadPool *pool);


/**
 * @brief Destroy the thread pool and release the memory.
 * @param pool The pointer of thread pool.
 * @param safe_exit `1` for awaiting all tasks to be completed, `0` for exiting immediately.
 * @return `0` for success, `1` for failure or a call from one of this pool's workers.
 *
 * Calls racing with destruction either enter before shutdown and are allowed to return, or are rejected.
 * Every caller must own a reference. On success this function consumes its reference; other retained
 * references remain valid only for observing rejection and must be released with `threadpool_release`.
**/
int threadpool_destroy(ThreadPool *pool, int safe_exit);


/**
 * @brief Destroy a thread pool and optionally report tasks discarded by an
 * immediate shutdown.
 * @param discarded_tasks Receives the number of queued tasks removed when
 * `safe_exit` is `0`; it may be `NULL`.
 *
 * This has the same ownership and return-value contract as
 * `threadpool_destroy()`.
 **/
int threadpool_destroy_ex(ThreadPool *pool, int safe_exit, size_t *discarded_tasks);


#endif
