#include "async_log.h"
#include "async_log_internal.h"
#include "threadpool.h"

#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

const char *const ASYNC_LOG_TIPS[LOG_LEVEL_COUNT] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
const char *const ASYNC_LOG_COLOURS[LOG_LEVEL_COUNT] = {"\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"};
#define ASYNC_LOG_TRUNCATION_SUFFIX " ...[truncated]"

typedef enum {
    ASYNC_LOG_STOPPED,
    ASYNC_LOG_STARTING,
    ASYNC_LOG_RUNNING,
    ASYNC_LOG_STOPPING
} AsyncLogState;

typedef struct {
    unsigned int references;
    int mode;
    _LogLockFunc lock;
    void *lock_ctx;
    _LogCallback file_callback;
    int file_callback_index;
    int last_callback_index;
    _LogCallback callback[ASYNC_LOG_MAX_CALLBACKS];
} AsyncLogConfig;

typedef struct AsyncLogTask {
    int line;
    int level;
    int time_valid;
    struct tm time;
    char file[ASYNC_LOG_MAX_FILE_LENGTH];
    char message[ASYNC_LOG_MAX_MESSAGE_LENGTH];
    struct AsyncLogTask *next;
} AsyncLogTask;

/* This is the allocation-free configuration retained between logger runs. */
AsyncLogConfig async_log_default_config = {
    .references = 1,
    .file_callback_index = ASYNC_LOG_MAX_CALLBACKS,
    .last_callback_index = -1
};

struct {
    Mutex mutex;
    ThreadCondition changed;
    AsyncLogState state;
    ThreadPool *pool;
    unsigned int submitters;
    unsigned int synchronous_dispatchers;
    AsyncLogConfig *config;
    AsyncLogTask *task_pool;
    AsyncLogTask *free_tasks;
    int last_exit_status;
    AsyncLogOverflowPolicy overflow_policy;
    AsyncLogFlushPolicy flush_policy;
    int min_level;
    AsyncLogStats stats;
} async_log;

#if defined(__OS_UNIX__)
    /* Unlike pthread_once(), this lock permits retrying transient setup failures. */
    Mutex async_log_init_mutex = PTHREAD_MUTEX_INITIALIZER;
    int async_log_sync_status = 1;
#elif defined(__OS_WINDOWS__)
    INIT_ONCE async_log_once = INIT_ONCE_STATIC_INIT;
#endif
AsyncLogThreadStorage async_log_thread_state_storage;

void __async_log_init_event__(LogEvent *event, void *ctx);
void __async_log_worker__(void *args);

#if defined(__OS_UNIX__)
    int __async_log_initialize__(void) {
        if (mutex_create(&async_log.mutex, 1) != 0) return 1;
        if (condition_init(&async_log.changed) != 0) {
            mutex_destroy(&async_log.mutex);
            return 1;
        }
        if (pthread_key_create(&async_log_thread_state_storage.key, free) != 0) {
            condition_destroy(&async_log.changed);
            mutex_destroy(&async_log.mutex);
            return 1;
        }
        async_log.config = &async_log_default_config;
        return 0;
    }

    int __async_log_sync_ready__(void) {
        int status;

        if (mutex_lock(&async_log_init_mutex) != 0) return 1;
        if (async_log_sync_status != 0) {
            status = __async_log_initialize__();
            if (status == 0) async_log_sync_status = 0;
        } else status = 0;
        if (mutex_unlock(&async_log_init_mutex) != 0) abort();
        return status;
    }
#elif defined(__OS_WINDOWS__)
    VOID WINAPI __async_log_thread_state_destroy__(PVOID value) {
        free(value);
    }

    BOOL CALLBACK __async_log_once_initialize__(PINIT_ONCE once, void *parameter, void **context) {
        (void)once;
        (void)parameter;
        (void)context;

        if (mutex_create(&async_log.mutex, 1) != 0) return FALSE;
        if (condition_init(&async_log.changed) != 0) {
            mutex_destroy(&async_log.mutex);
            return FALSE;
        }
        async_log_thread_state_storage.key = FlsAlloc(__async_log_thread_state_destroy__);
        if (async_log_thread_state_storage.key == FLS_OUT_OF_INDEXES) {
            condition_destroy(&async_log.changed);
            mutex_destroy(&async_log.mutex);
            return FALSE;
        }
        async_log.config = &async_log_default_config;
        return TRUE;
    }

    int __async_log_sync_ready__(void) {
        return InitOnceExecuteOnce(&async_log_once, __async_log_once_initialize__, NULL, NULL) ? 0 : 1;
    }
#endif

AsyncLogThreadState *__async_log_thread_state__(void) {
    AsyncLogThreadState *thread_state;

    if (__async_log_sync_ready__() != 0) return NULL;
#if defined(__OS_UNIX__)
    thread_state = pthread_getspecific(async_log_thread_state_storage.key);
    if (thread_state) return thread_state;
    thread_state = calloc(1, sizeof(*thread_state));
    if (!thread_state || pthread_setspecific(async_log_thread_state_storage.key, thread_state) != 0) {
        free(thread_state);
        return NULL;
    }
#elif defined(__OS_WINDOWS__)
    thread_state = FlsGetValue(async_log_thread_state_storage.key);
    if (thread_state) return thread_state;
    thread_state = calloc(1, sizeof(*thread_state));
    if (!thread_state || !FlsSetValue(async_log_thread_state_storage.key, thread_state)) {
        free(thread_state);
        return NULL;
    }
#endif
    return thread_state;
}

void __async_log_lock_state__(void) {
    if (mutex_lock(&async_log.mutex) != 0) abort();
}

void __async_log_unlock_state__(void) {
    if (mutex_unlock(&async_log.mutex) != 0) abort();
}

void __async_log_wait_state__(void) {
    if (condition_wait(&async_log.changed, &async_log.mutex) != 0) abort();
}

void __async_log_notify_state__(void) {
    if (condition_broadcast(&async_log.changed) != 0) abort();
}

void __async_log_record_stat__(uint64_t *stat) {
    if (__async_log_sync_ready__() != 0) return;
    __async_log_lock_state__();
    if (*stat != UINT64_MAX) (*stat)++;
    __async_log_unlock_state__();
}

void __async_log_increment_stat_locked__(uint64_t *stat) {
    if (*stat != UINT64_MAX) (*stat)++;
}

void __async_log_record_output_failure__(void) {
    __async_log_record_stat__(&async_log.stats.write_failures);
}

int __async_log_should_flush__(int level) {
    AsyncLogFlushPolicy policy;

    if (__async_log_sync_ready__() != 0) return level >= LOG_ERROR;
    __async_log_lock_state__();
    policy = async_log.flush_policy;
    __async_log_unlock_state__();

    return policy == ASYNC_LOG_FLUSH_ALWAYS ||
        (policy == ASYNC_LOG_FLUSH_ON_ERROR && level >= LOG_ERROR);
}

void __async_log_config_copy_locked__(AsyncLogConfig *config) {
    if (!config || !async_log.config) abort();
    *config = *async_log.config;
    config->references = 1; /* The global configuration pointer owns this reference. */
}

void __async_log_config_release_locked__(AsyncLogConfig *config) {
    if (!config || config->references == 0) abort();
    config->references--;
    if (config->references == 0 && config != &async_log_default_config) free(config);
}

AsyncLogConfig *__async_log_config_acquire_locked__(void) {
    AsyncLogConfig *config = async_log.config;

    if (!config || config->references == 0 || config->references == UINT_MAX) abort();
    config->references++;
    return config;
}

void __async_log_replace_config_locked__(AsyncLogConfig *config) {
    AsyncLogConfig *previous = async_log.config;

    if (!config || !previous) abort();
    async_log.config = config;
    __async_log_config_release_locked__(previous);
}

int __async_log_try_use_exclusive_config_locked__(AsyncLogConfig *config) {
    if (!config) return 1;
    if (async_log.config->references != 1) return ASYNC_LOG_CONFIG_BUSY;
    __async_log_config_copy_locked__(config);
    return 0;
}

int __async_log_begin_config_update__(AsyncLogConfig **config, int exclusive) {
    int status = 0;

    if (!config) return 1;
    *config = calloc(1, sizeof(**config));
    if (!*config) return 1;
    __async_log_lock_state__();
    if (async_log.state == ASYNC_LOG_STOPPING)
        status = ASYNC_LOG_CONFIG_BUSY;
    else if (exclusive)
        status = __async_log_try_use_exclusive_config_locked__(*config);
    else __async_log_config_copy_locked__(*config);
    if (status != 0) {
        __async_log_unlock_state__();
        free(*config);
        *config = NULL;
    }
    return status;
}

void __async_log_commit_config_update__(AsyncLogConfig *config) {
    __async_log_replace_config_locked__(config);
    __async_log_unlock_state__();
}

void __async_log_reset_config_locked__(void) {
    AsyncLogConfig *config = async_log.config;

    /* Exit has already joined workers and waited for synchronous dispatchers. */
    if (!config || config->references != 1) abort();
    if (config != &async_log_default_config) {
        if (async_log_default_config.references != 0) abort();
        async_log_default_config.references = 1;
        async_log_default_config.mode = config->mode;
        async_log.config = &async_log_default_config;
        free(config);
        config = &async_log_default_config;
    }
    memset(config->callback, 0, sizeof(config->callback));
    config->last_callback_index = -1;
    config->file_callback = (_LogCallback) {NULL, NULL};
    config->file_callback_index = ASYNC_LOG_MAX_CALLBACKS;
    config->lock = NULL;
    config->lock_ctx = NULL;
}

void __async_log_finish_dispatch__(AsyncLogConfig *config) {
    if (!config) return;
    __async_log_lock_state__();
    __async_log_config_release_locked__(config);
    __async_log_notify_state__();
    __async_log_unlock_state__();
}

int __async_log_in_dispatch__(const AsyncLogThreadState *thread_state) {
    return thread_state &&
        (thread_state->callback_depth != 0 || thread_state->worker_depth != 0);
}

void __async_log_release_task_locked__(AsyncLogTask *task) {
    if (!task) return;
    task->next = async_log.free_tasks;
    async_log.free_tasks = task;
    __async_log_notify_state__();
}

int __async_log_create_task_pool_locked__(int n_workers, int queue_capacity) {
    usize task_capacity;
    usize i;

    if ((usize)n_workers > SIZE_MAX - (usize)queue_capacity) return 1;
    task_capacity = (usize)n_workers + (usize)queue_capacity;
    if (task_capacity == 0 || task_capacity > SIZE_MAX / sizeof(*async_log.task_pool)) return 1;
    async_log.task_pool = calloc(task_capacity, sizeof(*async_log.task_pool));
    if (!async_log.task_pool) return 1;
    async_log.free_tasks = NULL;
    for (i = 0; i < task_capacity; i++) {
        async_log.task_pool[i].next = async_log.free_tasks;
        async_log.free_tasks = &async_log.task_pool[i];
    }
    return 0;
}

void __async_log_destroy_task_pool_locked__(void) {
    free(async_log.task_pool);
    async_log.task_pool = NULL;
    async_log.free_tasks = NULL;
}

AsyncLogTask *__async_log_acquire_task_locked__(void) {
    AsyncLogTask *task = async_log.free_tasks;

    if (!task) return NULL;
    async_log.free_tasks = task->next;
    task->next = NULL;
    return task;
}

int __async_log_localtime__(time_t now, struct tm *result) {
    if (!result) return 1;
    memset(result, 0, sizeof(*result));
#if defined(__OS_UNIX__)
    return localtime_r(&now, result) ? 0 : 1;
#elif defined(__OS_WINDOWS__)
    return localtime_s(result, &now) == 0 ? 0 : 1;
#endif
}

void __async_log_finish_synchronous_dispatch__(void) {
    __async_log_lock_state__();
    if (async_log.synchronous_dispatchers == 0) abort();
    async_log.synchronous_dispatchers--;
    __async_log_notify_state__();
    __async_log_unlock_state__();
}

/* The submitter count keeps the pool valid until threadpool_add() returns. */
void __async_log_finish_submission_locked__(void) {
    if (async_log.submitters == 0) abort();
    async_log.submitters--;
    __async_log_notify_state__();
}

/* Returns nonzero when a dispatch cannot be tracked without overflowing. */
int __async_log_begin_synchronous_dispatch_locked__(AsyncLogConfig **config) {
    if (async_log.synchronous_dispatchers == UINT_MAX) return 1;
    *config = __async_log_config_acquire_locked__();
    async_log.synchronous_dispatchers++;
    __async_log_increment_stat_locked__(&async_log.stats.synchronous_dispatches);
    return 0;
}

/* Select the queue-overflow behavior while the state lock is held. */
void __async_log_reject_submission_locked__(AsyncLogConfig **config, int *drop,
                                             int *safe_fallback, int *fallback_mode) {
    __async_log_increment_stat_locked__(&async_log.stats.rejected_submissions);
    if (async_log.overflow_policy == ASYNC_LOG_OVERFLOW_DROP) {
        __async_log_increment_stat_locked__(&async_log.stats.dropped_messages);
        *drop = 1;
    } else if (__async_log_begin_synchronous_dispatch_locked__(config) == 0) {
        __async_log_increment_stat_locked__(&async_log.stats.synchronous_overflows);
    } else {
        *safe_fallback = 1;
        *fallback_mode = async_log.config->mode;
    }
}

int __async_log_enter_callback_region__(int *old_cancel_state) {
    if (old_cancel_state) *old_cancel_state = 0;
#if defined(__OS_UNIX__)
    return pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, old_cancel_state);
#else
    return 0;
#endif
}

void __async_log_leave_callback_region__(int old_cancel_state) {
#if defined(__OS_UNIX__)
    if (pthread_setcancelstate(old_cancel_state, NULL) != 0) abort();
#else
    (void)old_cancel_state;
#endif
}

void __async_log_run_callback__(LogEvent *event, const _LogCallback *callback) {
    if (!callback->func) return;
    __async_log_init_event__(event, callback->ctx);
    callback->func(event);
}

void __async_log_write_stdout__(LogEvent *event) {
    __async_log_init_event__(event, stdout);
    __async_log_callback_stdout__(event);
}

void __async_log_dispatch__(LogEvent *event, const AsyncLogConfig *config,
                            AsyncLogThreadState *thread_state) {
    int old_cancel_state = 0;
    int cancel_disabled;
    int lock_held = 0;
    int last_callback_index;
    int n;

    if (!thread_state || !config) {
        __async_log_write_stdout__(event);
        return;
    }

    /*
     * Invariants for a dispatch snapshot:
     * - callback_depth is nonzero while user code and lock hooks run;
     * - config is released only after callback_depth and the optional hook lock
     *   are restored by the caller;
     * - POSIX cancellation is deferred across this region so a cancellation
     *   request cannot skip those restorations. Non-local exits such as
     *   longjmp remain outside the portable C contract and are forbidden by
     *   the public API.
     */
    cancel_disabled = __async_log_enter_callback_region__(&old_cancel_state) == 0;
    if (thread_state->callback_depth == UINT_MAX) abort();
    thread_state->callback_depth++;
    if (thread_state->callback_depth == 1 && config->lock) {
        config->lock(1, config->lock_ctx);
        lock_held = 1;
    }

    if (!config->mode) {
        __async_log_write_stdout__(event);
    }
    /* User hooks must return normally; C has no portable non-local-exit guard here. */
    if (config->last_callback_index >= 0 || config->file_callback.func) {
        last_callback_index = config->last_callback_index;
        if (config->file_callback.func && config->file_callback_index < ASYNC_LOG_MAX_CALLBACKS &&
            config->file_callback_index > last_callback_index)
            last_callback_index = config->file_callback_index;
        for (n = 0; n <= last_callback_index; n++) {
            if (n == config->file_callback_index)
                __async_log_run_callback__(event, &config->file_callback);
            __async_log_run_callback__(event, &config->callback[n]);
        }
    }
    if (config->file_callback.func && config->file_callback_index == ASYNC_LOG_MAX_CALLBACKS)
        __async_log_run_callback__(event, &config->file_callback);

    if (lock_held) config->lock(0, config->lock_ctx);
    thread_state->callback_depth--;
    if (cancel_disabled) __async_log_leave_callback_region__(old_cancel_state);
}

void __async_log_dispatch_synchronously__(LogEvent *event, AsyncLogConfig *config,
                                          AsyncLogThreadState *thread_state) {
    int old_cancel_state = 0;
    int cancel_disabled;

    cancel_disabled = __async_log_enter_callback_region__(&old_cancel_state) == 0;
    __async_log_dispatch__(event, config, thread_state);
    __async_log_finish_dispatch__(config);
    __async_log_finish_synchronous_dispatch__();
    if (cancel_disabled) __async_log_leave_callback_region__(old_cancel_state);
}

int __async_log_first_open_callback_index__(const AsyncLogConfig *config) {
    int n;

    for (n = 0; n < ASYNC_LOG_MAX_CALLBACKS; n++) {
        if (!config->callback[n].func) return n;
    }
    return ASYNC_LOG_MAX_CALLBACKS;
}

void __async_log_write_while_stopping__(LogEvent *event, int mode) {
    /* The configured callback contexts may be released as soon as exit returns. */
    if (mode) return;
    __async_log_record_stat__(&async_log.stats.stdout_fallbacks);
    __async_log_write_stdout__(event);
}

void __async_log_format_message__(char *message, usize capacity,
                                  const char *fmt, va_list args) {
    int message_length;

    if (!message || capacity == 0) return;
    message_length = fmt ? vsnprintf(message, capacity, fmt, args) : -1;
    if (message_length < 0)
        snprintf(message, capacity, "(invalid log message)");
    else if ((usize)message_length >= capacity && capacity >= sizeof(ASYNC_LOG_TRUNCATION_SUFFIX))
        memcpy(message + capacity - sizeof(ASYNC_LOG_TRUNCATION_SUFFIX),
               ASYNC_LOG_TRUNCATION_SUFFIX, sizeof(ASYNC_LOG_TRUNCATION_SUFFIX));
    message[capacity - 1] = '\0';
}

void __async_log_prepare_event__(LogEvent *event, char *message, const char *file,
                                 int line, int level) {
    time_t now;

    memset(event, 0, sizeof(*event));
    event->message = message;
    event->async_message = message;
    event->file = file ? file : "<unknown>";
    event->line = line;
    event->level = level;
    now = time(NULL);
    if (__async_log_localtime__(now, &event->time_storage) == 0)
        event->time = &event->time_storage;
}

typedef struct {
    AsyncLogConfig *config;
    AsyncLogTask *task;
    ThreadPool *pool;
    int safe_fallback;
    int fallback_mode;
    int drop;
} AsyncLogSubmission;

void __async_log_select_submission_locked__(AsyncLogSubmission *submission) {
    memset(submission, 0, sizeof(*submission));
    if (async_log.state == ASYNC_LOG_RUNNING && async_log.submitters != UINT_MAX) {
        submission->task = __async_log_acquire_task_locked__();
        if (submission->task) {
            submission->pool = async_log.pool;
            /* Keep the pool and task pool alive until threadpool_add() returns. */
            async_log.submitters++;
            return;
        }
    }
    if (async_log.state == ASYNC_LOG_RUNNING) {
        __async_log_reject_submission_locked__(&submission->config, &submission->drop,
                                           &submission->safe_fallback,
                                           &submission->fallback_mode);
    } else if (async_log.state == ASYNC_LOG_STOPPING) {
        submission->safe_fallback = 1;
        submission->fallback_mode = async_log.config->mode;
        __async_log_increment_stat_locked__(&async_log.stats.stopping_fallbacks);
    } else {
        if (__async_log_begin_synchronous_dispatch_locked__(&submission->config) != 0) {
            submission->safe_fallback = 1;
            submission->fallback_mode = async_log.config->mode;
        }
    }
}

void __async_log_fill_task__(AsyncLogTask *task, const LogEvent *event) {
    task->level = event->level;
    snprintf(task->file, sizeof(task->file), "%s", event->file);
    task->line = event->line;
    task->time = event->time_storage;
    task->time_valid = event->time != NULL;
}

int async_log_init(int n_workers, int queue_capacity) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    ThreadPool *pool;

    /* A worker stopped by exit cannot wait for the shutdown that joins it. */
    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (n_workers <= 0 || queue_capacity <= 0 || __async_log_sync_ready__() != 0) return 1;

    __async_log_lock_state__();
    while (async_log.state == ASYNC_LOG_STARTING || async_log.state == ASYNC_LOG_STOPPING)
        __async_log_wait_state__();
    if (async_log.state == ASYNC_LOG_RUNNING) {
        __async_log_unlock_state__();
        return 0;
    }
    async_log.state = ASYNC_LOG_STARTING;
    __async_log_unlock_state__();

    __async_log_lock_state__();
    if (__async_log_create_task_pool_locked__(n_workers, queue_capacity) != 0) {
        async_log.state = ASYNC_LOG_STOPPED;
        __async_log_notify_state__();
        __async_log_unlock_state__();
        return 1;
    }
    __async_log_unlock_state__();

    pool = threadpool_create(n_workers, queue_capacity);

    __async_log_lock_state__();
    if (pool) {
        async_log.pool = pool;
        async_log.state = ASYNC_LOG_RUNNING;
    } else {
        __async_log_destroy_task_pool_locked__();
        async_log.state = ASYNC_LOG_STOPPED;
    }
    __async_log_notify_state__();
    __async_log_unlock_state__();
    return pool ? 0 : 1;
}

int async_log_exit(int safe_exit) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    ThreadPool *pool;
    int destroy_status;
    size_t discarded_tasks = 0;

    /* A callback cannot wait for itself, and a worker cannot join its own pool. */
    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (__async_log_sync_ready__() != 0) return 1;

    __async_log_lock_state__();
    while (async_log.state == ASYNC_LOG_STARTING)
        __async_log_wait_state__();
    if (async_log.state == ASYNC_LOG_STOPPING) {
        while (async_log.state == ASYNC_LOG_STOPPING)
            __async_log_wait_state__();
        destroy_status = async_log.last_exit_status;
        __async_log_unlock_state__();
        return destroy_status;
    }
    if (async_log.state == ASYNC_LOG_STOPPED) {
        /* Stop new synchronous dispatches before waiting for the active ones. */
        async_log.state = ASYNC_LOG_STOPPING;
        async_log.last_exit_status = 1;
        __async_log_notify_state__();
        while (async_log.synchronous_dispatchers != 0)
            __async_log_wait_state__();
        __async_log_reset_config_locked__();
        async_log.state = ASYNC_LOG_STOPPED;
        async_log.last_exit_status = 0;
        __async_log_notify_state__();
        __async_log_unlock_state__();
        return 0;
    }

    pool = async_log.pool;
    async_log.pool = NULL;
    async_log.state = ASYNC_LOG_STOPPING;
    async_log.last_exit_status = 1;
    __async_log_notify_state__();
    while (async_log.submitters != 0 || async_log.synchronous_dispatchers != 0)
        __async_log_wait_state__();
    __async_log_unlock_state__();

    destroy_status = threadpool_destroy_ex(pool, safe_exit != 0, &discarded_tasks);

    __async_log_lock_state__();
    if (destroy_status == 0) {
        if (UINT64_MAX - async_log.stats.shutdown_discarded_messages < discarded_tasks)
            async_log.stats.shutdown_discarded_messages = UINT64_MAX;
        else async_log.stats.shutdown_discarded_messages += (uint64_t)discarded_tasks;
        async_log.state = ASYNC_LOG_STOPPED;
        __async_log_destroy_task_pool_locked__();
        __async_log_reset_config_locked__();
        async_log.last_exit_status = 0;
    } else {
        /* This should be unreachable after the worker/callback guard above. */
        async_log.pool = pool;
        async_log.state = ASYNC_LOG_RUNNING;
        async_log.last_exit_status = 1;
    }
    __async_log_notify_state__();
    __async_log_unlock_state__();
    return destroy_status == 0 ? 0 : 1;
}

int async_log_setting(int mode) {
    AsyncLogConfig *config;
    AsyncLogThreadState *thread_state = __async_log_thread_state__();

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (__async_log_sync_ready__() != 0) return 1;
    if (__async_log_begin_config_update__(&config, 0) != 0) return 1;
    config->mode = mode != 0;
    __async_log_commit_config_update__(config);
    return 0;
}

int async_log_config_write(FILE *f) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    AsyncLogConfig *config;
    int status;

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (__async_log_sync_ready__() != 0) return 1;
    status = __async_log_begin_config_update__(&config, 1);
    if (status != 0) return status;
    if (f && !config->file_callback.func)
        config->file_callback_index = __async_log_first_open_callback_index__(config);
    else if (!f)
        config->file_callback_index = ASYNC_LOG_MAX_CALLBACKS;
    config->file_callback = f ?
        (_LogCallback) {__async_log_callback_write__, f} :
        (_LogCallback) {NULL, NULL};
    __async_log_commit_config_update__(config);
    return 0;
}

int async_log_config_thread_lock(_LogLockFunc func, void *ctx) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    AsyncLogConfig *config;

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (__async_log_sync_ready__() != 0) return 1;
    {
        int status = __async_log_begin_config_update__(&config, 1);
        if (status != 0) return status;
    }
    config->lock = func;
    config->lock_ctx = func ? ctx : NULL;
    __async_log_commit_config_update__(config);
    return 0;
}

int async_log_add_callback(_LogCallbackFunc func, void *ctx) {
    AsyncLogConfig *config;
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    int n;

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (!func || __async_log_sync_ready__() != 0) return 1;
    if (__async_log_begin_config_update__(&config, 0) != 0) return 1;
    for (n = 0; n < ASYNC_LOG_MAX_CALLBACKS; n++) {
        if (!config->callback[n].func) {
            config->callback[n] = (_LogCallback) {func, ctx};
            if (n > config->last_callback_index) config->last_callback_index = n;
            break;
        }
    }
    if (n == ASYNC_LOG_MAX_CALLBACKS) free(config);
    else __async_log_replace_config_locked__(config);
    __async_log_unlock_state__();
    return n == ASYNC_LOG_MAX_CALLBACKS;
}

int async_log_remove_callback(_LogCallbackFunc func, void *ctx) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    AsyncLogConfig *config;
    int n;
    int count = 0;
    int status;

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (!func) return 0;
    if (__async_log_sync_ready__() != 0) return -1;
    status = __async_log_begin_config_update__(&config, 1);
    if (status != 0) {
        return status == ASYNC_LOG_CONFIG_BUSY ? -ASYNC_LOG_CONFIG_BUSY : -1;
    }
    for (n = 0; n < ASYNC_LOG_MAX_CALLBACKS; n++) {
        if (config->callback[n].func == func && config->callback[n].ctx == ctx) {
            config->callback[n].func = NULL;
            config->callback[n].ctx = NULL;
            count++;
        }
    }
    if (config->file_callback.func == func && config->file_callback.ctx == ctx) {
        config->file_callback = (_LogCallback) {NULL, NULL};
        config->file_callback_index = ASYNC_LOG_MAX_CALLBACKS;
        count++;
    }
    if (count) {
        for (n = ASYNC_LOG_MAX_CALLBACKS - 1; n >= 0; n--) {
            if (config->callback[n].func) break;
        }
        config->last_callback_index = n;
        __async_log_replace_config_locked__(config);
    } else free(config);
    __async_log_unlock_state__();
    return count;
}

int async_log_clear_callbacks(void) {
    AsyncLogThreadState *thread_state = __async_log_thread_state__();
    AsyncLogConfig *config;

    if (__async_log_in_dispatch__(thread_state)) return -1;
    if (__async_log_sync_ready__() != 0) return 1;
    {
        int status = __async_log_begin_config_update__(&config, 1);
        if (status != 0) return status;
    }
    memset(config->callback, 0, sizeof(config->callback));
    config->last_callback_index = -1;
    __async_log_commit_config_update__(config);
    return 0;
}

void async_log_set_overflow_policy(AsyncLogOverflowPolicy policy) {
    if (__async_log_sync_ready__() != 0) return;
    __async_log_lock_state__();
    async_log.overflow_policy = policy == ASYNC_LOG_OVERFLOW_DROP ?
        ASYNC_LOG_OVERFLOW_DROP : ASYNC_LOG_OVERFLOW_SYNCHRONOUS;
    __async_log_unlock_state__();
}

void async_log_set_flush_policy(AsyncLogFlushPolicy policy) {
    if (policy != ASYNC_LOG_FLUSH_ALWAYS && policy != ASYNC_LOG_FLUSH_NEVER)
        policy = ASYNC_LOG_FLUSH_ON_ERROR;
    if (__async_log_sync_ready__() != 0) return;
    __async_log_lock_state__();
    async_log.flush_policy = policy;
    __async_log_unlock_state__();
}

void async_log_set_min_level(int level) {
    if (level < LOG_TRACE || level >= LOG_LEVEL_COUNT) level = LOG_TRACE;
    if (__async_log_sync_ready__() != 0) return;
    __async_log_lock_state__();
    async_log.min_level = level;
    __async_log_unlock_state__();
}

int async_log_level_enabled(int level) {
    int min_level;

    if (level < LOG_TRACE || level >= LOG_LEVEL_COUNT) level = LOG_ERROR;
    if (__async_log_sync_ready__() != 0) return 1;
    __async_log_lock_state__();
    min_level = async_log.min_level;
    __async_log_unlock_state__();
    return level >= min_level;
}

void async_log_get_stats(AsyncLogStats *stats) {
    if (!stats) return;
    if (__async_log_sync_ready__() != 0) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    __async_log_lock_state__();
    *stats = async_log.stats;
    __async_log_unlock_state__();
}

void async_log_reset_stats(void) {
    if (__async_log_sync_ready__() != 0) return;
    __async_log_lock_state__();
    memset(&async_log.stats, 0, sizeof(async_log.stats));
    __async_log_unlock_state__();
}

void __async_log_print__(int level, const char *file, int line, const char *fmt, ...) {
    AsyncLogThreadState *thread_state;
    AsyncLogSubmission submission;
    LogEvent event;
    char stack_message[ASYNC_LOG_MAX_MESSAGE_LENGTH];
    char *message = stack_message;
    va_list args;

    level = (level >= LOG_TRACE && level < LOG_LEVEL_COUNT) ? level : LOG_ERROR;
    if (!async_log_level_enabled(level)) return;
    thread_state = __async_log_thread_state__();

    /* Do not feed logger callbacks or lock hooks back into callback dispatch. */
    if (thread_state && thread_state->callback_depth != 0) {
        __async_log_record_stat__(&async_log.stats.recursive_dropped_messages);
        return;
    }

    /* Logging must stay available as a best-effort diagnostic during init failure. */
    if (__async_log_sync_ready__() != 0) {
        va_start(args, fmt);
        __async_log_format_message__(message, sizeof(stack_message), fmt, args);
        va_end(args);
        __async_log_prepare_event__(&event, message, file, line, level);
        __async_log_write_stdout__(&event);
        return;
    }

    __async_log_lock_state__();
    __async_log_select_submission_locked__(&submission);
    __async_log_unlock_state__();

    if (submission.drop) return;

    if (submission.task) message = submission.task->message;
    va_start(args, fmt);
    __async_log_format_message__(message, ASYNC_LOG_MAX_MESSAGE_LENGTH, fmt, args);
    va_end(args);
    __async_log_prepare_event__(&event, message, file, line, level);

    if (submission.safe_fallback) {
        __async_log_write_while_stopping__(&event, submission.fallback_mode);
        return;
    }

    if (!submission.pool) {
        __async_log_dispatch_synchronously__(&event, submission.config, thread_state);
        return;
    }

    __async_log_fill_task__(submission.task, &event);

    if (threadpool_add(submission.pool, __async_log_worker__, submission.task, 0, NULL) == 0) {
        __async_log_lock_state__();
        __async_log_finish_submission_locked__();
        __async_log_unlock_state__();
        return;
    }

    memcpy(stack_message, submission.task->message, sizeof(stack_message));
    event.message = stack_message;
    event.async_message = stack_message;

    __async_log_lock_state__();
    __async_log_release_task_locked__(submission.task);
    __async_log_finish_submission_locked__();
    submission.config = NULL;
    submission.drop = 0;
    submission.safe_fallback = 0;
    submission.fallback_mode = 0;
    __async_log_reject_submission_locked__(&submission.config, &submission.drop,
                                            &submission.safe_fallback,
                                            &submission.fallback_mode);
    __async_log_unlock_state__();
    if (submission.drop) return;
    if (submission.safe_fallback) {
        __async_log_write_while_stopping__(&event, submission.fallback_mode);
        return;
    }
    __async_log_dispatch_synchronously__(&event, submission.config, thread_state);
}

void __async_log_worker__(void *args) {
    AsyncLogTask *task = (AsyncLogTask *)args;
    AsyncLogThreadState *thread_state;
    AsyncLogConfig *config;
    LogEvent event;
    int old_cancel_state = 0;
    int cancel_disabled;

    if (!task) return;
    cancel_disabled = __async_log_enter_callback_region__(&old_cancel_state) == 0;
    memset(&event, 0, sizeof(event));
    event.message = task->message;
    event.async_message = task->message;
    event.file = task->file;
    event.line = task->line;
    event.level = task->level;
    event.time = task->time_valid ? &task->time : NULL;

    thread_state = __async_log_thread_state__();
    if (!thread_state) {
        __async_log_lock_state__();
        __async_log_increment_stat_locked__(&async_log.stats.asynchronous_dispatches);
        __async_log_release_task_locked__(task);
        __async_log_unlock_state__();
        __async_log_write_stdout__(&event);
        if (cancel_disabled) __async_log_leave_callback_region__(old_cancel_state);
        return;
    }
    if (thread_state->worker_depth == UINT_MAX) abort();
    thread_state->worker_depth++;
    __async_log_lock_state__();
    __async_log_increment_stat_locked__(&async_log.stats.asynchronous_dispatches);
    config = __async_log_config_acquire_locked__();
    __async_log_unlock_state__();
    __async_log_dispatch__(&event, config, thread_state);
    __async_log_finish_dispatch__(config);
    __async_log_lock_state__();
    __async_log_release_task_locked__(task);
    __async_log_unlock_state__();
    thread_state->worker_depth--;
    if (cancel_disabled) __async_log_leave_callback_region__(old_cancel_state);
}

void __async_log_init_event__(LogEvent *event, void *ctx) {
    if (!event) return;
    if (!event->time) {
        time_t now = time(NULL);

        if (__async_log_localtime__(now, &event->time_storage) == 0)
            event->time = &event->time_storage;
    }
    event->ctx = ctx;
}

#undef ASYNC_LOG_TRUNCATION_SUFFIX
