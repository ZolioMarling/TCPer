#include "async_log.h"
#include "async_log_internal.h"
#include "os.h"

#include <string.h>

#define ASYNC_LOG_HEXADECIMAL "0123456789abcdef"

int __async_log_detect_stdout_colour__(void) {
#if defined(__OS_UNIX__)
    return isatty(fileno(stdout)) != 0;
#elif defined(__OS_WINDOWS__)
    DWORD mode;
    HANDLE handle;

    if (_isatty(_fileno(stdout)) == 0) return 0;
    handle = (HANDLE)_get_osfhandle(_fileno(stdout));
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return 0;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return 1;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#endif
}

int __async_log_is_utf8_continuation__(unsigned char character) {
    return character >= 0x80 && character <= 0xbf;
}

usize __async_log_utf8_sequence_length__(const unsigned char *text) {
    unsigned char first;
    unsigned char second;

    if (!text || text[0] == '\0') return 0;
    first = text[0];
    second = text[1];
    if (first >= 0xc2 && first <= 0xdf)
        return __async_log_is_utf8_continuation__(second) ? 2 : 0;
    if (second == '\0') return 0;
    if (first == 0xe0)
        return second >= 0xa0 && second <= 0xbf && text[2] != '\0' &&
            __async_log_is_utf8_continuation__(text[2]) ? 3 : 0;
    if ((first >= 0xe1 && first <= 0xec) || (first >= 0xee && first <= 0xef))
        return __async_log_is_utf8_continuation__(second) && text[2] != '\0' &&
            __async_log_is_utf8_continuation__(text[2]) ? 3 : 0;
    if (first == 0xed)
        return second >= 0x80 && second <= 0x9f && text[2] != '\0' &&
            __async_log_is_utf8_continuation__(text[2]) ? 3 : 0;
    if (text[2] == '\0') return 0;
    if (first == 0xf0)
        return second >= 0x90 && second <= 0xbf &&
            __async_log_is_utf8_continuation__(text[2]) && text[3] != '\0' &&
            __async_log_is_utf8_continuation__(text[3]) ? 4 : 0;
    if (first >= 0xf1 && first <= 0xf3)
        return __async_log_is_utf8_continuation__(second) &&
            __async_log_is_utf8_continuation__(text[2]) && text[3] != '\0' &&
            __async_log_is_utf8_continuation__(text[3]) ? 4 : 0;
    if (first == 0xf4)
        return second >= 0x80 && second <= 0x8f &&
            __async_log_is_utf8_continuation__(text[2]) && text[3] != '\0' &&
            __async_log_is_utf8_continuation__(text[3]) ? 4 : 0;
    return 0;
}

usize __async_log_append_escaped_byte__(char *buffer, usize capacity, usize offset,
                                        unsigned char character) {
    if (!buffer || offset > capacity || capacity - offset <= 4) return offset;
    buffer[offset++] = '\\';
    buffer[offset++] = 'x';
    buffer[offset++] = ASYNC_LOG_HEXADECIMAL[character >> 4];
    buffer[offset++] = ASYNC_LOG_HEXADECIMAL[character & 0x0f];
    return offset;
}

usize __async_log_append_escaped__(char *buffer, usize capacity, usize offset,
                                   const char *text) {
    const unsigned char *source = (const unsigned char *)text;

    if (!buffer || capacity == 0) return 0;
    if (offset >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    if (!text) return offset;
    while (*source != '\0' && offset + 1 < capacity) {
        unsigned char character = *source;
        usize sequence_length;

        if (character < 0x20 || character == 0x7f) {
            usize next = __async_log_append_escaped_byte__(buffer, capacity, offset, character);

            if (next == offset) break;
            offset = next;
            source++;
            continue;
        }
        sequence_length = __async_log_utf8_sequence_length__(source);
        if (sequence_length != 0) {
            if (sequence_length == 2 && character == 0xc2 && source[1] <= 0x9f) {
                if (capacity - offset <= 8) break;
                offset = __async_log_append_escaped_byte__(buffer, capacity, offset, character);
                offset = __async_log_append_escaped_byte__(buffer, capacity, offset, source[1]);
                source += 2;
                continue;
            }
            if (capacity - offset <= sequence_length) break;
            memcpy(buffer + offset, source, sequence_length);
            offset += sequence_length;
            source += sequence_length;
            continue;
        }
        if (character >= 0x80) {
            usize next = __async_log_append_escaped_byte__(buffer, capacity, offset, character);

            if (next == offset) break;
            offset = next;
            source++;
            continue;
        }
        buffer[offset++] = (char)character;
        source++;
    }
    buffer[offset] = '\0';
    return offset;
}

void __async_log_callback_common_format__(const LogEvent *event, int enable_colour) {
    char buffer[(ASYNC_LOG_MAX_MESSAGE_LENGTH + ASYNC_LOG_MAX_FILE_LENGTH) * 4 + 256];
    char time_buffer[32];
    const char *file;
    const char *message;
    const char *tip;
    const char *colour;
    FILE *out;
    usize offset;
    usize remaining;
    int write_failed = 0;
    int level;
    int n;

    if (!event) return;
    if (!event->time || strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", event->time) == 0)
        snprintf(time_buffer, sizeof(time_buffer), "unknown-time");

    level = event->level >= LOG_TRACE && event->level < LOG_LEVEL_COUNT ? event->level : LOG_ERROR;
    file = event->file ? event->file : "<unknown>";
    message = event->message ? event->message : event->async_message;
    if (!message) message = "(missing log message)";
    tip = ASYNC_LOG_TIPS[level];
    colour = ASYNC_LOG_COLOURS[level];
    remaining = sizeof(buffer);

    if (enable_colour)
        n = snprintf(buffer, remaining, "%s %s%-7s \x1b[90m", time_buffer, colour, tip);
    else n = snprintf(buffer, remaining, "%s %-7s ", time_buffer, tip);
    if (n < 0) return;
    offset = (usize)n < remaining ? (usize)n : remaining - 1;

    offset = __async_log_append_escaped__(buffer, sizeof(buffer), offset, file);
    remaining = sizeof(buffer) - offset;
    if (enable_colour)
        n = snprintf(buffer + offset, remaining, ":%d:\x1b[0m ", event->line);
    else n = snprintf(buffer + offset, remaining, ":%d: ", event->line);
    if (n < 0) return;
    offset += (usize)n < remaining ? (usize)n : remaining - 1;

    offset = __async_log_append_escaped__(buffer, sizeof(buffer), offset, message);
    if (offset + 1 < sizeof(buffer)) {
        buffer[offset++] = '\n';
        buffer[offset] = '\0';
    }

    out = event->ctx ? (FILE *)event->ctx : stdout;
    os_flockfile(out);
    if (fwrite(buffer, 1, offset, out) != offset || ferror(out)) write_failed = 1;
    if (__async_log_should_flush__(level) && (fflush(out) != 0 || ferror(out)))
        write_failed = 1;
    os_funlockfile(out);
    if (write_failed) __async_log_record_output_failure__();
}

void __async_log_callback_write__(const LogEvent *event) {
    __async_log_callback_common_format__(event, 0);
}

void __async_log_callback_stdout__(const LogEvent *event) {
    __async_log_callback_common_format__(event, __async_log_detect_stdout_colour__());
}

#undef ASYNC_LOG_HEXADECIMAL
