#if defined(_WIN32) && !defined(_CRT_RAND_S)
    #define _CRT_RAND_S
#endif

#include "os.h"

#include <ctype.h>

#if defined(__OS_WINDOWS__)
    #include <aclapi.h>
    #include <fcntl.h>
#endif

#if defined(__OS_UNIX__)
_Static_assert(sizeof(off_t) >= sizeof(int64), "TCPer requires 64-bit file offsets");
#endif


static volatile unsigned long long OS_SEED = 1;


int os_getpid(void) {
    #if defined(__OS_WINDOWS__)
        return (int)GetCurrentProcessId();
    #elif defined(__OS_UNIX__)
        return (int)getpid();
    #endif
}


int os_access(char *path) {
    if (!path || !path[0]) return -1;
    #if defined(__OS_UNIX__)
        struct stat st;
        if (lstat(path, &st) == 0) return 1;
        return (errno == ENOENT || errno == ENOTDIR) ? 0 : -1;
    #elif defined(__OS_WINDOWS__)
        DWORD attrs = GetFileAttributesA(path);
        if (attrs != INVALID_FILE_ATTRIBUTES) return 1;
        DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME) ? 0 : -1;
    #endif
}


char *os_readfile(char *path, int range_start, int range_end) {
    if (!path || !path[0]) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    int64 file_size = os_file_size(f);
    if (file_size < 0) {
        fclose(f);
        return NULL;
    }

    int64 length;
    if (range_start == 0 && range_end == -1) length = file_size;
    else {
        if (range_start < 0 || range_end < range_start ||
            os_fseek(f, (int64)range_start, SEEK_SET) != 0) {
            fclose(f);
            return NULL;
        }
        int64 requested = (int64)range_end - (int64)range_start + 1;
        int64 available = (int64)range_start < file_size ? file_size - (int64)range_start : 0;
        length = requested < available ? requested : available;
    }

    if ((uint64)length > (uint64)SIZE_MAX - 1) {
        fclose(f);
        return NULL;
    }
    usize capacity = (usize)length + 1;
    char *buffer = malloc(capacity);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    usize nread = fread(buffer, 1, (usize)length, f);
    if (nread < (usize)length && ferror(f)) {
        free(buffer);
        fclose(f);
        return NULL;
    }
    buffer[nread] = 0;
    if (fclose(f) != 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}


double os_time(void) {
    #if defined(__OS_UNIX__)
        struct timespec t;
        // `CLOCK_MONOTONIC` prevents system time from being tampered with.
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
    #elif defined(__OS_WINDOWS__)
        LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        return (double)counter.QuadPart / frequency.QuadPart;
    #else
        return (double)time(NULL);
    #endif
}


char *os_basename(char *path) {
    char *base = path;
    if (path == NULL || *path == '\0') return NULL;
    for (char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    if (*base == '\0') return NULL;
    return base;
}


void os_sleep(double second) {
    if (second <= 0.0) return;
    #if defined(__OS_UNIX__)
        struct timespec requested_time;
        struct timespec remaining_time;
        requested_time.tv_sec = (long)second;
        requested_time.tv_nsec = (long)(1e9 * (second - requested_time.tv_sec));
        while (nanosleep(&requested_time, &remaining_time) == -1 && errno == EINTR) requested_time = remaining_time;
    #elif defined(__OS_WINDOWS__)
        DWORD millisecond = (DWORD)(second * 1000.0);
        if (millisecond == 0 && second > 0) millisecond = 1;
        Sleep(millisecond);
    #endif
}


int os_isdir(char *path) {
    if (path == NULL || *path == '\0') return 0;

    #if defined(__OS_UNIX__)
        struct stat s;
        if (stat(path, &s) != 0) return 0;
        return S_ISDIR(s.st_mode);
    #elif defined(__OS_WINDOWS__)
        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return 1;
        return 0;
    #endif
}


int os_isfile(char *path) {
    if (path == NULL || *path == '\0') return 0;

    #if defined(__OS_UNIX__)
        struct stat s;
        return stat(path, &s) == 0 && S_ISREG(s.st_mode);
    #elif defined(__OS_WINDOWS__)
        struct _stat64 s;
        return _stat64(path, &s) == 0 && (s.st_mode & _S_IFMT) == _S_IFREG;
    #endif
}


int os_mkdir(char *dir) {
    int result = 0;
    errno = 0;
    #if defined(__OS_UNIX__)
        result = mkdir(dir, 0755);
    #elif defined(__OS_WINDOWS__)
        result = _mkdir(dir);
    #endif
    if (result == 0) return 0;
    else {
        if (errno == EEXIST) return 0;
        else return 1;
    }
}


FILE *os_fopen_private_write(const char *path) {
    if (!path || !path[0]) return NULL;
    #if defined(__OS_UNIX__)
        int flags = O_WRONLY | O_CREAT;
        #ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
        #endif
        int descriptor = open(path, flags, S_IRUSR | S_IWUSR);
        if (descriptor < 0) return NULL;
        struct stat status;
        if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
            fchmod(descriptor, S_IRUSR | S_IWUSR) != 0 || ftruncate(descriptor, 0) != 0) {
            close(descriptor);
            return NULL;
        }
        FILE *file = fdopen(descriptor, "w");
        if (!file) close(descriptor);
        return file;
    #elif defined(__OS_WINDOWS__)
        HANDLE token = NULL;
        DWORD information_size = 0;
        TOKEN_USER *user = NULL;
        ACL *acl = NULL;
        HANDLE handle = INVALID_HANDLE_VALUE;
        FILE *file = NULL;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto cleanup;
        GetTokenInformation(token, TokenUser, NULL, 0, &information_size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) goto cleanup;
        user = (TOKEN_USER *)malloc(information_size);
        if (!user || !GetTokenInformation(token, TokenUser, user, information_size,
                                          &information_size)) goto cleanup;

        DWORD acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
            GetLengthSid(user->User.Sid);
        acl = (ACL *)malloc(acl_size);
        if (!acl || !InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !AddAccessAllowedAce(acl, ACL_REVISION,
                                 FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE,
                                 user->User.Sid)) goto cleanup;

        SECURITY_DESCRIPTOR security;
        if (!InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&security, TRUE, acl, FALSE)) goto cleanup;
        SECURITY_ATTRIBUTES attributes = {
            .nLength = sizeof(attributes),
            .lpSecurityDescriptor = &security,
            .bInheritHandle = FALSE
        };
        handle = CreateFileA(path, GENERIC_WRITE | READ_CONTROL | WRITE_DAC, 0, &attributes,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) goto cleanup;

        BY_HANDLE_FILE_INFORMATION information;
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            SetSecurityInfo(handle, SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            NULL, NULL, acl, NULL) != ERROR_SUCCESS ||
            SetFilePointer(handle, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
            !SetEndOfFile(handle)) goto cleanup;

        int descriptor = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
        if (descriptor < 0) goto cleanup;
        handle = INVALID_HANDLE_VALUE;
        file = _fdopen(descriptor, "w");
        if (!file) _close(descriptor);

        cleanup:
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            free(acl);
            free(user);
            if (token) CloseHandle(token);
            return file;
    #endif
}


int os_canonical_path(char *output, usize capacity, const char *path) {
    if (!output || capacity == 0 || !path || !path[0]) return 1;

    char work[4096];
    int n = snprintf(work, sizeof(work), "%s", path);
    if (n < 0 || n >= (int)sizeof(work)) return 1;

    /* Resolve the final component when it exists, including symlink/junction aliases. */
    #if defined(__OS_UNIX__)
        char *existing = realpath(path, NULL);
        if (existing) {
            n = snprintf(output, capacity, "%s", existing);
            free(existing);
            return n < 0 || n >= (int)capacity ? 1 : 0;
        }
        if (errno != ENOENT && errno != ENOTDIR) return 1;
    #elif defined(__OS_WINDOWS__)
        HANDLE existing = CreateFileA(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (existing != INVALID_HANDLE_VALUE) {
            char resolved[4096];
            DWORD length = GetFinalPathNameByHandleA(existing, resolved,
                (DWORD)sizeof(resolved), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(existing);
            if (length == 0 || length >= sizeof(resolved)) return 1;
            char *canonical = strncmp(resolved, "\\\\?\\", 4) == 0 ? resolved + 4 : resolved;
            for (char *p = canonical; *p; p++) {
                if (*p == '\\') *p = '/';
                *p = (char)tolower((unsigned char)*p);
            }
            n = snprintf(output, capacity, "%s", canonical);
            return n < 0 || n >= (int)capacity ? 1 : 0;
        }
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND &&
            error != ERROR_INVALID_NAME) return 1;
    #endif

    #if defined(__OS_UNIX__)
        char *separator = strrchr(work, '/');
    #elif defined(__OS_WINDOWS__)
        char *slash = strrchr(work, '/');
        char *backslash = strrchr(work, '\\');
        char *separator = (!slash || (backslash && backslash > slash)) ? backslash : slash;
    #endif

    char *base = work;
    char parent[4096];
    if (separator) {
        base = separator + 1;
        if (!base[0]) return 1;
        usize parent_length = (usize)(separator - work);
        #if defined(__OS_UNIX__)
            if (parent_length == 0) parent_length = 1;
        #elif defined(__OS_WINDOWS__)
            if (parent_length == 2 && work[1] == ':') parent_length = 3;
        #endif
        if (parent_length >= sizeof(parent)) return 1;
        memcpy(parent, work, parent_length);
        parent[parent_length] = '\0';
    } else {
        snprintf(parent, sizeof(parent), ".");
    }

    #if defined(__OS_UNIX__)
        char *resolved = realpath(parent, NULL);
        if (!resolved) return 1;
        n = snprintf(output, capacity, "%s/%s", resolved, base);
        free(resolved);
    #elif defined(__OS_WINDOWS__)
        char resolved[4096];
        if (!_fullpath(resolved, parent, sizeof(resolved))) return 1;
        for (char *p = resolved; *p; p++) {
            if (*p == '\\') *p = '/';
            *p = (char)tolower((unsigned char)*p);
        }
        for (char *p = base; *p; p++) *p = (char)tolower((unsigned char)*p);
        n = snprintf(output, capacity, "%s/%s", resolved, base);
    #endif
    return n < 0 || n >= (int)capacity ? 1 : 0;
}


int os_transfer_lock_path(char *output, usize capacity, const char *lock_id) {
    if (!output || capacity == 0 || !lock_id || strlen(lock_id) != 64 ||
        strspn(lock_id, "0123456789abcdefABCDEF") != 64) return 1;

    char directory[4096];
    #if defined(__OS_UNIX__)
        int n = snprintf(directory, sizeof(directory), "/tmp/tcper-locks-%llu", (uint64)getuid());
        if (n < 0 || n >= (int)sizeof(directory)) return 1;
        if (mkdir(directory, 0700) != 0 && errno != EEXIST) return 1;
        struct stat st;
        if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
            st.st_uid != getuid() || (st.st_mode & 077) != 0) return 1;
    #elif defined(__OS_WINDOWS__)
        DWORD length = GetTempPathA((DWORD)sizeof(directory), directory);
        if (length == 0 || length >= sizeof(directory)) return 1;
        int n = snprintf(directory + length, sizeof(directory) - length, "tcper-locks");
        if (n < 0 || n >= (int)(sizeof(directory) - length)) return 1;
        if (!CreateDirectoryA(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 1;
        DWORD attrs = GetFileAttributesA(directory);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
            (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return 1;
    #endif

    int result = snprintf(output, capacity, "%s/%s.lock", directory, lock_id);
    return result < 0 || result >= (int)capacity ? 1 : 0;
}


int os_path_lock(PathLock *lock, const char *path) {
    if (!lock || !path || !path[0]) return 1;
    memset(lock, 0, sizeof(*lock));

    #if defined(__OS_UNIX__)
        for (int attempt = 0; attempt < 3; attempt++) {
            FILE *file = fopen(path, "a+b");
            if (!file) return 1;
            if (os_file_lock(file) != 0) {
                fclose(file);
                return 1;
            }

            struct stat opened;
            struct stat current;
            if (fstat(fileno(file), &opened) == 0 && lstat(path, &current) == 0 &&
                opened.st_dev == current.st_dev && opened.st_ino == current.st_ino) {
                lock->path = strdup(path);
                if (!lock->path) {
                    fclose(file);
                    return 1;
                }
                lock->file = file;
                return 0;
            }
            fclose(file);
        }
        return 1;
    #elif defined(__OS_WINDOWS__)
        const char *base = strrchr(path, '/');
        const char *backslash = strrchr(path, '\\');
        if (!base || (backslash && backslash > base)) base = backslash;
        base = base ? base + 1 : path;
        char name[96];
        int n = snprintf(name, sizeof(name), "Local\\TCPerTransfer-%s", base);
        if (n < 0 || n >= (int)sizeof(name)) return 1;
        HANDLE handle = CreateMutexA(NULL, FALSE, name);
        if (!handle) return 1;
        DWORD status = WaitForSingleObject(handle, 0);
        if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
            CloseHandle(handle);
            return 1;
        }
        lock->handle = handle;
        return 0;
    #endif
}


void os_path_unlock(PathLock *lock) {
    if (!lock) return;
    #if defined(__OS_UNIX__)
        if (lock->file) {
            struct stat opened;
            struct stat current;
            if (lock->path && fstat(fileno(lock->file), &opened) == 0 &&
                lstat(lock->path, &current) == 0 && opened.st_dev == current.st_dev &&
                opened.st_ino == current.st_ino) remove(lock->path);
            fclose(lock->file);
        }
        free(lock->path);
        lock->file = NULL;
        lock->path = NULL;
    #elif defined(__OS_WINDOWS__)
        if (lock->handle) {
            ReleaseMutex(lock->handle);
            CloseHandle(lock->handle);
            lock->handle = NULL;
        }
    #endif
}

int os_path_rwlock(PathRWLock *lock, const char *path, int exclusive) {
    if (!lock || !path || !path[0]) return 1;
    memset(lock, 0, sizeof(*lock));

    #if defined(__OS_UNIX__)
        FILE *file = fopen(path, "a+b");
        if (!file) return 1;
        int operation = exclusive ? LOCK_EX : LOCK_SH;
        while (flock(fileno(file), operation) != 0) {
            if (errno == EINTR) continue;
            fclose(file);
            return 1;
        }
        lock->file = file;
        return 0;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN, NULL);
        if (handle == INVALID_HANDLE_VALUE) return 1;
        DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
        if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &lock->overlap)) {
            CloseHandle(handle);
            memset(lock, 0, sizeof(*lock));
            return 1;
        }
        lock->handle = handle;
        return 0;
    #endif
}


void os_path_rwunlock(PathRWLock *lock) {
    if (!lock) return;
    #if defined(__OS_UNIX__)
        if (lock->file) fclose(lock->file);
        lock->file = NULL;
    #elif defined(__OS_WINDOWS__)
        if (lock->handle && lock->handle != INVALID_HANDLE_VALUE) {
            UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &lock->overlap);
            CloseHandle(lock->handle);
        }
        memset(lock, 0, sizeof(*lock));
    #endif
}


char *os_getpwd(char *buffer, int size) {
    #if defined(__OS_UNIX__)
        return getcwd(buffer, size);
    #elif defined(__OS_WINDOWS__)
        return _getcwd(buffer, size);
    #endif
}


void os_getexec(char *buffer, int size) {
    if (size == 0) return;
    memset(buffer, 0, size);

    #if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
        if (GetModuleFileName(NULL, buffer, (DWORD)size) == 0) {
            fprintf(stderr, "Error getting module file name.\n");
            return;
        }
    #elif defined(__linux__)
        int n = readlink("/proc/self/exe", buffer, size - 1);
        if (n == -1) {
            fprintf(stderr, "Error reading /proc/self/exe.\n");
            return;
        }
        if (n > 0 && n < size) buffer[n] = '\0';
    #elif defined(__APPLE__)
        unsigned int uint32size = (unsigned int)size;
        if (_NSGetExecutablePath(buffer, &uint32size) != 0) {
            fprintf(stderr, "Buffer too small; need size %u\n", uint32size);
            return;
        }
    #endif

    #if defined(__OS_UNIX__)
        char *last_slash = strrchr(buffer, '/');
    #elif defined(__OS_WINDOWS__)
        char *last_slash = strrchr(buffer, '\\');
    #endif

    if (last_slash != NULL) *last_slash = '\0';
    else fprintf(stderr, "Warning: No path separator found.\n");
}


void os_srand(void) {
    unsigned int i = (unsigned int)time(NULL);
    srand(i);
    OS_SEED = (((long long int)i) << 16) | rand();
}


double os_random(double low, double high) {
    OS_SEED = (0x5DEECE66DLL * OS_SEED + 0xBLL) & 0xFFFFFFFFFFFFLL;
    return low + (high - low) * ((double)(OS_SEED >> 16) / (double)0x100000000LL);
}


int os_random_bytes(void *buffer, usize length) {
    if (!buffer && length > 0) return 1;
    #if defined(__OS_UNIX__)
        FILE *fp = fopen("/dev/urandom", "rb");
        if (!fp) return 1;
        usize n = fread(buffer, 1, length, fp);
        fclose(fp);
        return n == length ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        byte *p = (byte *)buffer;
        for (usize i = 0; i < length; i++) {
            unsigned int value;
            if (rand_s(&value) != 0) return 1;
            p[i] = (byte)value;
        }
        return 0;
    #endif
}


int64 os_filesize(char *filepath) {
    #if defined(__OS_UNIX__)
        struct stat file;
        if (stat(filepath, &file) == -1) return -1;
        return (int64)file.st_size;
    #elif defined(__OS_WINDOWS__)
        struct _stat64 file;
        if (_stat64(filepath, &file) == -1) return -1;
        return (int64)file.st_size;
    #endif
}


int64 os_file_size(FILE *file) {
    if (!file) return -1;
    #if defined(__OS_UNIX__)
        struct stat status;
        if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode)) return -1;
        return (int64)status.st_size;
    #elif defined(__OS_WINDOWS__)
        struct _stat64 status;
        if (_fstat64(_fileno(file), &status) != 0 ||
            (status.st_mode & _S_IFMT) != _S_IFREG) return -1;
        return (int64)status.st_size;
    #endif
}


int os_file_is_path(FILE *file, const char *path) {
    if (!file || !path) return 0;
    #if defined(__OS_UNIX__)
        struct stat opened;
        struct stat current;
        return fstat(fileno(file), &opened) == 0 && stat(path, &current) == 0 &&
            opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
    #elif defined(__OS_WINDOWS__)
        intptr_t descriptor = _get_osfhandle(_fileno(file));
        if (descriptor == -1) return 0;
        HANDLE current = CreateFileA(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (current == INVALID_HANDLE_VALUE) return 0;
        BY_HANDLE_FILE_INFORMATION opened_info;
        BY_HANDLE_FILE_INFORMATION current_info;
        int same = GetFileInformationByHandle((HANDLE)descriptor, &opened_info) &&
            GetFileInformationByHandle(current, &current_info) &&
            opened_info.dwVolumeSerialNumber == current_info.dwVolumeSerialNumber &&
            opened_info.nFileIndexHigh == current_info.nFileIndexHigh &&
            opened_info.nFileIndexLow == current_info.nFileIndexLow;
        CloseHandle(current);
        return same;
    #endif
}


MapFile *os_mmap(char *filepath, usize length) {
    MapFile *f = malloc(sizeof(*f));
    if (!f) return NULL;
    f->size = length;

    #if defined(__OS_UNIX__)
        f->fd = open(filepath, O_RDONLY);
        if (f->fd == -1) {
            free(f);
            return NULL;
        }
        f->data = mmap(NULL, length, PROT_READ, MAP_PRIVATE, f->fd, 0);
        if (f->data == MAP_FAILED) {
            close(f->fd);
            free(f);
            return NULL;
        }
    #elif defined(__OS_WINDOWS__)
        f->hFile = CreateFile(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f->hFile == INVALID_HANDLE_VALUE) {
            free(f);
            return NULL;
        }
        f->hMapping = CreateFileMapping(f->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (f->hMapping == NULL) {
            CloseHandle(f->hFile);
            free(f);
            return NULL;
        }
        f->data = MapViewOfFile(f->hMapping, FILE_MAP_READ, 0, 0, length);
        if (f->data == NULL) {
            CloseHandle(f->hMapping);
            CloseHandle(f->hFile);
            free(f);
            return NULL;
        }
    #endif
    return f;
}


void os_munmap(MapFile *f) {
    if (!f) return;
    #if defined(__OS_UNIX__)
        munmap(f->data, f->size);
        close(f->fd);
    #elif defined(__OS_WINDOWS__)
        UnmapViewOfFile(f->data);
        CloseHandle(f->hMapping);
        CloseHandle(f->hFile);
    #endif
    free(f);
}


int os_listdir(char *path, _ListdirCallback func, void *args) {
    if (!path || !func) return 1;
    #if defined(__OS_UNIX__)
        DIR *dir = opendir(path);
        if (!dir) return 1;
        int status = 0;
        struct dirent *p;
        while (1) {
            errno = 0;
            p = readdir(dir);
            int read_error = errno;
            if (!p) {
                if (read_error != 0) status = 1;
                break;
            }
            if (strcmp(p->d_name, ".") != 0 && strcmp(p->d_name, "..") != 0) {
                bool folder;
                uint64 size;
                char full_path[4096];
                struct stat st;
                int n = snprintf(full_path, sizeof(full_path), "%s/%s", path, p->d_name);
                if (n < 0 || n >= (int)sizeof(full_path) || lstat(full_path, &st) != 0 || S_ISLNK(st.st_mode)) {
                    status = 1;
                    continue;
                }
                if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
                    status = 1;
                    continue;
                }
                folder = S_ISDIR(st.st_mode) ? True : False;
                size = (uint64)st.st_size;
                func(path, p->d_name, folder, size, args);
            }
        }
        if (closedir(dir) != 0) status = 1;
        return status;
    #elif defined(__OS_WINDOWS__)
        WIN32_FIND_DATA f;
        char dir[4096];
        if (snprintf(dir, sizeof(dir), "%s\\*", path) < 0) return 1;
        HANDLE h = FindFirstFile(dir, &f);
        if (h == INVALID_HANDLE_VALUE) return 1;
        int status = 0;
        do {
            if (strcmp(f.cFileName, ".") != 0 && strcmp(f.cFileName, "..") != 0) {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    status = 1;
                    continue;
                }
                bool folder = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? True : False;
                uint64 size = ((uint64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
                func(path, f.cFileName, folder, size, args);
            }
        } while (FindNextFile(h, &f));
        if (GetLastError() != ERROR_NO_MORE_FILES || !FindClose(h)) status = 1;
        return status;
    #endif
}


bool os_traversal(char *path) {
    if (path == NULL) return False;
    usize i = 0;
    usize len = strlen(path);
    while (i < len) {
        while (i < len && (path[i] == '/' || path[i] == '\\')) i++;
        if (i >= len) break;
        usize dot_count = 0;
        bool only_dots = True;
        while (i < len && !(path[i] == '/' || path[i] == '\\')) {
            if (path[i] == '.') dot_count++;
            else only_dots = False;
            i++;
        }
        if (only_dots && dot_count >= 2) return True;
    }
    return False;
}


#if defined(__OS_UNIX__)
static int __os_remove_directory_fd__(int directory_fd) {
    DIR *directory = fdopendir(directory_fd);
    if (!directory) {
        close(directory_fd);
        return 1;
    }

    int result = 0;
    int parent_fd = dirfd(directory);
    while (1) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno != 0) result = 1;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        struct stat expected;
        if (fstatat(parent_fd, entry->d_name, &expected, AT_SYMLINK_NOFOLLOW) != 0) {
            result = 1;
            continue;
        }
        if (!S_ISDIR(expected.st_mode)) {
            if (unlinkat(parent_fd, entry->d_name, 0) != 0) result = 1;
            continue;
        }

        int flags = O_RDONLY | O_DIRECTORY;
        #ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
        #endif
        int child_fd = openat(parent_fd, entry->d_name, flags);
        struct stat opened;
        if (child_fd < 0 || fstat(child_fd, &opened) != 0 ||
            opened.st_dev != expected.st_dev || opened.st_ino != expected.st_ino) {
            if (child_fd >= 0) close(child_fd);
            result = 1;
            continue;
        }
        if (__os_remove_directory_fd__(child_fd) != 0 ||
            unlinkat(parent_fd, entry->d_name, AT_REMOVEDIR) != 0) result = 1;
    }
    if (closedir(directory) != 0) result = 1;
    return result;
}
#endif


int os_remove(char *path) {
    if (!path || !path[0]) return 1;
    char base[4096];
    int len = snprintf(base, sizeof(base), "%s", path);
    if (len < 0 || len >= (int)sizeof(base)) return 1;
    int self = 1;

    if (len >= 2 && base[len - 1] == '.' && (base[len - 2] == '/' || base[len - 2] == '\\')) {
        base[len - 2] = '\0';
        self = 0;
    }

    #if defined(__OS_UNIX__)
        struct stat expected;
        if (lstat(base, &expected) != 0) return 1;
        if (S_ISLNK(expected.st_mode)) return self ? remove(base) : 1;
        if (!S_ISDIR(expected.st_mode)) return self ? remove(base) : 1;

        int flags = O_RDONLY | O_DIRECTORY;
        #ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
        #endif
        int directory_fd = open(base, flags);
        struct stat opened;
        if (directory_fd < 0 || fstat(directory_fd, &opened) != 0 ||
            opened.st_dev != expected.st_dev || opened.st_ino != expected.st_ino) {
            if (directory_fd >= 0) close(directory_fd);
            return 1;
        }
        int result = __os_remove_directory_fd__(directory_fd);
        if (self) {
            struct stat current;
            if (lstat(base, &current) != 0 || current.st_dev != opened.st_dev ||
                current.st_ino != opened.st_ino || remove(base) != 0) result = 1;
        }
        return result;
    #elif defined(__OS_WINDOWS__)
        char sub[4096];
        DWORD base_attrs = GetFileAttributesA(base);
        if (base_attrs == INVALID_FILE_ATTRIBUTES) return 1;
        if (!(base_attrs & FILE_ATTRIBUTE_DIRECTORY)) return remove(base);
        if (base_attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            return self ? (RemoveDirectoryA(base) ? 0 : 1) : 0;
        }

        WIN32_FIND_DATAA fd;
        int pattern_length = snprintf(sub, sizeof(sub), "%s\\*", base);
        if (pattern_length < 0 || pattern_length >= (int)sizeof(sub)) return 1;
        HANDLE h = FindFirstFileA(sub, &fd);
        if (h == INVALID_HANDLE_VALUE) return 1;
        int result = 0;
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                int n = snprintf(sub, sizeof(sub), "%s\\%s", base, fd.cFileName);
                if (n < 0 || n >= (int)sizeof(sub)) {
                    result = 1;
                    continue;
                }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    int status = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ?
                        (RemoveDirectoryA(sub) ? 0 : 1) : os_remove(sub);
                    if (status != 0) result = 1;
                } else if (remove(sub) != 0) result = 1;
            }
        } while (FindNextFileA(h, &fd));
        if (GetLastError() != ERROR_NO_MORE_FILES) result = 1;
        if (!FindClose(h)) result = 1;
        if (self && !RemoveDirectoryA(base)) result = 1;
        return result;
    #endif

    return 1;
}

int os_file_lock(FILE *file) {
    if (!file) return 1;
    #if defined(__OS_UNIX__)
        return flock(fileno(file), LOCK_EX | LOCK_NB) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
        if (handle == INVALID_HANDLE_VALUE) return 1;
        OVERLAPPED overlap = {0};
        return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, MAXDWORD, MAXDWORD, &overlap) ? 0 : 1;
    #endif
}

int os_file_sync(FILE *file) {
    if (!file || fflush(file) != 0) return 1;
    #if defined(__OS_UNIX__)
        return fsync(fileno(file)) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        intptr_t descriptor = _get_osfhandle(_fileno(file));
        if (descriptor == -1) return 1;
        return FlushFileBuffers((HANDLE)descriptor) ? 0 : 1;
    #endif
}


int os_file_lock_wait(FILE *file) {
    if (!file) return 1;
    #if defined(__OS_UNIX__)
        return flock(fileno(file), LOCK_EX) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
        if (handle == INVALID_HANDLE_VALUE) return 1;
        OVERLAPPED overlap = {0};
        return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlap) ? 0 : 1;
    #endif
}


void os_file_unlock(FILE *file) {
    if (!file) return;
    #if defined(__OS_UNIX__)
        flock(fileno(file), LOCK_UN);
    #elif defined(__OS_WINDOWS__)
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
        if (handle == INVALID_HANDLE_VALUE) return;
        OVERLAPPED overlap = {0};
        UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlap);
    #endif
}


int os_rename_replace(const char *source, const char *destination) {
    if (!source || !destination) return 1;
    #if defined(__OS_UNIX__)
        return rename(source, destination);
    #elif defined(__OS_WINDOWS__)
        return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING) ? 0 : 1;
    #endif
}


int os_hardlink(const char *source, const char *destination) {
    if (!source || !destination) return 1;
    #if defined(__OS_UNIX__)
        return link(source, destination) == 0 ? 0 : 1;
    #elif defined(__OS_WINDOWS__)
        return CreateHardLinkA(destination, source, NULL) ? 0 : 1;
    #endif
}
