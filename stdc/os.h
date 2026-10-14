#ifndef _OS_H_
#define _OS_H_


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


#if defined(__OS_WINDOWS__)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <ctype.h>
    #include <direct.h>
    #include <fcntl.h>
    #include <io.h>
    #include <windows.h>
    #include <aclapi.h>
    #include <bcrypt.h>
#elif defined(__OS_UNIX__)
    #include <fcntl.h>
    #include <dirent.h>
    #include <unistd.h>
    #include <limits.h>
    #include <sys/mman.h>
    #include <sys/file.h>
#endif


#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif


#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#include "type.h"


#if defined(__OS_UNIX__)
    _Static_assert(sizeof(off_t) >= sizeof(int64), "TCPer requires 64-bit file offsets");
#endif


#if defined(__OS_UNIX__)
    #define os_ftell ftello
    #define os_fseek fseeko
    #define os_flockfile flockfile
    #define os_funlockfile funlockfile
#elif defined(__OS_WINDOWS__)
    #define os_ftell _ftelli64
    #define os_fseek _fseeki64
    #define os_flockfile _lock_file
    #define os_funlockfile _unlock_file
#endif


#if defined(__OS_UNIX__)
    typedef struct MapFile {
        void *data;
        usize size;
        int fd;
    } MapFile;
#elif defined(__OS_WINDOWS__)
    typedef struct MapFile {
        void *data;
        size_t size;
        HANDLE hFile;
        HANDLE hMapping;
    } MapFile;
#endif


typedef struct _PathLock {
    #if defined(__OS_UNIX__)
        FILE *file;
        char *path;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
    #endif
} _PathLock;


typedef struct _PathRWLock {
    #if defined(__OS_UNIX__)
        FILE *file;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
        OVERLAPPED overlap;
    #endif
} _PathRWLock;


typedef void (*_ListdirCallback)(char *dir, char *name, bool folder, uint64 size, void *args);


/**
 * @brief Get process ID.
 * @return PID.
**/
int os_getpid(void);


/**
 * @brief Judge whether a filesystem entry exists without opening it.
 * @param path The pointer of path string.
 * @return `1` for existence, `0` for nonentity, `-1` for other errors (such as permission denied, etc.)
**/
int os_access(char *path);


/**
 * @brief Read file and convert it to string (note: the memory allocated must call free).
 * @param path The pointer of file path string.
 * @param range_start The start of range.
 * @param range_end The end of range.
 * @return The string content of file.
**/
char *os_readfile(char *path, int range_start, int range_end);


/**
 * @brief Get current time (`unit: s`).
 * @return Timestamp.
**/
double os_time(void);


/**
 * @brief Get the file name from a path.
 * @param path The path of file.
 * @return File name (note: `/home/Desktop/` returns `NULL`).
**/
char *os_basename(char *path);


/**
 * @brief Wait for a time (`unit: s`).
 * @param second You can set `3s` or `0.02s`.
**/
void os_sleep(double second);


/**
 * @brief Determine whether the path is a directory.
 * @param path The path.
 * @return `1` for directory, `0` for file or inexistence.
**/
int os_isdir(char *path);


/**
 * @brief Determine whether the path is a regular file.
 * @param path The pointer of path.
 * @return `1` for a regular file, `0` for another type or inexistence.
**/
int os_isfile(char *path);


/**
 * @brief Make a directory.
 * @param dir Name of folder.
 * @return `0` for success, `1` for failure.
**/
int os_mkdir(char *dir);


/**
 * @brief Open a regular file for replacement with permissions restricted to the current user.
 * @param path The pointer of path.
 * @return A writable file stream on success, or `NULL` on failure.
 * @note The caller must close the returned stream with `fclose()`.
**/
FILE *os_fopen_private_write(char *path);


/**
 * @brief Produce an absolute canonical path, resolving the existing path or, if the final component does not exist, its parent directory.
 * @param output Buffer that receives the null-terminated canonical path.
 * @param capacity Size of `output` in bytes.
 * @param path The path to canonicalize.
 * @return `0` on success, or `1` on failure.
**/
int os_canonical_path(char *output, usize capacity, char *path);


/**
 * @brief Build the path of a cross-process transfer lock file in the application's temporary lock directory.
 * @param output Buffer that receives the null-terminated lock-file path.
 * @param capacity Size of `output` in bytes.
 * @param lock_id A 64-character hexadecimal lock identifier.
 * @return `0` on success, or `1` on failure.
**/
int os_transfer_lock_path(char *output, usize capacity, char *lock_id);


/**
 * @brief Try to acquire an exclusive cross-process transfer lock without waiting.
 * @param lock Storage that receives the acquired lock state.
 * @param path Lock-file path, normally produced by `os_transfer_lock_path()`.
 * @return `0` if the lock was acquired, or `1` if the lock is already held, an argument is invalid, or a system error occurs.
 * @note On success, the caller must release the lock with `os_path_unlock()`.
**/
int os_path_lock(_PathLock *lock, char *path);


/**
 * @brief Release a cross-process transfer lock.
 * @param lock Lock state previously acquired by `os_path_lock()`.
**/
void os_path_unlock(_PathLock *lock);


/**
 * @brief Open or create a coordination file and wait for a shared or exclusive cross-process lock.
 * @param lock Storage that receives the acquired lock state.
 * @param path Path of the persistent coordination file.
 * @param exclusive Nonzero to acquire an exclusive lock, or zero to acquire a shared lock.
 * @return `0` if the lock was acquired, or `1` if an argument is invalid or a system error occurs.
 * @note On success, the caller must release the lock with `os_path_rwunlock()`.
**/
int os_path_rwlock(_PathRWLock *lock, char *path, int exclusive);


/**
 * @brief Release a shared or exclusive cross-process coordination-file lock.
 * @param lock Lock state previously acquired by `os_path_rwlock()`. `NULL` is ignored.
 * @note The coordination file remains on disk after the lock is released.
**/
void os_path_rwunlock(_PathRWLock *lock);


/**
 * @brief Get the current directory.
 * @param buffer Store the current directory.
 * @param size The size of buffer.
 * @return `NULL` for failure.
**/
char *os_getpwd(char *buffer, int size);


/**
 * @brief Get the executable directory.
 * @param buffer Store the executable directory.
 * @param size The size of buffer.
**/
void os_getexec(char *buffer, int size);


/**
 * @brief Initialize the seed for the rand function.
**/
void os_srand(void);


/**
 * @brief Generate a random value in zone `[low, high]`.
 * @param low The low value of zone.
 * @param high The high value of zone.
 * @return The random value.
**/
double os_random(double low, double high);


/**
 * @brief Fill a buffer with cryptographically secure random bytes supplied by the operating system.
 * @param buffer Destination buffer. It may be `NULL` only when `length` is zero.
 * @param length Number of random bytes to generate.
 * @return `0` on success, or `1` on failure.
 * @note On failure, the buffer may have been partially modified.
**/
int os_random_bytes(void *buffer, usize length);


/**
 * @brief Get the size of file.
 * @param filepath The path of file.
 * @return The size of file (`-1` for failure).
**/
int64 os_file_size(char *filepath);


/**
 * @brief Get the size of an already-open regular file stream.
 * @param stream Open file stream.
 * @return The file size in bytes, or `-1` on failure.
 * @note Flush buffered writes before calling this function when the latest size is required.
**/
int64 os_stream_size(FILE *stream);


/**
 * @brief Determine whether an open file stream and a path refer to the same filesystem object.
 * @param stream Open file stream to examine.
 * @param path Path to compare with the open file.
 * @return `1` if both refer to the same filesystem object, or `0` if they differ or the comparison fails.
 * @note The path may be replaced immediately after this function returns.
**/
int os_stream_matches_path(FILE *stream, char *path);


/**
 * @brief Try to acquire an exclusive operating-system lock on the underlying file without waiting.
 * @param stream Open file stream whose underlying file will be locked.
 * @return `0` if the lock was acquired, or `1` if it is unavailable or an error occurs.
 * @note On success, keep `stream` open and release the lock with `os_file_unlock()`.
**/
int os_file_lock(FILE *stream);


/**
 * @brief Flush buffered stream data and synchronize the underlying file with its storage device.
 * @param stream Open file stream to synchronize.
 * @return `0` on success, or `1` if flushing or synchronization fails.
**/
int os_file_sync(FILE *stream);


/**
 * @brief Wait until an exclusive operating-system lock can be acquired on the underlying file.
 * @param stream Open file stream whose underlying file will be locked.
 * @return `0` if the lock was acquired, or `1` if an error occurs.
 * @note On success, keep `stream` open and release the lock with `os_file_unlock()`.
**/
int os_file_lock_wait(FILE *stream);


/**
 * @brief Release an operating-system file lock acquired through this API.
 * @param stream Open file stream passed to `os_file_lock()` or `os_file_lock_wait()`.
 * @note This function does not close the stream. `NULL` is ignored.
**/
void os_file_unlock(FILE *stream);


/**
 * @brief Atomically replace a destination path by renaming a completed source file.
 * @param source Path of the completed source file.
 * @param destination Path to create or replace.
 * @return `0` on success, or `1` on failure.
 * @note `source` and `destination` must normally reside on the same filesystem or volume. This function does not flush pending file data before the replacement.
**/
int os_rename_replace(char *source, char *destination);


/**
 * @brief Create a new hard link that refers to the same file as an existing path.
 * @param source Path of the existing file.
 * @param destination Path at which to create the new hard link; it must not already exist.
 * @return `0` on success, or `1` if an argument is invalid or the hard link cannot be created.
 * @note Both paths must normally reside on the same filesystem or volume. Changes made through either path affect the same underlying file.
**/
int os_hardlink(char *source, char *destination);


/**
 * @brief Map a file into the process's virtual address space.
 * @param filepath The path of the file to be mapped.
 * @param length The number of bytes to map from the beginning of the file.
 * @return A pointer to a `MapFile` structure containing the memory address (`NULL` for failure).
 * @example
 * @code
MapFile *f = os_mmap("demo.txt", 12);
if (f && f->data) {
    printf("%c\n", ((char *)f->data)[7]);
    os_munmap(f);
} else {
    printf("Failure!\n");
}
 * @endcode
**/
MapFile *os_mmap(char *filepath, usize length);


/**
 * @brief Unmap a previously mapped file and release associated system resources.
 * @param f The pointer to the `MapFile` structure to be released.
**/
void os_munmap(MapFile *f);


/**
 * @brief Enumerate the immediate children of a directory.
 * @param path Path of the directory to enumerate.
 * @param func Callback invoked once for each regular file or directory. It receives the directory path, entry name, a directory flag, the entry size, and `args`.
 * @param args User-defined context passed unchanged to each callback invocation; it may be `NULL`.
 * @return `0` if enumeration completes successfully, or `1` if an argument is invalid, the directory cannot be read, or an entry cannot be inspected safely.
 * @note This function does not recurse and does not invoke `func` for `.` or `..`. Symbolic links and Windows reparse points are skipped and cause the function to return `1`. An entry name passed to `func` is valid only for the duration of that callback invocation. The size is meaningful as a byte count for regular files.
 * @example
 * @code
void print_entry(char *dir, char *name, bool folder, uint64 size, void *args) {
    (void)args;
    if (folder) printf("[directory] %s/%s\n", dir, name);
    else printf("[file] %llu bytes | %s/%s\n", (unsigned long long)size, dir, name);
}

int main(void) {
    if (os_listdir("./downloads", print_entry, NULL) != 0) {
        fprintf(stderr, "Unable to enumerate ./downloads completely.\n");
        return 1;
    }
    return 0;
}
 * @endcode
**/
int os_listdir(char *path, _ListdirCallback func, void *args);


/**
 * @brief Check whether the path exceeds the boundary.
 * @param path A path like `"./documents/../main.tex"`.
 * @return `1` for `True`, `0` for `False`.
**/
bool os_traversal(char *path);


#if defined(__OS_UNIX__)
    /**
     * @brief Recursively remove the contents of an open directory.
     * @param directory_fd Descriptor of the directory to empty. The function takes ownership of it.
     * @return `0` if every entry was removed, or `1` if an error occurs.
     * @note The directory represented by `directory_fd` is emptied but is not itself removed.
    **/
    int __os_remove_directory_fd__(int directory_fd);
#endif


/**
 * @brief Remove a file or directory.
 * @param path The path of file or directory.
 * @return `0` for success.
**/
int os_remove(char *path);


#endif
