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
    #include <direct.h>
    #include <io.h>
    #include <windows.h>
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
    typedef struct {
        void *data;
        usize size;
        int fd;
    } MapFile;
#elif defined(__OS_WINDOWS__)
    typedef struct {
        void *data;
        size_t size;
        HANDLE hFile;
        HANDLE hMapping;
    } MapFile;
#endif


typedef struct {
    #if defined(__OS_UNIX__)
        FILE *file;
        char *path;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
    #endif
} PathLock;


typedef struct {
    #if defined(__OS_UNIX__)
        FILE *file;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
        OVERLAPPED overlap;
    #endif
} PathRWLock;


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
 * @param path The path.
 * @return `1` for a regular file, `0` for another type or inexistence.
**/
int os_isfile(char *path);


/**
 * @brief Make a directory.
 * @param dir Name of folder.
 * @return `0` for success, `1` for failure.
**/
int os_mkdir(char *dir);


/** Open a regular file for replacement with permissions restricted to the current user. */
FILE *os_fopen_private_write(const char *path);


/** Resolve a path entry through its parent without following the final component. */
int os_canonical_path(char *output, usize capacity, const char *path);


/** Build a process-independent lock path in TCPer's bounded system lock directory. */
int os_transfer_lock_path(char *output, usize capacity, const char *lock_id);


/** Acquire a non-blocking process-independent lock for a canonical transfer target. */
int os_path_lock(PathLock *lock, const char *path);


/** Release a path lock and remove its transient Unix lock file. */
void os_path_unlock(PathLock *lock);


/** Acquire a blocking shared or exclusive lock on a persistent coordination file. */
int os_path_rwlock(PathRWLock *lock, const char *path, int exclusive);


/** Release a persistent coordination-file lock. */
void os_path_rwunlock(PathRWLock *lock);


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


/** Fill a buffer with operating-system random bytes. */
int os_random_bytes(void *buffer, usize length);


/**
 * @brief Get the size of file.
 * @param filepath The path of file.
 * @return The size of file (`-1` for failure).
**/

int64 os_filesize(char *filepath);


/** Return the size of the already-open regular file, or -1 on failure. */
int64 os_file_size(FILE *file);

/** Return 1 when an open file still names the same filesystem entry at path. */
int os_file_is_path(FILE *file, const char *path);

/** Acquire a non-blocking exclusive lock for a transfer temporary file. */
int os_file_lock(FILE *file);

/** Flush buffered data and commit it to the backing file before publication. */
int os_file_sync(FILE *file);

/** Release a lock previously acquired with os_file_lock. */
void os_file_unlock(FILE *file);

/** Atomically replace a destination path with a completed temporary file. */
int os_rename_replace(const char *source, const char *destination);


/** Create a hard link without copying file contents. */
int os_hardlink(const char *source, const char *destination);


/**
 * @brief Map a file into the process's virtual address space.
 * @param filepath The path of the file to be mapped.
 * @param length The number of bytes to map from the beginning of the file (`usize` is `size_t`).
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
 * @brief List the contents of a directory.
 * @param path The path of directory.
 * @param func The callback function like `void recurse(char *dir, char *name, bool folder, uint64 size, void *args)`. Go to the `os.h` declaration to see how to use `_ListdirCallback func`.
 * @param args The context arguments.
 * @example
 * @code
void recurse(char *dir, char *name, bool folder, uint64 size, void *args) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (folder) {
        printf("%s\n", path);
        os_listdir(path, recurse, args);
    }
    else printf("%llu | %s\n", size, path);
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


/**
 * @brief Remove a file or directory.
 * @param path The path of file or directory.
 * @return `0` for success.
**/
int os_remove(char *path);


/** Acquire an exclusive file lock, waiting until the current owner releases it. */
int os_file_lock_wait(FILE *file);


#endif
