#ifndef _ASYNC_LOG_H_
#define _ASYNC_LOG_H_

#include "thread.h"
#include "type.h"


#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>


enum {LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR, LOG_FATAL, LOG_LEVEL_COUNT};

extern const char *const ASYNC_LOG_TIPS[LOG_LEVEL_COUNT];
extern const char *const ASYNC_LOG_COLOURS[LOG_LEVEL_COUNT];


#define ASYNC_LOG_EVENT_API_VERSION 2
#define ASYNC_LOG_MAX_CALLBACKS 64
#define ASYNC_LOG_MAX_MESSAGE_LENGTH 512
#define ASYNC_LOG_MAX_FILE_LENGTH 512
/* Legacy compatibility constant; it does not limit asynchronous task capacity. */
#define ASYNC_LOG_MAX_THREAD_POOL_SIZE 1024
/* Legacy constant retained for callers that handled former nonblocking config updates. */
#define ASYNC_LOG_CONFIG_BUSY 2


/*
 * Return-code convention:
 * - Status APIs return 0 for success, 1 for ordinary resource/argument
 *   failures, ASYNC_LOG_CONFIG_BUSY when a nonblocking exclusive
 *   reconfiguration races an active callback snapshot or logger shutdown, and -1 when called from
 *   a forbidden logger callback/worker context.
 * - Count-returning APIs return a non-negative count on success and negative
 *   error codes; they use `-ASYNC_LOG_CONFIG_BUSY` for a busy callback snapshot or shutdown
 *   because the positive legacy value can also be a valid count.
 *
 * Private synchronization primitive failures are unrecoverable because lock
 * ownership can be indeterminate; they terminate the process with abort().
 * Public return values report normal operational failures only.
 */


/*
 * The logger is thread-safe but is not async-signal-safe. Do not call any
 * logger API from a signal handler. A child created by fork() from a
 * multi-threaded process must exec before using the logger again, because it
 * can inherit logger and stream locks whose owning threads no longer exist.
 *
 * async_log_exit() waits for active callbacks. Callbacks therefore need a
 * bounded completion path, and normal threads should stop submitting logs
 * before shutdown when bounded exit latency is required.
 */


#if defined(__GNUC__) || defined(__clang__)
    #define ASYNC_LOG_PRINTF_FORMAT(format_index, first_argument) \
        __attribute__((format(printf, format_index, first_argument)))
#else
    #define ASYNC_LOG_PRINTF_FORMAT(format_index, first_argument)
#endif


#define asynclog_trace(...) \
    ((void)(async_log_level_enabled(LOG_TRACE) ? \
        (__async_log_print__(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))
#define asynclog_debug(...) \
    ((void)(async_log_level_enabled(LOG_DEBUG) ? \
        (__async_log_print__(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))
#define asynclog_info(...) \
    ((void)(async_log_level_enabled(LOG_INFO) ? \
        (__async_log_print__(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))
#define asynclog_warning(...) \
    ((void)(async_log_level_enabled(LOG_WARNING) ? \
        (__async_log_print__(LOG_WARNING, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))
#define asynclog_error(...) \
    ((void)(async_log_level_enabled(LOG_ERROR) ? \
        (__async_log_print__(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))
#define asynclog_fatal(...) \
    ((void)(async_log_level_enabled(LOG_FATAL) ? \
        (__async_log_print__(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__), 0) : 0))


typedef struct LogEvent {
    /*
     * A callback always receives an already formatted, NUL-terminated message.
     * This is essential because a va_list cannot safely outlive the call that
     * created it, and an asynchronous callback runs on another thread.
     * The logger owns this event; callbacks receive it read-only and may not
     * retain its address after returning.
     */
    const char *message;
    const char *file;
    struct tm time_storage;
    struct tm *time;
    void *ctx;
    int line;
    int level;
    const char *async_message; /* Deprecated compatibility alias for message. */
} LogEvent;


/* Select how a full asynchronous queue is handled. */
typedef enum {
    ASYNC_LOG_OVERFLOW_SYNCHRONOUS,
    ASYNC_LOG_OVERFLOW_DROP
} AsyncLogOverflowPolicy;


/* Select when built-in stdout and file callbacks flush their stream. */
typedef enum {
    /* Flush error and fatal records; this is the default. */
    ASYNC_LOG_FLUSH_ON_ERROR,
    /* Flush every record, trading throughput for immediate write errors. */
    ASYNC_LOG_FLUSH_ALWAYS,
    /* Never explicitly flush; the stream owner remains responsible for it. */
    ASYNC_LOG_FLUSH_NEVER
} AsyncLogFlushPolicy;


typedef struct {
    /* Tasks whose worker began dispatching them. */
    uint64_t asynchronous_dispatches;
    uint64_t synchronous_dispatches;
    uint64_t synchronous_overflows;
    uint64_t rejected_submissions;
    uint64_t dropped_messages;
    /* Log attempts rejected because they originated from a logger callback. */
    uint64_t recursive_dropped_messages;
    uint64_t stopping_fallbacks;
    /* Records written directly to stdout because configured callback contexts
     * were unsafe or unavailable during an emergency fallback path. */
    uint64_t stdout_fallbacks;
    /* Queued tasks discarded by async_log_exit(0). */
    uint64_t shutdown_discarded_messages;
    /* Output attempts for which fwrite, fflush, or ferror reported failure.
     * A pre-existing sticky stream error is included until the owner clears it. */
    uint64_t write_failures;
} AsyncLogStats;


/* Private per-thread bookkeeping used by the logger implementation. */
typedef struct {
    #if defined(__OS_UNIX__)
        pthread_key_t key;
    #elif defined(__OS_WINDOWS__)
        DWORD key;
    #endif
} AsyncLogThreadStorage;


/* Private per-thread dispatch state used by the logger implementation. */
typedef struct {
    unsigned int callback_depth;
    unsigned int worker_depth;
} AsyncLogThreadState;


/*
 * The event is valid only for this call and callbacks must not retain it.
 * Callback and callback-lock functions must return normally; they must not
 * longjmp, cancel/exit the thread, or otherwise bypass the logger's cleanup.
 * POSIX thread cancellation is deferred while callbacks run, but arbitrary
 * non-local exits are not recoverable in portable C and remain forbidden.
 */
typedef void (*_LogCallbackFunc)(const LogEvent *event);
typedef void (*_LogLockFunc)(int lock, void *ctx);


typedef struct _LogCallback {
    _LogCallbackFunc func;
    void *ctx;
} _LogCallback;


/**
 * @brief Initialize an asynchronous logger.
 * @param n_workers The number of threads (like `4`).
 * @param queue_capacity The maximum length of thread queue (like `32`).
 *
 * Task records are preallocated for the current run, so steady-state logging
 * does not allocate on the hot path and `queue_capacity` is not truncated by an
 * unrelated fixed task array.
 *
 * This call is rejected from a logger callback or logger worker. In
 * particular, it never waits for a concurrent shutdown from those contexts.
 *
 * @return `0` when asynchronous logging is running (or was already running),
 * `1` for invalid arguments or resource initialization failure, and `-1` when
 * called from a logger callback or worker. On failure, log calls continue with
 * the synchronous fallback.
**/
int async_log_init(int n_workers, int queue_capacity);


/**
 * @brief Stop the asynchronous logger and release resources for its current run.
 * @param safe_exit `1` for awaiting queued tasks, `0` for discarding queued tasks.
 *
 * Concurrent callers wait for the active shutdown to finish. Calls made from a
 * logger callback or logger worker return immediately to avoid self-deadlock.
 * A completed exit releases the worker pool and queued task storage, and clears
 * callbacks and the callback lock, so their external contexts may be released
 * before a later initialization. The logger's small synchronization core is a
 * process-lifetime singleton; it remains available for synchronous fallback
 * logging and a later `async_log_init()` call.
 *
 * @return `0` on success or when the logger was already stopped, `1` when
 *         synchronization initialization or worker-pool destruction fails, and
 *         `-1` when called from a logger callback or worker.
**/
int async_log_exit(int safe_exit);


/**
 * @brief Set the asynchronous log mode.
 * @param mode `1` for not printing, `0` for default printing.
 *
 * This setting is synchronized with active log writers. It affects entries
 * dispatched after this call; entries already being dispatched use a snapshot.
 * It is rejected from a logger callback/worker like other configuration
 * mutations.
 * @return `0` on success, `1` if a configuration snapshot cannot be allocated,
 *         logger shutdown is in progress, or synchronization initialization fails,
 *         or `-1` when called from a logger callback/worker.
**/
int async_log_setting(int mode);


/**
 * @brief Configure the asynchronous logger written to the disk.
 * @param f The pointer of asynchronous log file, or `NULL` to remove the
 *          configured file callback.
 * @return `0` on success, `ASYNC_LOG_CONFIG_BUSY` if a callback snapshot is
 *         active or logger shutdown is in progress, `1` if memory is unavailable or synchronization
 *         initialization fails,
 *         or `-1` when called from a logger callback/worker.
 *
 * This replaces (rather than appends to) the existing built-in file callback.
 * It does not alter custom callbacks, even ones using
 * `__async_log_callback_write__`.
 * This succeeds only when no callback snapshot is active and shutdown is not
 * in progress. Otherwise it returns `ASYNC_LOG_CONFIG_BUSY` so callers can
 * avoid unbounded waits and retry later. After success, no callback can still reference the
 * replaced `FILE *`, so the old file may be closed. Reconfiguration from a
 * callback/worker is rejected for the same lifetime reason.
**/
int async_log_config_write(FILE *f);


/**
 * @brief Configure the multi-thread lock for callback dispatch.
 * @param func Thread lock function (go to the `async_log.h` declaration to see how to use it).
 * @param ctx A lock (e.g., a `Mutex`). Passing `NULL` for `func` clears the
 *            configured lock.
 *
 * @return `0` on success, `ASYNC_LOG_CONFIG_BUSY` if a callback snapshot is
 *         active or logger shutdown is in progress, `1` on allocation or synchronization initialization failure,
 *         or `-1` when called from a logger callback/worker.
 *
 * This succeeds only when no callback snapshot is active and shutdown is not
 * in progress. Otherwise it returns `ASYNC_LOG_CONFIG_BUSY` so callers can
 * avoid unbounded waits and retry later. After success, the former lock/context is no longer in
 * use. It is rejected from a callback/worker because changing a lock while it
 * is held cannot be made safe without risking a self-deadlock.
 * @example
 * @code
void mutex_func(int lock, void *ctx) {
    Mutex *mutex = (Mutex *)ctx;
    if (lock) {
        printf("-------- lock --------\n");
        mutex_lock(mutex);
    } else {
        mutex_unlock(mutex);
        printf("------- unlock -------\n");
    }
}
async_log_config_thread_lock(mutex_func, &mutex);
 * @endcode
**/
int async_log_config_thread_lock(_LogLockFunc func, void *ctx);


/**
 * @brief Add a custom callback function.
 * @param func Callback function (go to the `async_log.h` declaration to see how to use it).
 * @param ctx Store the user's context data and return it directly after the callback is executed.
 * @return `0` on success, `1` for an invalid callback, a full callback list,
 *         configuration-snapshot allocation failure, logger shutdown, or synchronization
 *         initialization failure, and `-1` when called from a logger
 *         callback/worker.
 *
 * Read the already-formatted `event->message` rather than a va_list.
 * `LogEvent` API version 2 deliberately no longer exposes a `va_list`, which
 * cannot safely cross an asynchronous boundary. A log call made from a logger
 * callback or callback lock hook is dropped and counted in
 * `recursive_dropped_messages`; this prevents callback feedback loops and
 * synchronous-overflow recursion. Configuration changes are visible to later
 * dispatches; an active dispatch uses a stable callback snapshot. Adding a
 * callback is rejected from a logger callback/worker like other configuration
 * mutations.
 * @example
 * @code
void custom_callback(const LogEvent *event) {
    printf("I want to write to a CSV file.\n");
    fprintf(event->ctx, "This, is, me, %s\n", "Illusionna");
    fprintf(event->ctx, "%s", event->message);
}
FILE *fp = fopen("demo.csv", "w");
async_log_add_callback(custom_callback, fp);
 * @endcode
**/
int async_log_add_callback(_LogCallbackFunc func, void *ctx);


/**
 * @brief Remove every callback whose function and context both match.
 * @return The number of callbacks removed, `-ASYNC_LOG_CONFIG_BUSY` when a
 *         callback snapshot is active or logger shutdown is in progress, or `-1` when called from a logger
 *         callback/worker, when allocation fails, or when synchronization
 *         initialization fails.
 *
 * This succeeds only when no callback snapshot is active and shutdown is not
 * in progress. Otherwise it returns `-ASYNC_LOG_CONFIG_BUSY` so callers can distinguish
 * retryable contention from ordinary failures. After success, callers can
 * release removed contexts immediately without racing active snapshots.
**/
int async_log_remove_callback(_LogCallbackFunc func, void *ctx);


/**
 * @brief Remove all custom callbacks when no active callback snapshot exists.
 * If a callback is active or logger shutdown is in progress, this returns
 * `ASYNC_LOG_CONFIG_BUSY` so callers can retry later. The built-in file callback configured by
 * `async_log_config_write()` remains installed.
 * @return `0` on success, `ASYNC_LOG_CONFIG_BUSY` if a callback snapshot is
 *         active or logger shutdown is in progress, `1` when allocation or synchronization initialization fails,
 *         or `-1` when called from a logger
 *         callback/worker.
**/
int async_log_clear_callbacks(void);


/**
 * @brief Set the behavior when an asynchronous task cannot be queued.
 *
 * `ASYNC_LOG_OVERFLOW_SYNCHRONOUS` preserves the message by dispatching it in
 * the calling thread. `ASYNC_LOG_OVERFLOW_DROP` drops it and increments stats.
**/
void async_log_set_overflow_policy(AsyncLogOverflowPolicy policy);


/**
 * @brief Set stream flushing for the built-in stdout and file callbacks.
 *
 * The default, `ASYNC_LOG_FLUSH_ON_ERROR`, flushes ERROR and more severe
 * records. Invalid values select that default. This setting is thread-safe
 * and can be changed before asynchronous logging is initialized.
**/
void async_log_set_flush_policy(AsyncLogFlushPolicy policy);


/**
 * @brief Set the minimum level accepted by logging calls.
 *
 * Messages below `level` are skipped before formatting. Invalid values select
 * `LOG_TRACE`, which enables every defined level.
**/
void async_log_set_min_level(int level);


/**
 * @brief Return nonzero when a level passes the current minimum-level filter.
 *
 * This is used by the public logging macros so filtered messages avoid even
 * evaluating their formatting arguments.
**/
int async_log_level_enabled(int level);


/**
 * @brief Copy logger counters into `stats`; passing `NULL` is a no-op.
 * Counters saturate at `UINT64_MAX` rather than wrapping.
**/
void async_log_get_stats(AsyncLogStats *stats);


/** @brief Reset logger counters. **/
void async_log_reset_stats(void);


/**
 * @brief Print an asynchronous log to terminal.
 * @param level The level of asynchronous log (`LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`, `LOG_FATAL`).
 * @param file The C language file. For asynchronous dispatch, up to
 *             `ASYNC_LOG_MAX_FILE_LENGTH - 1` bytes are copied into the task.
 * @param line The C language file line.
 * @param fmt The content string. Formatted messages longer than
 *             `ASYNC_LOG_MAX_MESSAGE_LENGTH - 1` bytes are shortened and
 *             suffixed with ` ...[truncated]`.
**/
void __async_log_print__(int level, const char *file, int line, const char *fmt, ...)
    ASYNC_LOG_PRINTF_FORMAT(4, 5);


/**
 * @brief Asynchronous universal callback format.
 * @param event The event of asynchronous log.
 * @param enable_colour `1` for activation, `0` for deactivation.
**/
void __async_log_callback_common_format__(const LogEvent *event, int enable_colour);


/**
 * @brief Standard output callback function.
 * @param event The event of asynchronous log.
 *
 * ANSI colour is emitted only when stdout is attached to a terminal.
**/
void __async_log_callback_stdout__(const LogEvent *event);


/**
 * @brief Write to the disk file callback function.
 * @param event The event of asynchronous log.
**/
void __async_log_callback_write__(const LogEvent *event);


#undef ASYNC_LOG_PRINTF_FORMAT


#endif
