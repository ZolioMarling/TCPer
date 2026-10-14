#include "threadpool.h"


#include <limits.h>
#include <stdint.h>
#include <stdlib.h>


typedef struct ThreadTask {
    void (*func)(void *args);
    void *args;
    void (*cleanup)(void *args);
} ThreadTask;


struct ThreadPool {
    /* This lock remains valid until the final retained reference is released. */
    Mutex lifecycle_lock;
    ThreadCondition api_idle;
    unsigned int references;
    unsigned int api_users;
    int closing;
    int destroyed;
    int destroy_active;

    Mutex queue_lock;
    ThreadCondition notify;
    ThreadCondition not_full;
    ThreadCondition all_idle;
    unsigned int pending_submissions;
    int shutdown;
    int n_workers;
    int n_joined_workers;
    int n_working;
    Thread *threads;
    size_t queue_length;
    size_t queue_capacity;
    ThreadTask *queue;
    size_t queue_head;
    size_t queue_tail;
};


static int threadpool_worker(void *args);


/* Private synchronization failures leave lock ownership indeterminate. */
static _Noreturn void threadpool_sync_failure(void) {
    abort();
}


static void threadpool_lock(Mutex *mutex) {
    if (mutex_lock(mutex) != 0) threadpool_sync_failure();
}


static void threadpool_unlock(Mutex *mutex) {
    if (mutex_unlock(mutex) != 0) threadpool_sync_failure();
}


static void threadpool_condition_wait(ThreadCondition *condition, Mutex *mutex) {
    if (condition_wait(condition, mutex) != 0) threadpool_sync_failure();
}


static void threadpool_condition_signal(ThreadCondition *condition) {
    if (condition_signal(condition) != 0) threadpool_sync_failure();
}


static void threadpool_condition_broadcast(ThreadCondition *condition) {
    if (condition_broadcast(condition) != 0) threadpool_sync_failure();
}


static void threadpool_join(Thread *thread) {
    int worker_status;

    if (thread_join(thread, &worker_status) != 0 || worker_status != 0)
        threadpool_sync_failure();
}


static void threadpool_free(ThreadPool *pool) {
    if (!pool) return;
    free(pool->threads);
    free(pool->queue);
    free(pool);
}


/* lifecycle_lock must be held. */
static void threadpool_cancel_api_user_locked(ThreadPool *pool) {
    if (pool->api_users > 0) pool->api_users--;
    threadpool_condition_broadcast(&pool->api_idle);
}


/* Entering a wait keeps queue_lock held, establishing its submission boundary. */
static int threadpool_wait_enter(ThreadPool *pool) {
    if (!pool) return 1;
    threadpool_lock(&pool->lifecycle_lock);

    if (pool->closing || pool->destroyed || pool->api_users == UINT_MAX) {
        threadpool_unlock(&pool->lifecycle_lock);
        return 1;
    }
    pool->api_users++;

    threadpool_lock(&pool->queue_lock);
    threadpool_unlock(&pool->lifecycle_lock);
    return 0;
}


static void threadpool_leave(ThreadPool *pool) {
    if (!pool) return;
    threadpool_lock(&pool->lifecycle_lock);

    threadpool_cancel_api_user_locked(pool);
    threadpool_unlock(&pool->lifecycle_lock);
}


/* Entering a submission holds queue_lock, which makes pending_submissions visible to waiters. */
static int threadpool_submit_enter(ThreadPool *pool) {
    if (!pool) return 1;
    threadpool_lock(&pool->lifecycle_lock);

    if (pool->closing || pool->destroyed || pool->api_users == UINT_MAX) {
        threadpool_unlock(&pool->lifecycle_lock);
        return 1;
    }
    pool->api_users++;

    threadpool_lock(&pool->queue_lock);
    pool->pending_submissions++;
    threadpool_unlock(&pool->lifecycle_lock);
    return 0;
}


static void threadpool_submit_leave_locked(ThreadPool *pool) {
    if (pool->pending_submissions > 0) pool->pending_submissions--;
    if (pool->pending_submissions == 0) threadpool_condition_broadcast(&pool->all_idle);
    threadpool_unlock(&pool->queue_lock);
    threadpool_leave(pool);
}


static void threadpool_broadcast_all(ThreadPool *pool) {
    threadpool_condition_broadcast(&pool->notify);
    threadpool_condition_broadcast(&pool->not_full);
    threadpool_condition_broadcast(&pool->all_idle);
}


static int threadpool_destroy_enter(ThreadPool *pool) {
    if (!pool) return 1;
    threadpool_lock(&pool->lifecycle_lock);

    if (pool->destroy_active || pool->destroyed || pool->api_users == UINT_MAX) {
        threadpool_unlock(&pool->lifecycle_lock);
        return 1;
    }
    pool->closing = 1;
    pool->destroy_active = 1;
    pool->api_users++;
    threadpool_unlock(&pool->lifecycle_lock);
    return 0;
}


static int threadpool_is_worker(ThreadPool *pool) {
    int i;

    if (!pool) return 0;
#if defined(__OS_UNIX__)
    {
        pthread_t self = pthread_self();
        for (i = 0; i < pool->n_workers; i++) {
            if (pthread_equal(self, pool->threads[i])) return 1;
        }
    }
#elif defined(__OS_WINDOWS__)
    {
        DWORD self = GetCurrentThreadId();
        for (i = 0; i < pool->n_workers; i++) {
            if (GetThreadId(pool->threads[i]) == self) return 1;
        }
    }
#else
    (void)i;
#endif
    return 0;
}


int threadpool_retain(ThreadPool *pool) {
    if (!pool) return 1;
    threadpool_lock(&pool->lifecycle_lock);

    if (pool->closing || pool->destroyed || pool->references == 0 ||
        pool->references == UINT_MAX) {
        threadpool_unlock(&pool->lifecycle_lock);
        return 1;
    }
    pool->references++;
    threadpool_unlock(&pool->lifecycle_lock);
    return 0;
}


int threadpool_release(ThreadPool *pool) {
    int free_pool = 0;

    if (!pool) return 1;
    threadpool_lock(&pool->lifecycle_lock);

    if (pool->references == 0 || (pool->references == 1 && !pool->destroyed)) {
        threadpool_unlock(&pool->lifecycle_lock);
        return 1;
    }
    pool->references--;
    if (pool->references == 0) free_pool = 1;
    threadpool_unlock(&pool->lifecycle_lock);

    if (free_pool) {
        condition_destroy(&pool->api_idle);
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
    }
    return 0;
}


ThreadPool *threadpool_create(int n_workers, int queue_capacity) {
    int i;
    ThreadPool *pool;

    if (n_workers <= 0 || queue_capacity <= 0) return NULL;
    if ((size_t)queue_capacity > SIZE_MAX / sizeof(ThreadTask) ||
        (size_t)n_workers > SIZE_MAX / sizeof(Thread)) return NULL;

    pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->queue = calloc((size_t)queue_capacity, sizeof(*pool->queue));
    pool->threads = calloc((size_t)n_workers, sizeof(*pool->threads));
    if (!pool->queue || !pool->threads) {
        threadpool_free(pool);
        return NULL;
    }

    pool->references = 1;
    pool->n_workers = n_workers;
    pool->queue_capacity = (size_t)queue_capacity;

    if (mutex_create(&pool->lifecycle_lock, 1) != 0) {
        threadpool_free(pool);
        return NULL;
    }
    if (condition_init(&pool->api_idle) != 0) {
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
        return NULL;
    }
    if (mutex_create(&pool->queue_lock, 1) != 0) {
        condition_destroy(&pool->api_idle);
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
        return NULL;
    }
    if (condition_init(&pool->all_idle) != 0) {
        mutex_destroy(&pool->queue_lock);
        condition_destroy(&pool->api_idle);
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
        return NULL;
    }
    if (condition_init(&pool->notify) != 0) {
        condition_destroy(&pool->all_idle);
        mutex_destroy(&pool->queue_lock);
        condition_destroy(&pool->api_idle);
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
        return NULL;
    }
    if (condition_init(&pool->not_full) != 0) {
        condition_destroy(&pool->notify);
        condition_destroy(&pool->all_idle);
        mutex_destroy(&pool->queue_lock);
        condition_destroy(&pool->api_idle);
        mutex_destroy(&pool->lifecycle_lock);
        threadpool_free(pool);
        return NULL;
    }

    for (i = 0; i < n_workers; i++) {
        if (thread_create(&pool->threads[i], threadpool_worker, pool) != 0) {
            threadpool_lock(&pool->queue_lock);
            pool->shutdown = 1;
            pool->n_workers = i;
            threadpool_broadcast_all(pool);
            threadpool_unlock(&pool->queue_lock);

            while (i > 0) {
                i--;
                threadpool_join(&pool->threads[i]);
            }
            condition_destroy(&pool->not_full);
            condition_destroy(&pool->notify);
            condition_destroy(&pool->all_idle);
            mutex_destroy(&pool->queue_lock);
            condition_destroy(&pool->api_idle);
            mutex_destroy(&pool->lifecycle_lock);
            threadpool_free(pool);
            return NULL;
        }
    }
    return pool;
}


int threadpool_add(ThreadPool *pool, void (*func)(void *args), void *args,
                   int block, void (*cleanup)(void *args)) {
    int can_block;

    if (!func || threadpool_submit_enter(pool) != 0) return 1;

    can_block = block && !threadpool_is_worker(pool);
    while (pool->queue_length == pool->queue_capacity && !pool->shutdown) {
        if (!can_block) {
            threadpool_submit_leave_locked(pool);
            return 2;
        }
        threadpool_condition_wait(&pool->not_full, &pool->queue_lock);
    }
    if (pool->shutdown) {
        threadpool_submit_leave_locked(pool);
        return 1;
    }

    pool->queue[pool->queue_tail].func = func;
    pool->queue[pool->queue_tail].args = args;
    pool->queue[pool->queue_tail].cleanup = cleanup;
    pool->queue_tail++;
    if (pool->queue_tail == pool->queue_capacity) pool->queue_tail = 0;
    pool->queue_length++;

    threadpool_condition_signal(&pool->notify);
    threadpool_submit_leave_locked(pool);
    return 0;
}


int threadpool_wait(ThreadPool *pool) {
    int status;

    if (threadpool_is_worker(pool) || threadpool_wait_enter(pool) != 0) return 1;

    while (pool->queue_length > 0 || pool->n_working > 0 ||
           pool->pending_submissions > 0) {
        if (pool->shutdown) {
            threadpool_unlock(&pool->queue_lock);
            threadpool_leave(pool);
            return 1;
        }
        threadpool_condition_wait(&pool->all_idle, &pool->queue_lock);
    }
    status = pool->shutdown ? 1 : 0;
    threadpool_unlock(&pool->queue_lock);
    threadpool_leave(pool);
    return status;
}


int threadpool_destroy_ex(ThreadPool *pool, int safe_exit, size_t *discarded_tasks) {
    size_t discarded_count = 0;
    size_t discarded_head = 0;
    size_t i;

    if (discarded_tasks) *discarded_tasks = 0;
    if (!pool || threadpool_is_worker(pool) || threadpool_destroy_enter(pool) != 0) return 1;

    threadpool_lock(&pool->queue_lock);
    pool->shutdown = 1;
    if (!safe_exit && pool->queue_length > 0) {
        discarded_head = pool->queue_head;
        discarded_count = pool->queue_length;
        pool->queue_length = 0;
        pool->queue_head = 0;
        pool->queue_tail = 0;
    }
    if (discarded_tasks) *discarded_tasks = discarded_count;
    threadpool_broadcast_all(pool);
    threadpool_unlock(&pool->queue_lock);

    /* Cleanup is allowed to be slow or reentrant, so it never runs under queue_lock. */
    for (i = 0; i < discarded_count; i++) {
        ThreadTask *task = &pool->queue[discarded_head];

        if (task->cleanup) task->cleanup(task->args);
        discarded_head++;
        if (discarded_head == pool->queue_capacity) discarded_head = 0;
    }

    threadpool_lock(&pool->lifecycle_lock);
    while (pool->api_users != 1) {
        threadpool_condition_wait(&pool->api_idle, &pool->lifecycle_lock);
    }
    threadpool_unlock(&pool->lifecycle_lock);

    while (pool->n_joined_workers < pool->n_workers) {
        threadpool_join(&pool->threads[pool->n_joined_workers]);
        pool->n_joined_workers++;
    }

    threadpool_lock(&pool->lifecycle_lock);
    pool->destroyed = 1;
    pool->destroy_active = 0;
    pool->api_users--;
    threadpool_condition_broadcast(&pool->api_idle);
    threadpool_unlock(&pool->lifecycle_lock);

    condition_destroy(&pool->not_full);
    condition_destroy(&pool->notify);
    condition_destroy(&pool->all_idle);
    mutex_destroy(&pool->queue_lock);
    return threadpool_release(pool);
}


int threadpool_destroy(ThreadPool *pool, int safe_exit) {
    return threadpool_destroy_ex(pool, safe_exit, NULL);
}


static int threadpool_worker(void *args) {
    ThreadPool *pool = (ThreadPool *)args;

    while (1) {
        ThreadTask task;

        threadpool_lock(&pool->queue_lock);
        while (pool->queue_length == 0 && !pool->shutdown) {
            threadpool_condition_wait(&pool->notify, &pool->queue_lock);
        }
        if (pool->shutdown && pool->queue_length == 0) {
            threadpool_unlock(&pool->queue_lock);
            return 0;
        }

        task = pool->queue[pool->queue_head];
        pool->queue_head++;
        if (pool->queue_head == pool->queue_capacity) pool->queue_head = 0;
        pool->queue_length--;
        threadpool_condition_signal(&pool->not_full);
        pool->n_working++;
        threadpool_unlock(&pool->queue_lock);

        task.func(task.args);
        if (task.cleanup) task.cleanup(task.args);

        threadpool_lock(&pool->queue_lock);
        pool->n_working--;
        if (pool->n_working == 0 && pool->queue_length == 0)
            threadpool_condition_broadcast(&pool->all_idle);
        threadpool_unlock(&pool->queue_lock);
    }
}
