#include "threadpool.h"


#if defined(_MSC_VER) && !defined(__clang__)
    static __declspec(thread) ThreadPool *threadpool_current;
#else
    static _Thread_local ThreadPool *threadpool_current;
#endif


static int __threadpool_worker__(void *args);
static void __threadpool_lock__(ThreadPool *pool);
static void __threadpool_unlock__(ThreadPool *pool);
static int __threadpool_condition_init__(_PoolCondition *condition);
static void __threadpool_condition_destroy__(_PoolCondition *condition);
static void __threadpool_condition_wait__(ThreadPool *pool, _PoolCondition *condition);
static void __threadpool_condition_signal__(_PoolCondition *condition);
static void __threadpool_condition_broadcast__(_PoolCondition *condition);
static void __threadpool_join__(Thread *thread);
static void __threadpool_free__(ThreadPool *pool);
static void __threadpool_free_initialized__(ThreadPool *pool);
static void __threadpool_count__(uint64 *counter, usize count);
static void __threadpool_signal_idle__(ThreadPool *pool);
static void __threadpool_leave__(ThreadPool *pool);
static void __threadpool_submit_leave__(ThreadPool *pool);
static void __threadpool_broadcast_all__(ThreadPool *pool);


// All synchronization objects survive until the final owned reference.
struct ThreadPool {
    Mutex lock;
    _PoolCondition notify;
    _PoolCondition not_full;
    _PoolCondition all_idle;
    _PoolCondition api_idle;
    unsigned int references;
    unsigned int api_users;
    unsigned int pending_submissions;
    unsigned int waiting_submitters;
    unsigned int waiting_batches;
    unsigned int waiting_clients;
    int waiting_workers;
    int shutdown;
    int destroyed;
    int n_workers;
    int n_working;
    Thread *threads;
    ThreadPoolTask *queue;
    usize queue_length;
    usize queue_capacity;
    usize queue_head;
    usize queue_tail;
    usize queue_peak;
    uint64 accepted;
    uint64 completed;
    uint64 discarded;
};


ThreadPool *threadpool_create(int n_workers, int queue_capacity) {
    if (n_workers <= 0 || queue_capacity <= 0) return NULL;
    if (
        (usize)queue_capacity > SIZE_MAX / sizeof(ThreadPoolTask)
        ||
        (usize)n_workers > SIZE_MAX / sizeof(Thread)
    ) return NULL;

    ThreadPool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->queue = calloc((usize)queue_capacity, sizeof(*pool->queue));
    pool->threads = calloc((usize)n_workers, sizeof(*pool->threads));
    if (!pool->queue || !pool->threads) {
        __threadpool_free__(pool);
        return NULL;
    }

    pool->references = 1;
    pool->n_workers = n_workers;
    pool->queue_capacity = (usize)queue_capacity;

    if (mutex_create(&pool->lock, 1) != 0) goto fail_alloc;
    if (__threadpool_condition_init__(&pool->notify) != 0) goto fail_lock;
    if (__threadpool_condition_init__(&pool->not_full) != 0) goto fail_notify;
    if (__threadpool_condition_init__(&pool->all_idle) != 0) goto fail_not_full;
    if (__threadpool_condition_init__(&pool->api_idle) != 0) goto fail_all_idle;

    int n;
    for (n = 0; n < n_workers; n++) {
        if (thread_create(&pool->threads[n], __threadpool_worker__, pool) != 0) {
            __threadpool_lock__(pool);
            pool->shutdown = 1;
            __threadpool_condition_broadcast__(&pool->notify);
            __threadpool_unlock__(pool);
            while (n > 0) __threadpool_join__(&pool->threads[--n]);
            __threadpool_free_initialized__(pool);
            return NULL;
        }
    }
    return pool;

    fail_all_idle: __threadpool_condition_destroy__(&pool->all_idle);
    fail_not_full: __threadpool_condition_destroy__(&pool->not_full);
    fail_notify: __threadpool_condition_destroy__(&pool->notify);
    fail_lock: mutex_destroy(&pool->lock);
    fail_alloc: __threadpool_free__(pool);

    return NULL;
}


int threadpool_add(ThreadPool *pool, void (*func)(void *args), void *args, int block, void (*cleanup)(void *args)) {
    ThreadPoolTask task = {func, args, cleanup};
    return threadpool_add_batch(pool, &task, 1, block);
}


int threadpool_add_batch(ThreadPool *pool, ThreadPoolTask *tasks, usize count, int block) {
    if (!pool || !tasks || count == 0) return 1;

    __threadpool_lock__(pool);

    if (pool->shutdown || pool->api_users == UINT_MAX || count > pool->queue_capacity) {
        __threadpool_unlock__(pool);
        return 1;
    }

    for (usize i = 0; i < count; i++) {
        if (!tasks[i].func) {
            __threadpool_unlock__(pool);
            return 1;
        }
    }

    pool->api_users++;
    pool->pending_submissions++;

    while (count > pool->queue_capacity - pool->queue_length && !pool->shutdown) {
        if (!block || threadpool_current == pool) {
            __threadpool_submit_leave__(pool);
            return 2;
        }
        pool->waiting_submitters++;
        if (count > 1) pool->waiting_batches++;
        __threadpool_condition_wait__(pool, &pool->not_full);
        if (count > 1) pool->waiting_batches--;
        pool->waiting_submitters--;
    }

    if (pool->shutdown) {
        __threadpool_submit_leave__(pool);
        return 1;
    }

    for (usize i = 0; i < count; i++) {
        pool->queue[pool->queue_tail] = tasks[i];
        if (++pool->queue_tail == pool->queue_capacity) pool->queue_tail = 0;
    }

    pool->queue_length = pool->queue_length + count;
    if (pool->queue_length > pool->queue_peak) pool->queue_peak = pool->queue_length;
    __threadpool_count__(&pool->accepted, count);

    if (pool->waiting_workers) {
        if (count >= (usize)pool->waiting_workers && count > 1) __threadpool_condition_broadcast__(&pool->notify);
        else for (usize i = 0; i < count; i++) __threadpool_condition_signal__(&pool->notify);
    }

    __threadpool_submit_leave__(pool);
    return 0;
}


int threadpool_wait(ThreadPool *pool) {
    if (!pool || threadpool_current == pool) return 1;

    __threadpool_lock__(pool);

    if (pool->shutdown || pool->api_users == UINT_MAX) {
        __threadpool_unlock__(pool);
        return 1;
    }

    pool->api_users++;
    pool->waiting_clients++;

    while (
        !pool->shutdown
        &&
        (pool->queue_length > 0 || pool->n_working > 0 || pool->pending_submissions > 0)
    ) __threadpool_condition_wait__(pool, &pool->all_idle);

    pool->waiting_clients--;
    int status = pool->shutdown ? 1 : 0;

    __threadpool_leave__(pool);
    return status;
}


int threadpool_stats(ThreadPool *pool, _ThreadPoolStats *stats) {
    if (!pool || !stats) return 1;
    __threadpool_lock__(pool);
    stats->workers = pool->n_workers;
    stats->active_tasks = pool->n_working;
    stats->idle_workers = pool->waiting_workers;
    stats->waiting_submitters = pool->waiting_submitters;
    stats->waiting_clients = pool->waiting_clients;
    stats->pending_submissions = pool->pending_submissions;
    stats->queued_tasks = pool->queue_length;
    stats->queue_capacity = pool->queue_capacity;
    stats->queue_peak = pool->queue_peak;
    stats->accepted = pool->accepted;
    stats->completed = pool->completed;
    stats->discarded = pool->discarded;
    stats->closing = pool->shutdown;
    stats->destroyed = pool->destroyed;
    __threadpool_unlock__(pool);
    return 0;
}


int threadpool_destroy(ThreadPool *pool, int safe_exit) {
    return __threadpool_destroy__(pool, safe_exit, NULL);
}


int __threadpool_destroy__(ThreadPool *pool, int safe_exit, usize *discarded_tasks) {
    if (discarded_tasks) *discarded_tasks = 0;
    if (!pool || threadpool_current == pool) return 1;

    __threadpool_lock__(pool);
    if (pool->shutdown) {
        __threadpool_unlock__(pool);
        return 1;
    }
    pool->shutdown = 1;

    usize discarded_head = 0;
    usize discarded_count = 0;

    if (!safe_exit) {
        discarded_head = pool->queue_head;
        discarded_count = pool->queue_length;
        pool->queue_length = 0;
        __threadpool_count__(&pool->discarded, discarded_count);
    }

    if (discarded_tasks) *discarded_tasks = discarded_count;

    __threadpool_broadcast_all__(pool);
    __threadpool_unlock__(pool);

    for (usize i = 0; i < discarded_count; i++) {
        ThreadPoolTask *task = &pool->queue[discarded_head];
        if (task->cleanup) task->cleanup(task->args);
        if (++discarded_head == pool->queue_capacity) discarded_head = 0;
    }

    __threadpool_lock__(pool);
    while (pool->api_users != 0) __threadpool_condition_wait__(pool, &pool->api_idle);
    __threadpool_unlock__(pool);

    for (int worker = 0; worker < pool->n_workers; worker++) __threadpool_join__(&pool->threads[worker]);

    __threadpool_lock__(pool);
    free(pool->threads);
    pool->threads = NULL;
    free(pool->queue);
    pool->queue = NULL;
    pool->destroyed = 1;
    __threadpool_unlock__(pool);

    return __threadpool_release__(pool);
}


int __threadpool_retain__(ThreadPool *pool) {
    if (!pool) return 1;
    __threadpool_lock__(pool);
    if (pool->shutdown || pool->references == UINT_MAX) {
        __threadpool_unlock__(pool);
        return 1;
    }
    pool->references++;
    __threadpool_unlock__(pool);
    return 0;
}


int __threadpool_release__(ThreadPool *pool) {
    if (!pool) return 1;
    __threadpool_lock__(pool);
    if (pool->references == 1 && !pool->destroyed) {
        __threadpool_unlock__(pool);
        return 1;
    }
    int free_pool = --pool->references == 0;
    __threadpool_unlock__(pool);
    if (free_pool) __threadpool_free_initialized__(pool);
    return 0;
}


static void __threadpool_lock__(ThreadPool *pool) {
    if (mutex_lock(&pool->lock) != 0) abort();
}


static void __threadpool_unlock__(ThreadPool *pool) {
    if (mutex_unlock(&pool->lock) != 0) abort();
}


static int __threadpool_condition_init__(_PoolCondition *condition) {
    #if defined(__OS_WINDOWS__)
        InitializeConditionVariable(condition);
        return 0;
    #else
        return condition_init(condition);
    #endif
}


static void __threadpool_condition_destroy__(_PoolCondition *condition) {
    #if defined(__OS_WINDOWS__)
        (void)condition;
    #else
        condition_destroy(condition);
    #endif
}


static void __threadpool_condition_wait__(ThreadPool *pool, _PoolCondition *condition) {
    #if defined(__OS_WINDOWS__)
        // The pool always holds this nonrecursive critical section exactly once.
        pool->lock.status = 0;
        BOOL success = SleepConditionVariableCS(condition, &pool->lock.cs, INFINITE);
        pool->lock.status = 1;
        if (!success) abort();
    #else
        if (condition_wait(condition, &pool->lock) != 0) abort();
    #endif
}


static void __threadpool_condition_signal__(_PoolCondition *condition) {
    #if defined(__OS_WINDOWS__)
        WakeConditionVariable(condition);
    #else
        if (condition_signal(condition) != 0) abort();
    #endif
}


static void __threadpool_condition_broadcast__(_PoolCondition *condition) {
    #if defined(__OS_WINDOWS__)
        WakeAllConditionVariable(condition);
    #else
        if (condition_broadcast(condition) != 0) abort();
    #endif
}


static void __threadpool_join__(Thread *thread) {
    int status;
    if (thread_join(thread, &status) != 0 || status != 0) abort();
}


static void __threadpool_free__(ThreadPool *pool) {
    free(pool->threads);
    free(pool->queue);
    free(pool);
}


static void __threadpool_free_initialized__(ThreadPool *pool) {
    __threadpool_condition_destroy__(&pool->api_idle);
    __threadpool_condition_destroy__(&pool->all_idle);
    __threadpool_condition_destroy__(&pool->not_full);
    __threadpool_condition_destroy__(&pool->notify);
    mutex_destroy(&pool->lock);
    __threadpool_free__(pool);
}


static void __threadpool_count__(uint64 *counter, usize count) {
    *counter = count > UINT64_MAX - *counter ? UINT64_MAX : *counter + count;
}


static void __threadpool_signal_idle__(ThreadPool *pool) {
    if (
        pool->waiting_clients
        &&
        pool->queue_length == 0
        &&
        pool->n_working == 0
        &&
        pool->pending_submissions == 0
    ) __threadpool_condition_broadcast__(&pool->all_idle);
}


static void __threadpool_leave__(ThreadPool *pool) {
    pool->api_users--;
    if (pool->shutdown && pool->api_users == 0) __threadpool_condition_signal__(&pool->api_idle);
    __threadpool_unlock__(pool);
}


static void __threadpool_submit_leave__(ThreadPool *pool) {
    pool->pending_submissions--;
    __threadpool_signal_idle__(pool);
    __threadpool_leave__(pool);
}


static void __threadpool_broadcast_all__(ThreadPool *pool) {
    __threadpool_condition_broadcast__(&pool->notify);
    __threadpool_condition_broadcast__(&pool->not_full);
    __threadpool_condition_broadcast__(&pool->all_idle);
}


static int __threadpool_worker__(void *args) {
    ThreadPool *pool = args;
    threadpool_current = pool;

    __threadpool_lock__(pool);

    while (true) {
        while (pool->queue_length == 0 && !pool->shutdown) {
            pool->waiting_workers++;
            __threadpool_condition_wait__(pool, &pool->notify);
            pool->waiting_workers--;
        }

        if (pool->shutdown && pool->queue_length == 0) break;
        ThreadPoolTask task = pool->queue[pool->queue_head];

        if (++pool->queue_head == pool->queue_capacity) pool->queue_head = 0;
        pool->queue_length--;
        if (pool->waiting_submitters) {
            if (pool->waiting_batches) __threadpool_condition_broadcast__(&pool->not_full);
            else __threadpool_condition_signal__(&pool->not_full);
        }

        pool->n_working++;
        __threadpool_unlock__(pool);

        task.func(task.args);
        if (task.cleanup) task.cleanup(task.args);

        __threadpool_lock__(pool);
        pool->n_working--;
        __threadpool_count__(&pool->completed, 1);
        __threadpool_signal_idle__(pool);
        // Keep the mutex lock for the next dequeue to avoid a second acquisition.
    }

    __threadpool_unlock__(pool);
    threadpool_current = NULL;
    return 0;
}
