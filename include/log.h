/*
 * log.h -- apprecorder's levelled log. Sole owner of diagnostic output.
 *
 * THREE CONSTRAINTS SHAPE THIS
 *
 * 1. Negligible idle cost. A call below the compile-time floor
 *    (APR_LOG_COMPILE_MIN) is erased by the preprocessor -- no code, no
 *    argument evaluation. A call above the floor but below the runtime level
 *    is one relaxed load, one compare and a well-predicted branch; its
 *    arguments are never evaluated either.
 *
 * 2. Thread safety with no lock on the caller. Capture threads log. Records go
 *    into a fixed lock-free ring; a single consumer drains and formats them.
 *
 * 3. Safe from an audio callback. Nothing on the emit path allocates, takes a
 *    lock, or touches the file system:
 *
 *      - APR_RT0/1/2 are the real-time forms. They store a pointer to a static
 *        format literal and up to two int64 values. No formatting happens on
 *        the calling thread at all.
 *      - APR_TRACE/DEBUG/INFO/WARN/ERROR format into the ring slot with
 *        _vsnwprintf_s. That allocates nothing and takes no lock, but it is
 *        not free; on a capture callback prefer the RT forms.
 *
 * DRAINING
 *
 *   Someone must call apr_log_drain() -- exactly one thread at a time; it is
 *   the single consumer. apr_log_init() with `background_drain` set spawns a
 *   thread that does it (waking on an event, or every 250 ms for records left
 *   by real-time producers, which never signal an event). Tests and simple
 *   tools drain by hand so nothing depends on timing.
 *
 * OVERFLOW POLICY -- note the difference from core/ringbuf.c
 *
 *   When the ring is full the NEWEST record is dropped and counted. A log ring
 *   only fills when the drain is starved, and the records already queued are
 *   the context that explains the stall; also, refusing at the door is the
 *   only policy that never writes over a slot the consumer might be reading.
 *   core/ringbuf.c carries live audio and makes the opposite choice (drop the
 *   oldest) for reasons documented there. Neither is a default; both are the
 *   right answer for their payload.
 */
#ifndef APPRECORDER_LOG_H
#define APPRECORDER_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

/* Levels as preprocessor integers so the compile-time floor can be an #if. */
#define APR_LV_TRACE 0
#define APR_LV_DEBUG 1
#define APR_LV_INFO  2
#define APR_LV_WARN  3
#define APR_LV_ERROR 4
#define APR_LV_OFF   5

typedef enum AprLogLevel {
    APR_LOG_TRACE = APR_LV_TRACE,
    APR_LOG_DEBUG = APR_LV_DEBUG,
    APR_LOG_INFO  = APR_LV_INFO,
    APR_LOG_WARN  = APR_LV_WARN,
    APR_LOG_ERROR = APR_LV_ERROR,
    APR_LOG_OFF   = APR_LV_OFF
} AprLogLevel;

/* Everything below this is removed at compile time. Override on the command
 * line to strip trace/debug entirely from a shipping build. */
#ifndef APR_LOG_COMPILE_MIN
#  ifdef NDEBUG
#    define APR_LOG_COMPILE_MIN APR_LV_DEBUG
#  else
#    define APR_LOG_COMPILE_MIN APR_LV_TRACE
#  endif
#endif

/* Records held between drains. Power of two. Each slot is fixed size and the
 * whole ring is a static array: the log never allocates, at any point. */
#ifndef APR_LOG_RING_SLOTS
#define APR_LOG_RING_SLOTS 256
#endif

/* Characters available for one formatted message, terminator included. */
#define APR_LOG_TEXT_CCH 176

/* Receives one fully rendered line (no trailing newline). Called on the
 * draining thread only. `user` is whatever was handed to apr_log_set_sink. */
typedef void (*AprLogSink)(const wchar_t *line, void *user);

typedef struct AprLogConfig {
    AprLogLevel    level;            /* records below this are dropped at the call */
    const wchar_t *path;             /* UTF-8 log file, or NULL for none */
    int            to_debugger;      /* also OutputDebugStringW */
    int            to_stderr;        /* also stderr */
    int            background_drain; /* spawn the drain thread */
} AprLogConfig;

/* ---------------------------------------------------------------------------
 * Lifecycle. Not thread-safe against itself: call from one thread, before the
 * threads that will log. Calling init twice shuts the previous one down first.
 * ------------------------------------------------------------------------- */

/* Configure sinks and level. `cfg` is copied; `cfg->path` is copied into an
 * internal buffer, so the caller keeps ownership. Returns a failure only if a
 * requested file could not be opened -- logging still works without it. */
AprErr apr_log_init(const AprLogConfig *cfg);

/* Stop the drain thread, drain what is queued, close the file, clear the sink.
 * Safe to call when never initialised, and safe to call twice.
 *
 * IT CAN FAIL, AND EVERY LINE BELOW IT DEPENDS ON THE ANSWER.
 *
 *   apr_ok()  -- the drain thread has exited. The file is closed, the sink is
 *                cleared, and nothing is left reading the ring.
 *
 *   failure   -- the bounded wait for the drain thread timed out. IT IS STILL
 *                RUNNING, and it is inside apr_log_drain(): rendering records,
 *                calling the sink, writing to the file handle. So the file is
 *                NOT closed, the sink is NOT cleared and the ring is NOT
 *                touched -- closing a handle a live thread is about to
 *                WriteFile to, or clearing a sink pointer it is about to call,
 *                is the same bug this whole family had (join.h).
 *
 *   A failed shutdown leaves the log usable and leaves the thread joinable.
 *   Call it again later and it will finish the job. apr_log_init() refuses
 *   while a shutdown is outstanding, because re-initialising would memset the
 *   ring under that thread. */
AprErr apr_log_shutdown(void);

/* Install (or clear, with NULL) the in-process sink. Thread-safe against
 * loggers; do not race it against the drain. */
void apr_log_set_sink(AprLogSink sink, void *user);

/* ---------------------------------------------------------------------------
 * Level. Both are thread-safe and allocation-free; the getter is a relaxed
 * load, cheap enough for the hot path.
 * ------------------------------------------------------------------------- */

void        apr_log_set_level(AprLogLevel level);
AprLogLevel apr_log_get_level(void);
int         apr_log_enabled(AprLogLevel level);

/* ---------------------------------------------------------------------------
 * Consumption
 * ------------------------------------------------------------------------- */

/* Render and emit every complete queued record. SINGLE CONSUMER: exactly one
 * thread may be inside this at a time, and if background_drain is on, that
 * thread is the drain thread. Returns the number of records emitted.
 * Allocation-free; may block on its file sink, which is why it is never called
 * from a capture thread. */
size_t apr_log_drain(void);

/* Records accepted into the ring, and records refused because it was full,
 * since init. Either pointer may be NULL. Thread-safe. */
void apr_log_stats(uint64_t *out_emitted, uint64_t *out_dropped);

/* ---------------------------------------------------------------------------
 * Emission -- the macros are the API; the two functions are plumbing.
 * ------------------------------------------------------------------------- */

/* fmt is a wide printf format; formatting happens on the calling thread into
 * the ring slot. Allocation-free, lock-free. */
void apr_log_write(AprLogLevel level, const char *func, const char *file,
                   int line, _In_z_ _Printf_format_string_ const wchar_t *fmt, ...);

/* Real-time form. `fmt` MUST be a literal with static lifetime and may contain
 * at most two %lld conversions and nothing else -- it is stored by pointer and
 * expanded later, on the drain thread, against `a` and `b`. */
void apr_log_write_rt(AprLogLevel level, const char *func, const char *file,
                      int line, const wchar_t *fmt, int64_t a, int64_t b);

/* Log a whole AprErr: context, decoded message, symbolic name, raise site. */
void apr_log_err(AprLogLevel level, const AprErr *e,
                 const char *func, const char *file, int line);

#define APR_LOG_AT_(level, ...) \
    do { if (apr_log_enabled(level)) \
             apr_log_write((level), __func__, __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define APR_LOG_RT_AT_(level, fmt, a, b) \
    do { if (apr_log_enabled(level)) \
             apr_log_write_rt((level), __func__, __FILE__, __LINE__, (fmt), \
                              (int64_t)(a), (int64_t)(b)); \
    } while (0)

#if APR_LOG_COMPILE_MIN <= APR_LV_TRACE
#  define APR_TRACE(...) APR_LOG_AT_(APR_LOG_TRACE, __VA_ARGS__)
#else
#  define APR_TRACE(...) ((void)0)
#endif

#if APR_LOG_COMPILE_MIN <= APR_LV_DEBUG
#  define APR_DEBUG(...) APR_LOG_AT_(APR_LOG_DEBUG, __VA_ARGS__)
#else
#  define APR_DEBUG(...) ((void)0)
#endif

#if APR_LOG_COMPILE_MIN <= APR_LV_INFO
#  define APR_INFO(...) APR_LOG_AT_(APR_LOG_INFO, __VA_ARGS__)
#else
#  define APR_INFO(...) ((void)0)
#endif

#if APR_LOG_COMPILE_MIN <= APR_LV_WARN
#  define APR_WARN(...) APR_LOG_AT_(APR_LOG_WARN, __VA_ARGS__)
#else
#  define APR_WARN(...) ((void)0)
#endif

#if APR_LOG_COMPILE_MIN <= APR_LV_ERROR
#  define APR_ERROR(...) APR_LOG_AT_(APR_LOG_ERROR, __VA_ARGS__)
#else
#  define APR_ERROR(...) ((void)0)
#endif

/* Real-time forms: zero, one or two int64 parameters. Use these from capture
 * callbacks. */
#define APR_RT0(level, fmt)       APR_LOG_RT_AT_((level), (fmt), 0, 0)
#define APR_RT1(level, fmt, a)    APR_LOG_RT_AT_((level), (fmt), (a), 0)
#define APR_RT2(level, fmt, a, b) APR_LOG_RT_AT_((level), (fmt), (a), (b))

#define APR_LOG_ERR(level, errp) \
    do { if (apr_log_enabled(level)) \
             apr_log_err((level), (errp), __func__, __FILE__, __LINE__); \
    } while (0)

#endif /* APPRECORDER_LOG_H */
