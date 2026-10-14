#include "app.h"

#include <ctype.h>
#include <stdatomic.h>

static int REMOTE_HOST_PORT = 0;
static _Atomic int RUNNING = 1;
static _Atomic(Socket) LISTENER = SOCKET_INVALID;
static char REMOTE_HOST_IPV4[32] = "0.0.0.0";
static char SERVER_KEY[PROTOCOL_KEY_MAX_LENGTH + 1];

#define ACTIVE_CLIENT_LIMIT 8
static Mutex ACTIVE_CLIENTS_LOCK;
static Socket ACTIVE_CLIENTS[ACTIVE_CLIENT_LIMIT];
static Mutex UPLOAD_QUOTA_LOCK;
static bool UPLOAD_QUOTA_READY = False;
static uint64 UPLOAD_QUOTA_FILES = 0;
static uint64 UPLOAD_QUOTA_BYTES = 0;
static uint64 DIRECTORY_STAGING_FILES = 0;
static uint64 DIRECTORY_STAGING_BYTES = 0;
static Mutex STORAGE_QUOTA_LOCK;
static bool STORAGE_QUOTA_READY = False;
static uint64 STORAGE_FILES = 0;
static uint64 STORAGE_BYTES = 0;
static uint64 STORAGE_RESERVED_FILES = 0;
static uint64 STORAGE_RESERVED_BYTES = 0;

#if defined(__OS_WINDOWS__)
static bool __storage_windows_open_checked__(const char *path, DWORD access, DWORD creation,
                                             HANDLE *output);
#endif

/* Keep private transfer state flat and outside the published remote namespace. */
#define STORAGE_STATE_DIRECTORY "./.tcper-state"
#define STORAGE_TREE_LOCK_FILE "./.tcper-state/storage-tree.lock"
#define UPLOAD_STATE_LOCK_FILE "./.tcper-state/upload-state.lock"
#define PATH_LOCK_STRIPES 64

typedef struct {
    PathRWLock locks[PATH_LOCK_STRIPES];
    usize count;
} _StoragePathLock;

static bool __safe_relative_path__(const char *path, bool allow_root) {
    if (!path || !path[0]) return allow_root;
    if ((allow_root && strcmp(path, ".") == 0)) return True;
    if (path[0] == '/' || path[0] == '\\') return False;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return False;

    const char *segment = path;
    for (const char *p = path; ; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 && ch != '\0') return False;
        if (ch == ':') return False;
        if (ch == '/' || ch == '\\' || ch == '\0') {
            usize length = (usize)(p - segment);
            if (length == 0 || (length == 2 && segment[0] == '.' && segment[1] == '.') ||
                (length == 1 && segment[0] == '.')) return False;
            if (ch == '\0') break;
            segment = p + 1;
        }
    }
    return True;
}

static bool __storage_path__(char *output, usize capacity, const char *remote, bool allow_root) {
    if (!__safe_relative_path__(remote, allow_root)) return False;
    int n;
    if (allow_root && (!remote[0] || strcmp(remote, ".") == 0)) n = snprintf(output, capacity, "./tcp-file-storage");
    else n = snprintf(output, capacity, "./tcp-file-storage/%s", remote);
    if (n < 0 || n >= (int)capacity) return False;
    for (char *p = output; *p; p++) if (*p == '\\') *p = '/';

    /* Reject pre-existing links in every component of a client-controlled path. */
    char current[PATH_LENGTH_RESTRICTION];
    int current_length = snprintf(current, sizeof(current), ".");
    if (current_length < 0 || current_length >= (int)sizeof(current)) return False;
    const char *segment = output + 2;
    while (*segment) {
        const char *end = strchr(segment, '/');
        usize length = end ? (usize)(end - segment) : strlen(segment);
        int appended = snprintf(current + current_length, sizeof(current) - (usize)current_length,
            "/%.*s", (int)length, segment);
        if (appended < 0 || appended >= (int)(sizeof(current) - (usize)current_length)) return False;
        current_length += appended;

        #if defined(__OS_UNIX__)
            struct stat st;
            int status = lstat(current, &st);
            if (status == 0 && S_ISLNK(st.st_mode)) return False;
            if (status != 0 && errno != ENOENT) return False;
        #elif defined(__OS_WINDOWS__)
            DWORD attrs = GetFileAttributesA(current);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return False;
        #endif
        if (!end) break;
        segment = end + 1;
    }
    return True;
}

static bool __checked_directory__(const char *path) {
    if (os_mkdir((char *)path) != 0 || !os_isdir((char *)path)) return False;
    #if defined(__OS_UNIX__)
        struct stat st;
        return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    #elif defined(__OS_WINDOWS__)
        DWORD attrs = GetFileAttributesA(path);
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
    #endif
}

/* State holds resumable payloads, so do not trust a pre-existing shared directory. */
static bool __checked_state_directory__(const char *path) {
    #if defined(__OS_UNIX__)
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return False;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
            st.st_uid != getuid()) return False;
        if ((st.st_mode & 0077) != 0 && chmod(path, 0700) != 0) return False;
        return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
            st.st_uid == getuid() && (st.st_mode & 0077) == 0;
    #elif defined(__OS_WINDOWS__)
        return __checked_directory__(path);
    #endif
}

/* Return 1 for a safe checkpoint, 0 when absent, and -1 for an invalid entry. */
static int __state_checkpoint_size__(const char *path, int64 *size) {
    if (!path || !size) return -1;
    *size = -1;
    #if defined(__OS_UNIX__)
        struct stat st;
        if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) return -1;
        *size = (int64)st.st_size;
        return 1;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
        if (!__storage_windows_open_checked__(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING,
                                              &handle)) {
            return os_access((char *)path) == 0 ? 0 : -1;
        }
        LARGE_INTEGER length;
        bool ok = GetFileSizeEx(handle, &length) && length.QuadPart >= 0;
        CloseHandle(handle);
        if (!ok) return -1;
        *size = (int64)length.QuadPart;
        return 1;
    #endif
}

static bool __state_open_upload_checkpoint__(const char *path, FILE **file) {
    if (!path || !file) return False;
    *file = NULL;
    #if defined(__OS_UNIX__)
        int descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
        struct stat st;
        if (descriptor < 0 || fstat(descriptor, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_nlink != 1 || fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
            if (descriptor >= 0) close(descriptor);
            return False;
        }
        *file = fdopen(descriptor, "r+b");
        if (!*file) close(descriptor);
        return *file != NULL;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
        if (!__storage_windows_open_checked__(path, GENERIC_READ | GENERIC_WRITE,
                                              OPEN_ALWAYS, &handle)) return False;
        int descriptor = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(handle);
            return False;
        }
        *file = _fdopen(descriptor, "r+b");
        if (!*file) _close(descriptor);
        return *file != NULL;
    #endif
}

typedef struct {
    #if defined(__OS_UNIX__)
        int fd;
    #elif defined(__OS_WINDOWS__)
        HANDLE handle;
    #endif
} _StorageDirectory;

typedef struct {
    #if defined(__OS_UNIX__)
        int directory_fd;
    #endif
    char leaf[PATH_LENGTH_RESTRICTION];
} _StorageParent;

static void __storage_directory_close__(_StorageDirectory *directory) {
    if (!directory) return;
    #if defined(__OS_UNIX__)
        if (directory->fd >= 0) close(directory->fd);
        directory->fd = -1;
    #elif defined(__OS_WINDOWS__)
        if (directory->handle && directory->handle != INVALID_HANDLE_VALUE)
            CloseHandle(directory->handle);
        directory->handle = INVALID_HANDLE_VALUE;
    #endif
}

#if defined(__OS_WINDOWS__)
static bool __storage_windows_open_checked__(const char *path, DWORD access, DWORD creation,
                                             HANDLE *output) {
    HANDLE handle = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, NULL, creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        NULL);
    if (handle == INVALID_HANDLE_VALUE) return False;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle);
        return False;
    }
    *output = handle;
    return True;
}
#endif

/* Resolve every untrusted component relative to the already-open storage root. */
static bool __storage_open_parent__(const char *remote, bool create, _StorageParent *parent) {
    if (!parent || !__safe_relative_path__(remote, False)) return False;
    memset(parent, 0, sizeof(*parent));

    char work[PATH_LENGTH_RESTRICTION];
    int n = snprintf(work, sizeof(work), "%s", remote);
    if (n <= 0 || n >= (int)sizeof(work) || work[n - 1] == '/' || work[n - 1] == '\\')
        return False;

    #if defined(__OS_UNIX__)
        int directory_fd = open("./tcp-file-storage", O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                O_NOFOLLOW);
        if (directory_fd < 0) return False;
        char *segment = work;
        while (1) {
            char *separator = strpbrk(segment, "/\\");
            if (!separator) break;
            *separator = '\0';
            if (!segment[0]) {
                close(directory_fd);
                return False;
            }
            int child_fd = openat(directory_fd, segment,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child_fd < 0 && errno == ENOENT && create &&
                mkdirat(directory_fd, segment, 0755) == 0) {
                child_fd = openat(directory_fd, segment,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            }
            struct stat status;
            if (child_fd < 0 || fstat(child_fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
                if (child_fd >= 0) close(child_fd);
                close(directory_fd);
                return False;
            }
            close(directory_fd);
            directory_fd = child_fd;
            segment = separator + 1;
        }
        if (!segment[0]) {
            close(directory_fd);
            return False;
        }
        parent->directory_fd = directory_fd;
    #elif defined(__OS_WINDOWS__)
        char current[PATH_LENGTH_RESTRICTION];
        int current_length = snprintf(current, sizeof(current), "./tcp-file-storage");
        if (current_length < 0 || current_length >= (int)sizeof(current)) return False;
        char *segment = work;
        while (1) {
            char *separator = strpbrk(segment, "/\\");
            if (!separator) break;
            *separator = '\0';
            int appended = snprintf(current + current_length,
                sizeof(current) - (usize)current_length, "/%s", segment);
            if (!segment[0] || appended < 0 ||
                appended >= (int)(sizeof(current) - (usize)current_length)) return False;
            current_length += appended;
            HANDLE handle;
            if (!__storage_windows_open_checked__(current, FILE_LIST_DIRECTORY,
                                                  OPEN_EXISTING, &handle)) {
                if (!create || os_mkdir(current) != 0 ||
                    !__storage_windows_open_checked__(current, FILE_LIST_DIRECTORY,
                                                      OPEN_EXISTING, &handle)) return False;
            }
            CloseHandle(handle);
            segment = separator + 1;
        }
    #endif

    n = snprintf(parent->leaf, sizeof(parent->leaf), "%s", segment);
    if (n <= 0 || n >= (int)sizeof(parent->leaf)) {
        #if defined(__OS_UNIX__)
            close(parent->directory_fd);
        #endif
        return False;
    }
    return True;
}

static void __storage_parent_close__(_StorageParent *parent) {
    if (!parent) return;
    #if defined(__OS_UNIX__)
        if (parent->directory_fd >= 0) close(parent->directory_fd);
        parent->directory_fd = -1;
    #endif
}

static bool __storage_ensure_parents__(const char *remote) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    bool ok = __storage_open_parent__(remote, True, &parent);
    __storage_parent_close__(&parent);
    return ok;
}

static int __storage_entry_state__(const char *remote) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, False, &parent)) return -1;
    int result = -1;
    #if defined(__OS_UNIX__)
        struct stat status;
        if (fstatat(parent.directory_fd, parent.leaf, &status, AT_SYMLINK_NOFOLLOW) == 0)
            result = S_ISLNK(status.st_mode) ? -1 : 1;
        else if (errno == ENOENT) result = 0;
    #elif defined(__OS_WINDOWS__)
        char path[PATH_LENGTH_RESTRICTION];
        if (__storage_path__(path, sizeof(path), remote, False)) {
            HANDLE handle;
            if (__storage_windows_open_checked__(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING,
                                                 &handle)) {
                CloseHandle(handle);
                result = 1;
            } else {
                DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) result = 0;
            }
        }
    #endif
    __storage_parent_close__(&parent);
    return result;
}

static bool __storage_open_regular__(const char *remote, FILE **file, int64 *size) {
    if (!file || !size) return False;
    *file = NULL;
    *size = -1;
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, False, &parent)) return False;
    bool ok = False;
    #if defined(__OS_UNIX__)
        int descriptor = openat(parent.directory_fd, parent.leaf,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat status;
        if (descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode)) {
            FILE *opened = fdopen(descriptor, "rb");
            if (opened) {
                *file = opened;
                *size = (int64)status.st_size;
                ok = True;
            } else close(descriptor);
        } else if (descriptor >= 0) close(descriptor);
    #elif defined(__OS_WINDOWS__)
        char path[PATH_LENGTH_RESTRICTION];
        HANDLE handle;
        if (__storage_path__(path, sizeof(path), remote, False) &&
            __storage_windows_open_checked__(path, GENERIC_READ, OPEN_EXISTING, &handle)) {
            int descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
            if (descriptor >= 0) {
                FILE *opened = _fdopen(descriptor, "rb");
                if (opened) {
                    *file = opened;
                    *size = os_file_size(opened);
                    ok = *size >= 0;
                    if (!ok) fclose(opened);
                } else _close(descriptor);
            } else CloseHandle(handle);
        }
    #endif
    __storage_parent_close__(&parent);
    return ok;
}

static bool __storage_open_directory__(const char *remote, _StorageDirectory *directory) {
    if (!directory || !__safe_relative_path__(remote, True)) return False;
    #if defined(__OS_UNIX__)
        directory->fd = open("./tcp-file-storage", O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                             O_NOFOLLOW);
        if (directory->fd < 0) return False;
        if (!remote[0] || strcmp(remote, ".") == 0) return True;
        char work[PATH_LENGTH_RESTRICTION];
        int n = snprintf(work, sizeof(work), "%s", remote);
        if (n <= 0 || n >= (int)sizeof(work)) {
            __storage_directory_close__(directory);
            return False;
        }
        char *segment = work;
        while (segment) {
            char *separator = strpbrk(segment, "/\\");
            if (separator) *separator = '\0';
            if (!segment[0]) {
                __storage_directory_close__(directory);
                return False;
            }
            int child = openat(directory->fd, segment,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                __storage_directory_close__(directory);
                return False;
            }
            close(directory->fd);
            directory->fd = child;
            segment = separator ? separator + 1 : NULL;
        }
        return True;
    #elif defined(__OS_WINDOWS__)
        char path[PATH_LENGTH_RESTRICTION];
        if (!__storage_path__(path, sizeof(path), remote, True)) return False;
        return __storage_windows_open_checked__(path, FILE_LIST_DIRECTORY, OPEN_EXISTING,
                                                &directory->handle);
    #endif
}

static bool __storage_rename_into__(const char *source, const char *remote) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, False, &parent)) return False;
    bool ok;
    #if defined(__OS_UNIX__)
        ok = renameat(AT_FDCWD, source, parent.directory_fd, parent.leaf) == 0;
    #elif defined(__OS_WINDOWS__)
        char destination[PATH_LENGTH_RESTRICTION];
        ok = __storage_path__(destination, sizeof(destination), remote, False) &&
            os_rename_replace(source, destination) == 0;
    #endif
    __storage_parent_close__(&parent);
    return ok;
}

static bool __storage_rename_out__(const char *remote, const char *destination) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, False, &parent)) return False;
    bool ok;
    #if defined(__OS_UNIX__)
        ok = renameat(parent.directory_fd, parent.leaf, AT_FDCWD, destination) == 0;
    #elif defined(__OS_WINDOWS__)
        char source[PATH_LENGTH_RESTRICTION];
        ok = __storage_path__(source, sizeof(source), remote, False) &&
            os_rename_replace(source, destination) == 0;
    #endif
    __storage_parent_close__(&parent);
    return ok;
}

#if defined(__OS_UNIX__)
static int __storage_remove_at__(int directory_fd, const char *name) {
    struct stat status;
    if (fstatat(directory_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0) return 1;
    if (!S_ISDIR(status.st_mode)) return unlinkat(directory_fd, name, 0) == 0 ? 0 : 1;
    int child_fd = openat(directory_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_fd < 0) return 1;
    DIR *directory = fdopendir(child_fd);
    if (!directory) {
        close(child_fd);
        return 1;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            __storage_remove_at__(child_fd, entry->d_name) != 0) result = 1;
    }
    if (closedir(directory) != 0) result = 1;
    return result == 0 && unlinkat(directory_fd, name, AT_REMOVEDIR) == 0 ? 0 : 1;
}
#endif

static bool __storage_remove__(const char *remote) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, False, &parent)) return False;
    bool ok;
    #if defined(__OS_UNIX__)
        ok = __storage_remove_at__(parent.directory_fd, parent.leaf) == 0;
    #elif defined(__OS_WINDOWS__)
        char path[PATH_LENGTH_RESTRICTION];
        ok = __storage_path__(path, sizeof(path), remote, False) && os_remove(path) == 0;
    #endif
    __storage_parent_close__(&parent);
    return ok;
}

static bool __storage_mkdir__(const char *remote) {
    _StorageParent parent = {0};
    #if defined(__OS_UNIX__)
        parent.directory_fd = -1;
    #endif
    if (!__storage_open_parent__(remote, True, &parent)) return False;
    bool ok = False;
    #if defined(__OS_UNIX__)
        if (mkdirat(parent.directory_fd, parent.leaf, 0755) == 0 || errno == EEXIST) {
            struct stat status;
            ok = fstatat(parent.directory_fd, parent.leaf, &status, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(status.st_mode);
        }
    #elif defined(__OS_WINDOWS__)
        char path[PATH_LENGTH_RESTRICTION];
        ok = __storage_path__(path, sizeof(path), remote, False) && __checked_directory__(path);
    #endif
    __storage_parent_close__(&parent);
    return ok;
}

static int __storage_list_directory__(const char *remote, _ListdirCallback callback, void *args) {
    if (!callback) return 1;
    _StorageDirectory storage = {0};
    #if defined(__OS_UNIX__)
        storage.fd = -1;
    #elif defined(__OS_WINDOWS__)
        storage.handle = INVALID_HANDLE_VALUE;
    #endif
    if (!__storage_open_directory__(remote, &storage)) return 1;
    #if defined(__OS_UNIX__)
        DIR *directory = fdopendir(storage.fd);
        if (!directory) {
            __storage_directory_close__(&storage);
            return 1;
        }
        storage.fd = -1;  /* closedir owns the descriptor after fdopendir. */
        int result = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            struct stat status;
            if (fstatat(dirfd(directory), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
                S_ISLNK(status.st_mode) || (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode))) {
                result = 1;
                continue;
            }
            callback(NULL, entry->d_name, S_ISDIR(status.st_mode), (uint64)status.st_size, args);
        }
        if (closedir(directory) != 0) result = 1;
        return result;
    #elif defined(__OS_WINDOWS__)
        __storage_directory_close__(&storage);
        char path[PATH_LENGTH_RESTRICTION];
        return __storage_path__(path, sizeof(path), remote, True) ? os_listdir(path, callback, args) : 1;
    #endif
}

static bool __ensure_parent_dirs_under__(char *path, const char *root_prefix) {
    char parent[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(parent, sizeof(parent), "%s", path);
    if (n < 0 || n >= (int)sizeof(parent)) return False;

    usize prefix_length = strlen(root_prefix);
    if (strncmp(parent, root_prefix, prefix_length) != 0) return False;
    for (char *p = parent + prefix_length; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!__checked_directory__(parent)) return False;
        *p = '/';
    }
    return True;
}

static bool __storage_tree_lock__(PathRWLock *lock, bool exclusive) {
    return os_path_rwlock(lock, STORAGE_TREE_LOCK_FILE, exclusive ? 1 : 0) == 0;
}

static unsigned int __path_lock_stripe__(const char *path, usize length) {
    SHA256Context context;
    byte digest[SHA256_DIGEST_SIZE];
    sha256_init(&context);
    sha256_update(&context, (unsigned char *)path, (unsigned int)length);
    sha256_final(&context, digest);
    return digest[0] % PATH_LOCK_STRIPES;
}

static bool __storage_path_lock__(_StoragePathLock *lock, const char *path) {
    static const char root[] = "./tcp-file-storage";
    usize root_length = sizeof(root) - 1;
    if (!lock || !path || strncmp(path, root, root_length) != 0 ||
        (path[root_length] != '\0' && path[root_length] != '/')) return False;
    memset(lock, 0, sizeof(*lock));

    bool selected[PATH_LOCK_STRIPES] = {0};
    bool exclusive[PATH_LOCK_STRIPES] = {0};
    usize path_length = strlen(path);
    selected[__path_lock_stripe__(path, root_length)] = True;
    for (usize i = root_length + 1; i <= path_length; i++) {
        if (i != path_length && path[i] != '/') continue;
        unsigned int stripe = __path_lock_stripe__(path, i);
        selected[stripe] = True;
        if (i == path_length) exclusive[stripe] = True;
    }

    for (unsigned int stripe = 0; stripe < PATH_LOCK_STRIPES; stripe++) {
        if (!selected[stripe]) continue;
        char lock_path[128];
        int length = snprintf(lock_path, sizeof(lock_path),
                              STORAGE_STATE_DIRECTORY "/path-%02x.lock", stripe);
        if (length < 0 || length >= (int)sizeof(lock_path) ||
            os_path_rwlock(&lock->locks[lock->count], lock_path,
                           exclusive[stripe] ? 1 : 0) != 0) {
            while (lock->count > 0)
                os_path_rwunlock(&lock->locks[--lock->count]);
            return False;
        }
        lock->count++;
    }
    return True;
}

static void __storage_path_unlock__(_StoragePathLock *lock) {
    if (!lock) return;
    while (lock->count > 0) os_path_rwunlock(&lock->locks[--lock->count]);
}

static bool __storage_open_regular__(const char *remote, FILE **file, int64 *size);
static bool __storage_mkdir__(const char *remote);
static bool __storage_remove__(const char *remote);

static bool __transfer_lock_path__(char *output, usize capacity, const char *key) {
    char canonical[PATH_LENGTH_RESTRICTION * 2];
    if (os_canonical_path(canonical, sizeof(canonical), key) != 0) return False;
    char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
    sha256_string(canonical, hash);
    return os_transfer_lock_path(output, capacity, hash) == 0;
}
static bool __checkpoint_path__(char *output, usize capacity, const char *remote,
                                const char *sha256) {
    if (!sha256 || strlen(sha256) != PROTOCOL_FILE_HASH_LENGTH ||
        strspn(sha256, "0123456789abcdefABCDEF") != PROTOCOL_FILE_HASH_LENGTH)
        return False;
    char storage_path[PATH_LENGTH_RESTRICTION];
    char canonical[PATH_LENGTH_RESTRICTION * 2];
    if (!__storage_path__(storage_path, sizeof(storage_path), remote, False) ||
        os_canonical_path(canonical, sizeof(canonical), storage_path) != 0) return False;

    SHA256Context context;
    byte digest[SHA256_DIGEST_SIZE];
    char identity[PROTOCOL_FILE_HASH_LENGTH + 1];
    byte separator = 0;
    sha256_init(&context);
    sha256_update(&context, canonical, strlen(canonical));
    sha256_update(&context, &separator, sizeof(separator));
    sha256_update(&context, (void *)sha256, strlen(sha256));
    sha256_final(&context, digest);
    app_hex_encode(digest, sizeof(digest), identity);

    int n = snprintf(output, capacity, STORAGE_STATE_DIRECTORY "/upload-%s.part", identity);
    return n >= 0 && n < (int)capacity;
}

typedef struct {
    uint64 files;
    uint64 directories;
    uint64 bytes;
    bool failed;
} _StorageUsage;

typedef struct {
    bool active;
    uint64 old_files;
    uint64 old_bytes;
    uint64 new_files;
    uint64 new_bytes;
    uint64 reserved_files;
    uint64 reserved_bytes;
} _StorageQuotaReservation;

static void __measure_storage_usage__(char *dir, char *name, bool folder, uint64 size,
                                      void *args) {
    _StorageUsage *usage = (_StorageUsage *)args;
    if (!usage || usage->failed) return;
    char path[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path)) {
        usage->failed = True;
        return;
    }
    if (folder) {
        if (usage->directories == UINT64_MAX ||
            os_listdir(path, __measure_storage_usage__, usage) != 0) {
            usage->failed = True;
            return;
        }
        usage->directories++;
        return;
    }
    if (usage->files == UINT64_MAX || UINT64_MAX - usage->bytes < size) {
        usage->failed = True;
        return;
    }
    usage->files++;
    usage->bytes += size;
}

/* The root itself is not counted, so replacing one directory is a pure delta of its contents. */
static bool __measure_storage_path__(const char *path, _StorageUsage *usage) {
    if (!path || !usage) return False;
    memset(usage, 0, sizeof(*usage));
    if (os_isfile((char *)path)) {
        int64 size = os_filesize((char *)path);
        if (size < 0) return False;
        usage->files = 1;
        usage->bytes = (uint64)size;
        return True;
    }
    if (!os_isdir((char *)path)) return False;
    return os_listdir((char *)path, __measure_storage_usage__, usage) == 0 && !usage->failed;
}

static bool __measure_remote_storage__(const char *remote, _StorageUsage *usage) {
    char path[PATH_LENGTH_RESTRICTION];
    return __storage_path__(path, sizeof(path), remote, False) &&
        __measure_storage_path__(path, usage);
}

static bool __storage_quota_allows__(uint64 used, uint64 reserved, uint64 additional,
                                     uint64 maximum) {
    if (additional == 0) return True;
    return used <= maximum && reserved <= maximum - used &&
        additional <= maximum - used - reserved;
}

static bool __reserve_storage_quota__(_StorageQuotaReservation *reservation,
                                      const _StorageUsage *old_usage,
                                      const _StorageUsage *new_usage) {
    if (!reservation || !old_usage || !new_usage || !STORAGE_QUOTA_READY) return False;
    memset(reservation, 0, sizeof(*reservation));
    uint64 additional_files = new_usage->files > old_usage->files ?
        new_usage->files - old_usage->files : 0;
    uint64 additional_bytes = new_usage->bytes > old_usage->bytes ?
        new_usage->bytes - old_usage->bytes : 0;

    mutex_lock(&STORAGE_QUOTA_LOCK);
    bool allowed = __storage_quota_allows__(STORAGE_FILES, STORAGE_RESERVED_FILES,
                                            additional_files, STORAGE_MAX_FILES) &&
        __storage_quota_allows__(STORAGE_BYTES, STORAGE_RESERVED_BYTES,
                                 additional_bytes, STORAGE_MAX_BYTES);
    if (allowed) {
        STORAGE_RESERVED_FILES += additional_files;
        STORAGE_RESERVED_BYTES += additional_bytes;
        reservation->active = True;
        reservation->old_files = old_usage->files;
        reservation->old_bytes = old_usage->bytes;
        reservation->new_files = new_usage->files;
        reservation->new_bytes = new_usage->bytes;
        reservation->reserved_files = additional_files;
        reservation->reserved_bytes = additional_bytes;
    }
    mutex_unlock(&STORAGE_QUOTA_LOCK);
    return allowed;
}

static void __finalize_storage_reservation__(_StorageQuotaReservation *reservation,
                                             bool committed) {
    if (!reservation || !reservation->active || !STORAGE_QUOTA_READY) return;
    mutex_lock(&STORAGE_QUOTA_LOCK);
    if (committed && STORAGE_FILES >= reservation->old_files &&
        STORAGE_BYTES >= reservation->old_bytes) {
        STORAGE_FILES = STORAGE_FILES - reservation->old_files + reservation->new_files;
        STORAGE_BYTES = STORAGE_BYTES - reservation->old_bytes + reservation->new_bytes;
    }
    STORAGE_RESERVED_FILES = STORAGE_RESERVED_FILES >= reservation->reserved_files ?
        STORAGE_RESERVED_FILES - reservation->reserved_files : 0;
    STORAGE_RESERVED_BYTES = STORAGE_RESERVED_BYTES >= reservation->reserved_bytes ?
        STORAGE_RESERVED_BYTES - reservation->reserved_bytes : 0;
    mutex_unlock(&STORAGE_QUOTA_LOCK);
    reservation->active = False;
}

static bool __initialize_storage_quota__(void) {
    _StorageUsage usage = {0};
    if (!__measure_storage_path__("./tcp-file-storage", &usage) ||
        mutex_create(&STORAGE_QUOTA_LOCK, 1) != 0) return False;
    STORAGE_FILES = usage.files;
    STORAGE_BYTES = usage.bytes;
    STORAGE_RESERVED_FILES = 0;
    STORAGE_RESERVED_BYTES = 0;
    STORAGE_QUOTA_READY = True;
    if (usage.files > STORAGE_MAX_FILES || usage.bytes > STORAGE_MAX_BYTES)
        asynclog_warning("existing storage already exceeds the configured quota (%llu files, %llu bytes).",
                         usage.files, usage.bytes);
    return True;
}

static void __destroy_storage_quota__(void) {
    if (!STORAGE_QUOTA_READY) return;
    STORAGE_QUOTA_READY = False;
    mutex_destroy(&STORAGE_QUOTA_LOCK);
}

typedef struct {
    uint64 files;
    uint64 bytes;
    bool failed;
} _UploadStateUsage;

typedef struct {
    bool active;
    uint64 planned_size;
} _UploadQuotaReservation;

static bool __upload_checkpoint_name__(const char *name) {
    static const char prefix[] = "upload-";
    static const char suffix[] = ".part";
    usize length = strlen(name);
    return length == sizeof(prefix) - 1 + PROTOCOL_FILE_HASH_LENGTH + sizeof(suffix) - 1 &&
        strncmp(name, prefix, sizeof(prefix) - 1) == 0 &&
        strspn(name + sizeof(prefix) - 1, "0123456789abcdef") == PROTOCOL_FILE_HASH_LENGTH &&
        strcmp(name + sizeof(prefix) - 1 + PROTOCOL_FILE_HASH_LENGTH, suffix) == 0;
}

static void __measure_upload_state__(char *dir, char *name, bool folder, uint64 size, void *args) {
    _UploadStateUsage *usage = (_UploadStateUsage *)args;
    if (usage->failed || folder || !__upload_checkpoint_name__(name)) return;

    char path[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path)) {
        usage->failed = True;
        return;
    }
    if (size == 0) {
        if (remove(path) != 0) usage->failed = True;
        return;
    }
    if (usage->files == UINT64_MAX || UINT64_MAX - usage->bytes < size) {
        usage->failed = True;
        return;
    }
    usage->files++;
    usage->bytes += size;
}

static bool __initialize_upload_quota__(void) {
    _UploadStateUsage usage = {0};
    if (os_listdir(STORAGE_STATE_DIRECTORY, __measure_upload_state__, &usage) != 0 || usage.failed)
        return False;
    if (mutex_create(&UPLOAD_QUOTA_LOCK, 1) != 0) return False;
    UPLOAD_QUOTA_FILES = usage.files;
    UPLOAD_QUOTA_BYTES = usage.bytes;
    DIRECTORY_STAGING_FILES = 0;
    DIRECTORY_STAGING_BYTES = 0;
    UPLOAD_QUOTA_READY = True;
    return True;
}

static void __destroy_upload_quota__(void) {
    if (!UPLOAD_QUOTA_READY) return;
    UPLOAD_QUOTA_READY = False;
    DIRECTORY_STAGING_FILES = 0;
    DIRECTORY_STAGING_BYTES = 0;
    mutex_destroy(&UPLOAD_QUOTA_LOCK);
}

static void __forget_upload_checkpoint__(int64 size) {
    if (!UPLOAD_QUOTA_READY || size < 0) return;
    mutex_lock(&UPLOAD_QUOTA_LOCK);
    if (UPLOAD_QUOTA_FILES > 0) UPLOAD_QUOTA_FILES--;
    uint64 bytes = (uint64)size;
    UPLOAD_QUOTA_BYTES = UPLOAD_QUOTA_BYTES >= bytes ? UPLOAD_QUOTA_BYTES - bytes : 0;
    mutex_unlock(&UPLOAD_QUOTA_LOCK);
}

static bool __reserve_upload_checkpoint__(_UploadQuotaReservation *reservation,
                                           int64 existing_size, uint64 total_size) {
    if (!reservation || !UPLOAD_QUOTA_READY || total_size > (uint64)INT64_MAX) return False;
    memset(reservation, 0, sizeof(*reservation));

    uint64 current_size = existing_size >= 0 ? (uint64)existing_size : 0;
    if (current_size > total_size) return False;
    uint64 additional = total_size - current_size;
    uint64 additional_files = existing_size < 0 ? 1 : 0;

    mutex_lock(&UPLOAD_QUOTA_LOCK);
    bool allowed = additional_files <= UPLOAD_CHECKPOINT_MAX_FILES &&
        UPLOAD_QUOTA_FILES <= UPLOAD_CHECKPOINT_MAX_FILES - additional_files &&
        UPLOAD_QUOTA_BYTES <= UPLOAD_CHECKPOINT_MAX_BYTES &&
        additional <= UPLOAD_CHECKPOINT_MAX_BYTES - UPLOAD_QUOTA_BYTES;
    if (allowed && STORAGE_QUOTA_READY) {
        /* A resumable payload coexists with the published tree until rename commits it. */
        mutex_lock(&STORAGE_QUOTA_LOCK);
        allowed = STORAGE_FILES <= STORAGE_MAX_FILES && STORAGE_BYTES <= STORAGE_MAX_BYTES &&
            __storage_quota_allows__(STORAGE_FILES, 0, DIRECTORY_STAGING_FILES,
                                           STORAGE_MAX_FILES) &&
            DIRECTORY_STAGING_FILES <= STORAGE_MAX_FILES - STORAGE_FILES &&
            UPLOAD_QUOTA_FILES <= STORAGE_MAX_FILES - STORAGE_FILES -
                DIRECTORY_STAGING_FILES &&
            additional_files <= STORAGE_MAX_FILES - STORAGE_FILES -
                DIRECTORY_STAGING_FILES - UPLOAD_QUOTA_FILES &&
            __storage_quota_allows__(STORAGE_BYTES, 0, DIRECTORY_STAGING_BYTES,
                                     STORAGE_MAX_BYTES) &&
            DIRECTORY_STAGING_BYTES <= STORAGE_MAX_BYTES - STORAGE_BYTES &&
            UPLOAD_QUOTA_BYTES <= STORAGE_MAX_BYTES - STORAGE_BYTES -
                DIRECTORY_STAGING_BYTES &&
            additional <= STORAGE_MAX_BYTES - STORAGE_BYTES - DIRECTORY_STAGING_BYTES -
                UPLOAD_QUOTA_BYTES;
        mutex_unlock(&STORAGE_QUOTA_LOCK);
    }
    if (allowed) {
        UPLOAD_QUOTA_FILES += additional_files;
        UPLOAD_QUOTA_BYTES += additional;
        reservation->active = True;
        reservation->planned_size = total_size;
    }
    mutex_unlock(&UPLOAD_QUOTA_LOCK);
    return allowed;
}

static void __finalize_upload_reservation__(_UploadQuotaReservation *reservation,
                                             int64 actual_size) {
    if (!reservation || !reservation->active || !UPLOAD_QUOTA_READY) return;

    mutex_lock(&UPLOAD_QUOTA_LOCK);
    if (actual_size < 0) {
        if (UPLOAD_QUOTA_FILES > 0) UPLOAD_QUOTA_FILES--;
        UPLOAD_QUOTA_BYTES = UPLOAD_QUOTA_BYTES >= reservation->planned_size ?
            UPLOAD_QUOTA_BYTES - reservation->planned_size : 0;
    } else {
        uint64 actual = (uint64)actual_size;
        uint64 unused = reservation->planned_size > actual ? reservation->planned_size - actual : 0;
        UPLOAD_QUOTA_BYTES = UPLOAD_QUOTA_BYTES >= unused ? UPLOAD_QUOTA_BYTES - unused : 0;
    }
    mutex_unlock(&UPLOAD_QUOTA_LOCK);
    reservation->active = False;
}

static bool __reserve_directory_staging__(void) {
    if (!UPLOAD_QUOTA_READY || !STORAGE_QUOTA_READY) return False;
    mutex_lock(&UPLOAD_QUOTA_LOCK);
    mutex_lock(&STORAGE_QUOTA_LOCK);
    bool allowed = STORAGE_FILES <= STORAGE_MAX_FILES && STORAGE_BYTES <= STORAGE_MAX_BYTES &&
        DIRECTORY_STAGING_FILES <= STORAGE_MAX_FILES - STORAGE_FILES &&
        DIRECTORY_UPLOAD_MAX_FILES <= STORAGE_MAX_FILES - STORAGE_FILES -
            DIRECTORY_STAGING_FILES &&
        UPLOAD_QUOTA_FILES <= STORAGE_MAX_FILES - STORAGE_FILES -
            DIRECTORY_STAGING_FILES - DIRECTORY_UPLOAD_MAX_FILES &&
        DIRECTORY_STAGING_BYTES <= STORAGE_MAX_BYTES - STORAGE_BYTES &&
        DIRECTORY_UPLOAD_MAX_BYTES <= STORAGE_MAX_BYTES - STORAGE_BYTES -
            DIRECTORY_STAGING_BYTES &&
        UPLOAD_QUOTA_BYTES <= STORAGE_MAX_BYTES - STORAGE_BYTES -
            DIRECTORY_STAGING_BYTES - DIRECTORY_UPLOAD_MAX_BYTES;
    if (allowed) {
        DIRECTORY_STAGING_FILES += DIRECTORY_UPLOAD_MAX_FILES;
        DIRECTORY_STAGING_BYTES += DIRECTORY_UPLOAD_MAX_BYTES;
    }
    mutex_unlock(&STORAGE_QUOTA_LOCK);
    mutex_unlock(&UPLOAD_QUOTA_LOCK);
    return allowed;
}

static void __release_directory_staging__(void) {
    if (!UPLOAD_QUOTA_READY) return;
    mutex_lock(&UPLOAD_QUOTA_LOCK);
    DIRECTORY_STAGING_FILES = DIRECTORY_STAGING_FILES >= DIRECTORY_UPLOAD_MAX_FILES ?
        DIRECTORY_STAGING_FILES - DIRECTORY_UPLOAD_MAX_FILES : 0;
    DIRECTORY_STAGING_BYTES = DIRECTORY_STAGING_BYTES >= DIRECTORY_UPLOAD_MAX_BYTES ?
        DIRECTORY_STAGING_BYTES - DIRECTORY_UPLOAD_MAX_BYTES : 0;
    mutex_unlock(&UPLOAD_QUOTA_LOCK);
}

static void __clear_upload_checkpoint__(char *dir, char *name, bool folder, uint64 size,
                                        void *args) {
    (void)size;
    bool *failed = (bool *)args;
    if (*failed || folder || !__upload_checkpoint_name__(name)) return;
    char path[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path) || remove(path) != 0) *failed = True;
}

static bool __cleanup_upload_state__(void) {
    PathRWLock lock = {0};
    if (os_path_rwlock(&lock, UPLOAD_STATE_LOCK_FILE, 1) != 0) return False;

    _UploadStateUsage usage = {0};
    if (os_listdir(STORAGE_STATE_DIRECTORY, __measure_upload_state__, &usage) != 0)
        usage.failed = True;
    bool over_quota = usage.failed || usage.files > UPLOAD_CHECKPOINT_MAX_FILES ||
        usage.bytes > UPLOAD_CHECKPOINT_MAX_BYTES;
    if (over_quota) {
        bool clear_failed = False;
        if (os_listdir(STORAGE_STATE_DIRECTORY, __clear_upload_checkpoint__, &clear_failed) != 0 ||
            clear_failed) {
            os_path_rwunlock(&lock);
            return False;
        }
        asynclog_warning("upload checkpoints exceeded quota and were cleared (%llu files, %llu bytes).",
                         usage.files, usage.bytes);
        usage.files = 0;
        usage.bytes = 0;
    }
    if (UPLOAD_QUOTA_READY) {
        mutex_lock(&UPLOAD_QUOTA_LOCK);
        UPLOAD_QUOTA_FILES = usage.files;
        UPLOAD_QUOTA_BYTES = usage.bytes;
        mutex_unlock(&UPLOAD_QUOTA_LOCK);
    }
    os_path_rwunlock(&lock);
    return True;
}

static void __register_active_client__(Socket c) {
    mutex_lock(&ACTIVE_CLIENTS_LOCK);
    for (int i = 0; i < ACTIVE_CLIENT_LIMIT; i++) {
        if (ACTIVE_CLIENTS[i] == SOCKET_INVALID) {
            ACTIVE_CLIENTS[i] = c;
            break;
        }
    }
    mutex_unlock(&ACTIVE_CLIENTS_LOCK);
}

static void __unregister_active_client__(Socket c) {
    mutex_lock(&ACTIVE_CLIENTS_LOCK);
    for (int i = 0; i < ACTIVE_CLIENT_LIMIT; i++) {
        if (ACTIVE_CLIENTS[i] == c) ACTIVE_CLIENTS[i] = SOCKET_INVALID;
    }
    mutex_unlock(&ACTIVE_CLIENTS_LOCK);
}

static void __abort_active_clients__(void) {
    mutex_lock(&ACTIVE_CLIENTS_LOCK);
    for (int i = 0; i < ACTIVE_CLIENT_LIMIT; i++) {
        if (ACTIVE_CLIENTS[i] != SOCKET_INVALID) socket_abort(ACTIVE_CLIENTS[i]);
    }
    mutex_unlock(&ACTIVE_CLIENTS_LOCK);
}

static void __close_client_task__(void *args) {
    Socket c = (Socket)uintptr(args);
    __unregister_active_client__(c);
    socket_close(c);
}

void app_interrupt(int semaphore) {
    (void)semaphore;
    atomic_store_explicit(&RUNNING, 0, memory_order_release);
    Socket listener = atomic_exchange_explicit(&LISTENER, SOCKET_INVALID,
                                                memory_order_acq_rel);
    if (listener != SOCKET_INVALID) socket_close(listener);
}

static bool __send_app_response__(ProtocolConnection *connection, byte status,
                                  const char *message) {
    if (!message) message = "";
    bool ok = protocol_send_all(connection, &status, sizeof(status)) == 0 &&
        protocol_send_all(connection, message, strlen(message) + 1) == 0;
    protocol_shutdown(connection);
    return ok;
}

static bool __send_upload_error__(ProtocolConnection *connection, const char *message) {
    uint64 marker = socket_htonll(TRANSFER_ERROR_MARKER);
    return protocol_send_all(connection, &marker, sizeof(marker)) == 0 &&
        __send_app_response__(connection, APP_RESPONSE_ERROR, message);
}

static void __send_download_error__(ProtocolConnection *connection) {
    uint64 marker = socket_htonll(UINT64_MAX);
    protocol_send_all(connection, &marker, sizeof(marker));
    protocol_shutdown(connection);
}

bool app_handshake(ProtocolConnection *connection, TransportProtocolHeader *meta,
                   const char *key, double deadline) {
    if (!protocol_server_authenticate(connection, meta, key, REMOTE_HOST_IPV4,
                                      REMOTE_HOST_PORT, deadline))
        return False;

    if (!__safe_relative_path__(meta->filename, meta->command == APP_COMMAND_LIST)) {
        asynclog_warning("refusal to transmit with unsafe path \"%s\" suspected to traverse boundary!", meta->filename);
        byte status = HANDSHAKE_ERROR_PATH;
        protocol_send_all(connection, &status, sizeof(status));
        protocol_shutdown(connection);
        return False;
    }

    byte status = HANDSHAKE_OK;
    return protocol_send_all(connection, &status, sizeof(status)) == 0;
}

bool app_run_service(int port, const char *key) {
    atomic_store_explicit(&RUNNING, 1, memory_order_release);
    if (!atomic_is_lock_free(&RUNNING) || !atomic_is_lock_free(&LISTENER)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m lock-free service atomics are unavailable.\n");
        return False;
    }
    if (!protocol_key_valid(key)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m encryption key must contain 16 to %d bytes.\n",
               PROTOCOL_KEY_MAX_LENGTH);
        return False;
    }
    snprintf(SERVER_KEY, sizeof(SERVER_KEY), "%s", key ? key : "");
    signal(SIGINT, app_interrupt);
    bool active_clients_ready = False;
    bool upload_quota_ready = False;
    bool storage_quota_ready = False;
    PathLock service_lock = {0};
    bool service_locked = False;
    char service_lock_file[PATH_LENGTH_RESTRICTION];

    char workspace[PATH_LENGTH_RESTRICTION];
    if (!__checked_directory__("tcp-file-storage")) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to create the \"tcp-file-storage\" folder in \"%s\"\n", os_getpwd(workspace, sizeof(workspace)));
        goto clear_server_key;
    }
    if (!__checked_state_directory__(STORAGE_STATE_DIRECTORY)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to initialize private transfer state in \"%s\"\n", os_getpwd(workspace, sizeof(workspace)));
        goto clear_server_key;
    }
    if (!__initialize_storage_quota__()) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to measure remote storage quota.\n");
        goto clear_server_key;
    }
    storage_quota_ready = True;
    /* Quota reservations are process-local, so only one service may own a storage root. */
    if (!__transfer_lock_path__(service_lock_file, sizeof(service_lock_file), STORAGE_STATE_DIRECTORY) ||
        os_path_lock(&service_lock, service_lock_file) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m another service is already using this storage directory.\n");
        goto release_service_lock;
    }
    service_locked = True;
    /* Older releases used this transient subtree; it has no resumable value. */
    char legacy_downloads[] = STORAGE_STATE_DIRECTORY "/downloads";
    if (os_access(legacy_downloads) == 1 && os_remove(legacy_downloads) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to clear stale download snapshots.\n");
        goto release_service_lock;
    }
    if (!__cleanup_upload_state__()) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to enforce upload checkpoint quota.\n");
        goto release_service_lock;
    }
    if (!__initialize_upload_quota__()) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to initialize upload checkpoint accounting.\n");
        goto release_service_lock;
    }
    upload_quota_ready = True;

    if (socket_init() == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m initialize socket failed.\n");
        goto release_service_lock;
    }

    Socket listener = socket_create(AF_INET, SOCK_STREAM, 0);
    if (listener == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m create socket failed.\n");
        goto release_server;
    }
    atomic_store_explicit(&LISTENER, listener, memory_order_release);

    int recv_buffer_size = BUFFER_CAPACITY;
    if (socket_setopt(listener, SOL_SOCKET, SO_RCVBUF, &recv_buffer_size, sizeof(recv_buffer_size)) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket buffer setting failed.\n");
        goto release_server;
    }

    if (socket_setopt(listener, SOL_SOCKET, SO_REUSEADDR, NULL, 0) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket port reuse failed.\n");
        goto release_server;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    socket_config(&server, AF_INET, "0.0.0.0", port);
    REMOTE_HOST_PORT = port;
    socket_ipv4(REMOTE_HOST_IPV4, sizeof(REMOTE_HOST_IPV4));

    if (socket_bind(listener, &server, sizeof(server)) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket bind failed: %s.\n", strerror(errno));
        goto release_server;
    }

    if (socket_listen(listener, 16) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket listen failed.\n");
        goto release_server;
    }

    if (mutex_create(&ACTIVE_CLIENTS_LOCK, 1) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to initialize active client tracking.\n");
        goto release_server;
    }
    for (int i = 0; i < ACTIVE_CLIENT_LIMIT; i++) ACTIVE_CLIENTS[i] = SOCKET_INVALID;
    active_clients_ready = True;

    ThreadPool *pool = threadpool_create(8, 32);
    if (!pool) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to initialize server workers.\n");
        goto release_server;
    }

    app_start_tips(REMOTE_HOST_IPV4, REMOTE_HOST_PORT);

    while (atomic_load_explicit(&RUNNING, memory_order_acquire)) {
        struct sockaddr_in client;
        int client_length = sizeof(client);

        Socket c = socket_accept(listener, &client, &client_length);
        if (c == SOCKET_INVALID) {
            if (!atomic_load_explicit(&RUNNING, memory_order_acquire)) break;
            asynclog_warning("socket accept failed.");
            continue;
        }

        asynclog_trace("client from \"%s:%d\"", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        /* Reject excess established clients so shutdown cannot block on a full work queue. */
        if (threadpool_add(pool, app_thread_task, (void *)uintptr(c), 0, __close_client_task__)) {
            asynclog_warning("server rejected or stopped while queuing a client connection.");
            socket_abort(c);
            socket_close(c);
            if (!atomic_load_explicit(&RUNNING, memory_order_acquire)) break;
        }
    }

    __abort_active_clients__();
    threadpool_destroy(pool, 0);
    mutex_destroy(&ACTIVE_CLIENTS_LOCK);
    __destroy_upload_quota__();
    __destroy_storage_quota__();
    Socket owned_listener = atomic_exchange_explicit(&LISTENER, SOCKET_INVALID,
                                                     memory_order_acq_rel);
    if (owned_listener != SOCKET_INVALID) socket_close(owned_listener);
    socket_destroy();
    os_path_unlock(&service_lock);
    secure_memzero(SERVER_KEY, sizeof(SERVER_KEY));
    return True;
    release_server:
        if (active_clients_ready) mutex_destroy(&ACTIVE_CLIENTS_LOCK);
        Socket release_listener = atomic_exchange_explicit(&LISTENER, SOCKET_INVALID,
                                                           memory_order_acq_rel);
        if (release_listener != SOCKET_INVALID) socket_close(release_listener);
        socket_destroy();
    release_service_lock:
        if (upload_quota_ready) __destroy_upload_quota__();
        if (storage_quota_ready) __destroy_storage_quota__();
        if (service_locked) os_path_unlock(&service_lock);
    clear_server_key:
        secure_memzero(SERVER_KEY, sizeof(SERVER_KEY));
        return False;
}

void app_thread_task(void *args) {
    Socket c = (Socket)uintptr(args);
    ProtocolConnection connection;
    protocol_connection_init(&connection, c);
    __register_active_client__(c);
    if (!atomic_load_explicit(&RUNNING, memory_order_acquire)) return;

    if (socket_setopt_timeout(c, 0, HANDSHAKE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        asynclog_warning("socket sender timeout setting failed.");
        goto release_connection;
    }
    if (socket_setopt_timeout(c, 1, HANDSHAKE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        asynclog_warning("socket receiver timeout setting failed.");
        goto release_connection;
    }

    TransportProtocolHeader meta;
    double handshake_deadline = os_time() + HANDSHAKE_TIMEOUT_SECONDS;
    if (!app_handshake(&connection, &meta, SERVER_KEY, handshake_deadline)) {
        goto release_connection;
    }

    if (socket_setopt_timeout(c, 0, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID ||
        socket_setopt_timeout(c, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        asynclog_warning("socket transfer timeout setting failed.");
        goto release_connection;
    }

    switch (meta.command) {
        case APP_COMMAND_SEND:
            asynclog_info("execute APP_COMMAND_SEND for file: \"%s\".", meta.filename);
            app_server_send(&connection, &meta);
            break;
        case APP_COMMAND_LIST:
            asynclog_info("execute APP_COMMAND_LIST.");
            app_server_list(&connection, &meta);
            break;
        case APP_COMMAND_DOWNLOAD:
            asynclog_info("execute APP_COMMAND_DOWNLOAD for file: \"%s\".", meta.filename);
            app_server_download(&connection, &meta);
            break;
        case APP_COMMAND_REMOVE:
            asynclog_info("execute APP_COMMAND_REMOVE for file: \"%s\".", meta.filename);
            app_server_remove(&connection, &meta);
            break;
        case APP_COMMAND_SEND_DIRECTORY:
            asynclog_info("execute APP_COMMAND_SEND_DIRECTORY for directory: \"%s\".", meta.filename);
            app_server_send_directory(&connection, &meta);
            break;
        case APP_COMMAND_MKDIR:
            asynclog_info("execute APP_COMMAND_MKDIR for directory: \"%s\".", meta.filename);
            app_server_mkdir(&connection, &meta);
            break;
        default:
            asynclog_warning("Unknown command received: 0x%02x.", meta.command);
            app_server_default(&connection);
            break;
    }

    release_connection:
    protocol_connection_clear(&connection);
    /* The thread-pool cleanup callback owns the socket for every exit path. */
}

static bool __recv_full__(ProtocolConnection *connection, char *buffer, uint64 want) {
    return protocol_recv_all(connection, buffer, (usize)want) == 0;
}

typedef bool (*_HashProgressCallback)(ProtocolConnection *connection);

static bool __send_upload_hash_progress__(ProtocolConnection *connection) {
    uint64 marker = socket_htonll(TRANSFER_HASHING_MARKER);
    return protocol_send_all(connection, &marker, sizeof(marker)) == 0;
}

static bool __send_download_hash_progress__(ProtocolConnection *connection) {
    byte marker = DOWNLOAD_HASHING;
    return protocol_send_all(connection, &marker, sizeof(marker)) == 0;
}

static bool __hash_open_file_prefix__(SHA256Context *ctx, FILE *fp, uint64 length,
                                      ProtocolConnection *connection,
                                      _HashProgressCallback progress) {
    if (!ctx || !fp || os_fseek(fp, 0, SEEK_SET) != 0) return False;
    char buffer[BUFFER_CAPACITY];
    uint64 remaining = length;
    bool ok = True;
    double last_progress = os_time();
    while (remaining > 0) {
        usize want = remaining > sizeof(buffer) ? sizeof(buffer) : (usize)remaining;
        usize read = fread(buffer, 1, want, fp);
        if (read != want) {
            ok = False;
            break;
        }
        sha256_update(ctx, (unsigned char *)buffer, (unsigned int)read);
        remaining -= read;
        double now = os_time();
        if (progress && now - last_progress >= HASH_PROGRESS_INTERVAL_SECONDS) {
            if (!progress(connection)) return False;
            last_progress = now;
        }
    }
    return ok;
}

static bool __hash_file_prefix__(SHA256Context *ctx, const char *path, uint64 length,
                                 ProtocolConnection *connection,
                                 _HashProgressCallback progress) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return False;
    bool ok = __hash_open_file_prefix__(ctx, fp, length, connection, progress);
    if (fclose(fp) != 0) ok = False;
    return ok;
}

static bool __hash_file_digest__(const char *path, uint64 length,
                                 ProtocolConnection *connection,
                                 _HashProgressCallback progress, char hash[PROTOCOL_FILE_HASH_LENGTH + 1]) {
    SHA256Context ctx;
    sha256_init(&ctx);
    if (!__hash_file_prefix__(&ctx, path, length, connection, progress)) return False;
    byte digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, digest);
    app_hex_encode(digest, sizeof(digest), hash);
    return True;
}

void app_server_send(ProtocolConnection *connection, TransportProtocolHeader *meta) {
    double start_time = os_time();

    char filepath[PATH_LENGTH_RESTRICTION];
    char temp_file[PATH_LENGTH_RESTRICTION * 2];
    PathRWLock state_lock = {0};
    _UploadQuotaReservation quota_reservation = {0};
    _StorageQuotaReservation storage_reservation = {0};
    _StorageUsage previous_usage = {0};
    bool state_locked = False;
    bool storage_committed = False;
    bool cleanup_state = False;
    bool offset_sent = False;
    bool terminal_sent = False;
    int64 checkpoint_size = -1;
    const char *failure = "Upload initialization failed.";
    _StoragePathLock hierarchy_lock = {0};
    if (!__storage_path__(filepath, sizeof(filepath), meta->filename, False)) {
        asynclog_warning("unable to create a safe destination for \"%s\".", meta->filename);
        __send_upload_error__(connection, "Invalid upload destination.");
        return;
    }
    if (strlen(meta->sha256) != PROTOCOL_FILE_HASH_LENGTH || strspn(meta->sha256, "0123456789abcdefABCDEF") != PROTOCOL_FILE_HASH_LENGTH) {
        asynclog_warning("invalid SHA-256 metadata for \"%s\".", meta->filename);
        __send_upload_error__(connection, "Invalid upload checksum metadata.");
        return;
    }
    if (!__storage_path_lock__(&hierarchy_lock, filepath)) {
        __send_upload_error__(connection, "Failed to lock the upload path hierarchy.");
        return;
    }

    PathRWLock tree_lock = {0};
    if (!__storage_tree_lock__(&tree_lock, True)) {
        __send_upload_error__(connection, "Failed to lock remote storage.");
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }
    bool parent_ready = __storage_ensure_parents__(meta->filename);
    os_path_rwunlock(&tree_lock);
    if (!parent_ready) {
        asynclog_warning("unable to create a safe destination for \"%s\".", meta->filename);
        __send_upload_error__(connection, "Failed to create upload destination.");
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }

    char lock_file[PATH_LENGTH_RESTRICTION];
    if (!__transfer_lock_path__(lock_file, sizeof(lock_file), filepath)) {
        __send_upload_error__(connection, "Failed to create upload lock.");
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }
    PathLock transfer_lock;
    if (os_path_lock(&transfer_lock, lock_file) != 0) {
        uint64 busy = socket_htonll(TRANSFER_BUSY_MARKER);
        protocol_send_all(connection, &busy, sizeof(busy));
        protocol_shutdown(connection);
        asynclog_warning("transfer already active for \"%s\".", meta->filename);
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }

    if (!__checkpoint_path__(temp_file, sizeof(temp_file), meta->filename, meta->sha256)) {
        failure = "Failed to create upload checkpoint path.";
        goto release_transfer_lock;
    }
    if (os_path_rwlock(&state_lock, UPLOAD_STATE_LOCK_FILE, 0) != 0) {
        failure = "Failed to lock upload checkpoint state.";
        goto release_transfer_lock;
    }
    state_locked = True;

    uint64 total_size = socket_ntohll(meta->filesize);
    int64 temp_size = -1;
    if (__state_checkpoint_size__(temp_file, &temp_size) < 0) {
        failure = "Upload checkpoint is not a private regular file.";
        goto release_transfer_lock;
    }
    checkpoint_size = temp_size;
    bool existing_complete = False;

    /* Open the published file under a shared snapshot lock, then hash it without
       monopolizing the storage tree. The per-target lock serializes replacement. */
    FILE *existing = NULL;
    if (!__storage_tree_lock__(&tree_lock, False)) {
        failure = "Failed to inspect the remote destination.";
        goto release_transfer_lock;
    }
    int destination_state = __storage_entry_state__(meta->filename);
    if (destination_state < 0 ||
        (destination_state == 1 && !__measure_remote_storage__(meta->filename,
                                                                 &previous_usage))) {
        os_path_rwunlock(&tree_lock);
        failure = "Failed to measure the remote destination.";
        goto release_transfer_lock;
    }
    int64 finished_size = -1;
    if (!__storage_open_regular__(meta->filename, &existing, &finished_size) ||
        (uint64)finished_size != total_size) {
        if (existing) fclose(existing);
        existing = NULL;
    }
    os_path_rwunlock(&tree_lock);

    if (existing) {
        char existing_hash[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
        SHA256Context existing_ctx;
        sha256_init(&existing_ctx);
        bool hash_ok = __hash_open_file_prefix__(&existing_ctx, existing, total_size, connection,
                                                 __send_upload_hash_progress__);
        if (hash_ok) {
            byte digest[SHA256_DIGEST_SIZE];
            sha256_final(&existing_ctx, digest);
            app_hex_encode(digest, sizeof(digest), existing_hash);
        }
        bool still_published = False;
        if (hash_ok && __storage_tree_lock__(&tree_lock, False)) {
            still_published = os_file_is_path(existing, filepath) == 1;
            os_path_rwunlock(&tree_lock);
        }
        if (fclose(existing) != 0) hash_ok = False;
        if (!hash_ok) {
            failure = "Failed to verify the existing remote file.";
            goto release_transfer_lock;
        }
        if (still_published && strcmp(existing_hash, meta->sha256) == 0) {
            existing_complete = True;
        }
    }

    if (!existing_complete) {
        _StorageUsage next_usage = { .files = 1, .bytes = total_size };
        if (!__reserve_storage_quota__(&storage_reservation, &previous_usage,
                                       &next_usage)) {
            failure = "Final storage quota would be exceeded.";
            goto release_transfer_lock;
        }
    }

    if (temp_size >= 0 && (uint64)temp_size > total_size) {
        if (remove(temp_file) != 0) {
            failure = "Failed to reset an invalid upload checkpoint.";
            goto release_transfer_lock;
        }
        __forget_upload_checkpoint__(temp_size);
        temp_size = -1;
        checkpoint_size = -1;
    }
    if (!existing_complete && temp_size >= 0 && (uint64)temp_size == total_size) {
        char complete_hash[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
        bool complete_ok = __hash_file_digest__(temp_file, total_size, connection,
                                                __send_upload_hash_progress__, complete_hash);
        bool promoted = False;
        if (complete_ok && strcmp(complete_hash, meta->sha256) == 0 &&
            __storage_tree_lock__(&tree_lock, True)) {
            promoted = __storage_rename_into__(temp_file, meta->filename);
            os_path_rwunlock(&tree_lock);
        }
        if (promoted) {
            storage_committed = True;
            __forget_upload_checkpoint__(temp_size);
            temp_size = -1;
            checkpoint_size = -1;
            existing_complete = True;
        } else {
            if (remove(temp_file) != 0) {
                failure = "Failed to remove an invalid upload checkpoint.";
                goto release_transfer_lock;
            }
            __forget_upload_checkpoint__(temp_size);
            temp_size = -1;
            checkpoint_size = -1;
        }
    }
    if (existing_complete && temp_size >= 0) {
        if (remove(temp_file) != 0) {
            failure = "Failed to remove a stale upload checkpoint.";
            goto release_transfer_lock;
        }
        __forget_upload_checkpoint__(temp_size);
        temp_size = -1;
        checkpoint_size = -1;
    }

    uint64 offset = existing_complete ? total_size : temp_size >= 0 ? (uint64)temp_size : 0;

    SHA256Context ctx;
    sha256_init(&ctx);
    if (!existing_complete) {
        if (!__reserve_upload_checkpoint__(&quota_reservation, temp_size, total_size)) {
            uint64 quota = socket_htonll(TRANSFER_QUOTA_MARKER);
            protocol_send_all(connection, &quota, sizeof(quota));
            protocol_shutdown(connection);
            terminal_sent = True;
            asynclog_warning("upload checkpoint quota rejected \"%s\" (%llu bytes).",
                             meta->filename, total_size);
            goto release_transfer_lock;
        }
        /* Finish hashing the retained prefix before inviting the client to send more data. */
        if (offset > 0 && !__hash_file_prefix__(&ctx, temp_file, offset, connection,
                                                __send_upload_hash_progress__)) {
            asynclog_warning("failed to hash temporary file \"%s\".", temp_file);
            failure = "Failed to verify the upload checkpoint.";
            goto release_transfer_lock;
        }
    }

    uint64 network_offset = socket_htonll(offset);
    if (protocol_send_all(connection, &network_offset, sizeof(network_offset)) != 0) {
        asynclog_warning("sending file offset failed.");
        terminal_sent = True;
        goto release_transfer_lock;
    }
    offset_sent = True;

    if (existing_complete && total_size == 0) {
        const char *response = "[\x1b[1;32mOK\x1b[0m] empty file already exists.";
        __send_app_response__(connection, APP_RESPONSE_OK, response);
        terminal_sent = True;
        goto release_transfer_lock;
    }

    if (total_size > 0 && offset >= total_size) {
        asynclog_warning("file \"%s\" already fully received.", meta->filename);
        protocol_shutdown(connection);
        terminal_sent = True;
        goto release_transfer_lock;
    }

    char buffer[BUFFER_CAPACITY];
    FILE *fp = NULL;
    if (!__state_open_upload_checkpoint__(temp_file, &fp)) {
        asynclog_warning("temp file \"%s\" fopen failed.", temp_file);
        failure = "Failed to open the upload checkpoint.";
        goto release_transfer_lock;
    }
    if (os_fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        failure = "Failed to seek the upload checkpoint.";
        goto release_transfer_lock;
    }

    bool ok = True;
    uint64 received_bytes = 0;
    uint64 expected_size = total_size - offset;

    while (received_bytes < expected_size) {
        uint64 want = (expected_size - received_bytes) > sizeof(buffer) ? sizeof(buffer) : expected_size - received_bytes;
        if (!__recv_full__(connection, buffer, want)) {
            ok = False;
            break;
        }

        if (fwrite(buffer, 1, want, fp) != (usize)want) {
            asynclog_fatal("file \"%s\" disk write failed.", temp_file);
            ok = False;
            break;
        }
        sha256_update(&ctx, (unsigned char *)buffer, (unsigned int)want);
        received_bytes = received_bytes + want;
    }

    int sync_status = os_file_sync(fp);
    int64 actual_size = os_file_size(fp);
    int close_status = fclose(fp);
    if (sync_status != 0 || close_status != 0) {
        asynclog_fatal("failed to flush temporary file \"%s\".", temp_file);
        ok = False;
    }

    checkpoint_size = actual_size;
    if (!ok) {
        cleanup_state = True;
        failure = "Upload interrupted or checkpoint write failed.";
        asynclog_warning("incomplete transmission to keep temporary file for resume.");
        goto release_transfer_lock;
    }

    if (actual_size == -1) {
        cleanup_state = True;
        failure = "Failed to inspect the upload checkpoint.";
        asynclog_warning("get actual size of temp file failed.");
        goto release_transfer_lock;
    }
    if ((uint64)actual_size == total_size) {
        char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
        byte digest[SHA256_DIGEST_SIZE];
        sha256_final(&ctx, digest);
        app_hex_encode(digest, sizeof(digest), hash);

        char response[1024];
        byte response_status = APP_RESPONSE_ERROR;
        bool committed = False;
        /* Payload reception is concurrent; only the atomic publication waits for directory snapshots. */
        if (strcmp(hash, meta->sha256) == 0 && __storage_tree_lock__(&tree_lock, True)) {
            committed = __storage_rename_into__(temp_file, meta->filename);
            os_path_rwunlock(&tree_lock);
        }
        if (strcmp(hash, meta->sha256) != 0) {
            remove(temp_file);
            checkpoint_size = -1;
            snprintf(
                response, sizeof(response),
                "[\x1b[1;31mFAILURE\x1b[0m] delete the \"%s\" file that does dismatch the SHA-256 checksum.",
                os_basename(filepath)
            );
            asynclog_warning("delete the \"%s\" file that does dismatch the SHA-256 checksum.", os_basename(filepath));
        } else if (committed) {
            storage_committed = True;
            checkpoint_size = -1;
            response_status = APP_RESPONSE_OK;
            char temp[16];
            parse_unit_convert(total_size, temp, sizeof(temp));
            snprintf(
                response, sizeof(response),
                "[\x1b[1;32mOK\x1b[0m] file \"%s\" has been successfully uploaded:\n"
                " - SHA-256 hash: %s\n"
                " - File size: %s\n"
                " - Cost time: %.5lf s",
                os_basename(filepath), hash, temp, os_time() - start_time
            );
            asynclog_info(
                "file \"%s\" has been successfully uploaded:\n"
                " - SHA-256 hash: %s\n"
                " - File size: %s\n"
                " - Cost time: %.5lf s",
                os_basename(filepath), hash, temp, os_time() - start_time
            );
        } else {
            cleanup_state = True;
            snprintf(response, sizeof(response), "[\x1b[1;31mFAILURE\x1b[0m] rename failed for \"%s\".", os_basename(filepath));
            asynclog_warning("failed to rename temp file to \"%s\".", filepath);
        }
        __send_app_response__(connection, response_status, response);
        terminal_sent = True;
    } else {
        cleanup_state = True;
        asynclog_warning("checkpoint progress: %.3lf%%", (double)actual_size / total_size * 100);
    }

    release_transfer_lock:
        if (!terminal_sent) {
            if (offset_sent) __send_app_response__(connection, APP_RESPONSE_ERROR, failure);
            else __send_upload_error__(connection, failure);
        }
        if (cleanup_state && checkpoint_size == 0 && remove(temp_file) == 0)
            checkpoint_size = -1;
        __finalize_upload_reservation__(&quota_reservation, checkpoint_size);
        __finalize_storage_reservation__(&storage_reservation, storage_committed);
        if (state_locked) os_path_rwunlock(&state_lock);
        os_path_unlock(&transfer_lock);
        __storage_path_unlock__(&hierarchy_lock);
        if (cleanup_state && !__cleanup_upload_state__())
            asynclog_error("failed to enforce upload checkpoint quota after interruption.");
}

static bool __directory_transaction_paths__(char *staging, usize staging_capacity,
                                            char *backup, usize backup_capacity,
                                            const char *root) {
    char canonical[PATH_LENGTH_RESTRICTION * 2];
    if (os_canonical_path(canonical, sizeof(canonical), root) != 0) return False;
    char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
    sha256_string(canonical, hash);
    int staging_length = snprintf(staging, staging_capacity,
                                  STORAGE_STATE_DIRECTORY "/directory-%s.uploading", hash);
    int backup_length = snprintf(backup, backup_capacity,
                                 STORAGE_STATE_DIRECTORY "/directory-%s.backup", hash);
    return staging_length >= 0 && staging_length < (int)staging_capacity &&
        backup_length >= 0 && backup_length < (int)backup_capacity;
}

static bool __server_receive_directory_file__(ProtocolConnection *connection, char *filepath,
                                              const char *staging_root, uint64 size) {
    if (size > (uint64)INT64_MAX) return False;

    char root_prefix[PATH_LENGTH_RESTRICTION * 2];
    int prefix_length = snprintf(root_prefix, sizeof(root_prefix), "%s/", staging_root);
    if (prefix_length < 0 || prefix_length >= (int)sizeof(root_prefix) ||
        !__ensure_parent_dirs_under__(filepath, root_prefix)) return False;

    char temporary[PATH_LENGTH_RESTRICTION * 2 + 32];
    int temporary_length = snprintf(temporary, sizeof(temporary), "%s.tcper-downloading", filepath);
    if (temporary_length < 0 || temporary_length >= (int)sizeof(temporary)) return False;
    if (os_access(temporary) == 1 && remove(temporary) != 0) return False;

    PathRWLock state_lock = {0};
    bool state_locked = os_path_rwlock(&state_lock, UPLOAD_STATE_LOCK_FILE, 0) == 0;
    _UploadQuotaReservation quota_reservation = {0};
    bool ok = state_locked && __reserve_upload_checkpoint__(&quota_reservation, -1, size);
    FILE *fp = ok ? fopen(temporary, "wb") : NULL;
    if (!fp) ok = False;

    SHA256Context sha256;
    sha256_init(&sha256);
    char buffer[BUFFER_CAPACITY];
    uint64 received = 0;
    while (ok && received < size) {
        usize want = size - received > sizeof(buffer) ? sizeof(buffer) : (usize)(size - received);
        if (protocol_recv_all(connection, buffer, want) != 0) {
            ok = False;
            break;
        }
        if (fwrite(buffer, 1, want, fp) != want) {
            ok = False;
            break;
        }
        sha256_update(&sha256, (unsigned char *)buffer, (unsigned int)want);
        received += want;
    }

    if (fp) {
        int sync_status = os_file_sync(fp);
        int close_status = fclose(fp);
        if (sync_status != 0 || close_status != 0) ok = False;
        fp = NULL;
    }

    char expected[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
    if (ok && protocol_recv_all(connection, expected, PROTOCOL_FILE_HASH_LENGTH) != 0) ok = False;
    if (ok && strspn(expected, "0123456789abcdefABCDEF") != PROTOCOL_FILE_HASH_LENGTH) ok = False;

    byte digest[SHA256_DIGEST_SIZE];
    char actual[PROTOCOL_FILE_HASH_LENGTH + 1];
    sha256_final(&sha256, digest);
    app_hex_encode(digest, sizeof(digest), actual);
    if (ok && strcmp(actual, expected) != 0) ok = False;

    if (ok) ok = os_rename_replace(temporary, filepath) == 0;

    if (!ok && temporary[0]) remove(temporary);
    __finalize_upload_reservation__(&quota_reservation, -1);
    if (state_locked) os_path_rwunlock(&state_lock);
    return ok;
}

void app_server_send_directory(ProtocolConnection *connection,
                               TransportProtocolHeader *meta) {
    char root[PATH_LENGTH_RESTRICTION];
    bool ok = __storage_path__(root, sizeof(root), meta->filename, False);
    _StoragePathLock hierarchy_lock = {0};
    bool hierarchy_locked = False;
    PathRWLock tree_lock = {0};
    PathLock transfer_lock = {0};
    _StorageQuotaReservation storage_reservation = {0};
    bool transfer_locked = False;
    bool staging_reserved = False;
    bool storage_committed = False;
    char staging[PATH_LENGTH_RESTRICTION * 2] = {0};
    char backup[PATH_LENGTH_RESTRICTION * 2] = {0};
    const char *failure = "Directory upload interrupted.";

    if (ok) {
        hierarchy_locked = __storage_path_lock__(&hierarchy_lock, root);
        ok = hierarchy_locked;
    }
    if (ok) ok = __storage_tree_lock__(&tree_lock, True);
    if (ok) {
        ok = __storage_ensure_parents__(meta->filename);
        os_path_rwunlock(&tree_lock);
    }
    if (ok) ok = __directory_transaction_paths__(staging, sizeof(staging), backup,
                                                  sizeof(backup), root);
    char lock_file[PATH_LENGTH_RESTRICTION];
    if (ok) ok = __transfer_lock_path__(lock_file, sizeof(lock_file), root);
    if (ok) {
        ok = os_path_lock(&transfer_lock, lock_file) == 0;
        transfer_locked = ok;
    }
    if (ok) {
        staging_reserved = __reserve_directory_staging__();
        if (!staging_reserved) {
            failure = "Directory staging quota would exceed storage capacity.";
            ok = False;
        }
    }

    /* Recover either side of an interrupted tree swap before starting anew. */
    if (ok) ok = __storage_tree_lock__(&tree_lock, True);
    if (ok) {
        int root_exists = __storage_entry_state__(meta->filename);
        int backup_exists = os_access(backup);
        ok = root_exists >= 0 && backup_exists >= 0;
        if (ok && root_exists == 0 && backup_exists == 1)
            ok = __storage_rename_into__(backup, meta->filename);
        os_path_rwunlock(&tree_lock);
    }
    if (ok && os_access(backup) == 1) ok = os_remove(backup) == 0;
    if (ok && os_access(staging) == 1) ok = os_remove(staging) == 0;
    if (ok) ok = __checked_directory__(staging);

    uint64 files = 0;
    uint64 directories = 0;
    uint64 bytes = 0;
    while (ok) {
        byte type;
        if (protocol_recv_all(connection, &type, sizeof(type)) != 0) {
            ok = False;
            break;
        }
        if (type == APP_DIRECTORY_END) break;
        if (type != APP_DIRECTORY_ENTRY && type != APP_DIRECTORY_FILE) {
            ok = False;
            break;
        }

        uint16 network_length;
        uint64 network_size;
        if (protocol_recv_all(connection, &network_length, sizeof(network_length)) != 0 ||
            protocol_recv_all(connection, &network_size, sizeof(network_size)) != 0) {
            ok = False;
            break;
        }
        usize length = socket_ntohs(network_length);
        uint64 size = socket_ntohll(network_size);
        if (length == 0 || length >= PATH_LENGTH_RESTRICTION) {
            ok = False;
            break;
        }

        char relative[PATH_LENGTH_RESTRICTION];
        if (protocol_recv_all(connection, relative, length) != 0) {
            ok = False;
            break;
        }
        relative[length] = 0;
        if (!__safe_relative_path__(relative, False)) {
            ok = False;
            break;
        }

        char destination[PATH_LENGTH_RESTRICTION * 2];
        int n = snprintf(destination, sizeof(destination), "%s/%s", staging, relative);
        if (n < 0 || n >= (int)sizeof(destination)) {
            ok = False;
            break;
        }

        if (type == APP_DIRECTORY_ENTRY) {
            if (directories >= DIRECTORY_UPLOAD_MAX_DIRECTORIES) {
                failure = "Directory upload exceeds the directory entry limit.";
                ok = False;
                break;
            }
            char staging_prefix[PATH_LENGTH_RESTRICTION * 2];
            int prefix_length = snprintf(staging_prefix, sizeof(staging_prefix), "%s/", staging);
            ok = size == 0 && prefix_length >= 0 &&
                prefix_length < (int)sizeof(staging_prefix) &&
                __ensure_parent_dirs_under__(destination, staging_prefix) &&
                __checked_directory__(destination);
            if (ok) directories++;
        } else {
            if (files >= DIRECTORY_UPLOAD_MAX_FILES ||
                size > DIRECTORY_UPLOAD_MAX_BYTES - bytes) {
                failure = "Directory upload exceeds the file count or byte quota.";
                ok = False;
                break;
            }
            ok = __server_receive_directory_file__(connection, destination, staging, size);
            if (ok) {
                files++;
                if (UINT64_MAX - bytes < size) ok = False;
                else bytes += size;
            }
        }
    }

    _StorageUsage next_usage = {0};
    if (ok && !__measure_storage_path__(staging, &next_usage)) {
        failure = "Failed to measure the completed directory upload.";
        ok = False;
    }
    if (ok) ok = __storage_tree_lock__(&tree_lock, True);
    if (ok) {
        int root_exists = __storage_entry_state__(meta->filename);
        _StorageUsage previous_usage = {0};
        ok = root_exists >= 0;
        if (ok && root_exists == 1 &&
            !__measure_remote_storage__(meta->filename, &previous_usage)) {
            failure = "Failed to measure the directory being replaced.";
            ok = False;
        }
        if (ok && !__reserve_storage_quota__(&storage_reservation, &previous_usage,
                                              &next_usage)) {
            failure = "Final storage quota would be exceeded.";
            ok = False;
        }
        if (ok && root_exists == 1) ok = __storage_rename_out__(meta->filename, backup);
        if (ok && !__storage_rename_into__(staging, meta->filename)) {
            ok = False;
            if (root_exists == 1 && !__storage_rename_into__(backup, meta->filename))
                asynclog_fatal("failed to restore directory \"%s\" after publication failure.", root);
        }
        if (ok) storage_committed = True;
        os_path_rwunlock(&tree_lock);
    }
    if (ok && os_access(backup) == 1 && os_remove(backup) != 0)
        asynclog_warning("directory upload completed, but old backup \"%s\" could not be removed.", backup);

    char response[256];
    if (ok) {
        snprintf(response, sizeof(response), "Directory uploaded: %llu file(s), %llu directory(s), %llu bytes.",
                 files, directories, bytes);
        __send_app_response__(connection, APP_RESPONSE_OK, response);
    } else {
        snprintf(response, sizeof(response), "%s Completed %llu file(s) before rejection.",
                 failure, files);
        __send_app_response__(connection, APP_RESPONSE_ERROR, response);
    }
    if (!ok && staging[0] && os_access(staging) == 1) os_remove(staging);
    __finalize_storage_reservation__(&storage_reservation, storage_committed);
    if (staging_reserved) __release_directory_staging__();
    if (transfer_locked) os_path_unlock(&transfer_lock);
    if (hierarchy_locked) __storage_path_unlock__(&hierarchy_lock);
}

typedef struct {
    ProtocolConnection *connection;
    bool ok;
} _ServerListContext;

void app_server_list(ProtocolConnection *connection, TransportProtocolHeader *meta) {
    if (!__safe_relative_path__(meta->filename, True)) {
        byte status = APP_LIST_INTERRUPTED;
        protocol_send_all(connection, &status, sizeof(status));
        protocol_shutdown(connection);
        return;
    }

    PathRWLock tree_lock = {0};
    if (!__storage_tree_lock__(&tree_lock, False)) {
        byte status = APP_LIST_INTERRUPTED;
        protocol_send_all(connection, &status, sizeof(status));
        protocol_shutdown(connection);
        return;
    }

    byte status;
    if (strcmp(meta->filename, ".") != 0 && __storage_entry_state__(meta->filename) == 0)
        status = APP_LIST_NOT_FOUND;
    else {
        _ServerListContext ctx = { .connection = connection, .ok = True };
        int result = __storage_list_directory__(meta->filename, __app_server_list__, &ctx);
        status = result == 0 && ctx.ok ? APP_LIST_COMPLETE : APP_LIST_INTERRUPTED;
    }
    os_path_rwunlock(&tree_lock);
    protocol_send_all(connection, &status, sizeof(status));
    protocol_shutdown(connection);
}

typedef struct {
    ProtocolConnection *connection;
    const char *root;
    usize root_length;
    bool ok;
    uint64 files;
    uint64 directories;
    uint64 bytes;
} _DirectoryDownloadContext;

static bool __server_send_directory_record__(_DirectoryDownloadContext *ctx, byte type, const char *relative, char *filepath, uint64 size) {
    usize length = strlen(relative);
    if (length == 0 || length >= PATH_LENGTH_RESTRICTION || length > UINT16_MAX) return False;

    FILE *fp = NULL;
    if (type == 2) {
        fp = fopen(filepath, "rb");
        if (!fp) return False;
        int64 actual_size = os_file_size(fp);
        if (actual_size < 0) {
            fclose(fp);
            return False;
        }
        size = (uint64)actual_size;
    }

    uint16 network_length = socket_htons((uint16)length);
    uint64 network_size = socket_htonll(size);
    if (protocol_send_all(ctx->connection, &type, sizeof(type)) != 0 ||
        protocol_send_all(ctx->connection, &network_length, sizeof(network_length)) != 0 ||
        protocol_send_all(ctx->connection, &network_size, sizeof(network_size)) != 0 ||
        protocol_send_all(ctx->connection, relative, length) != 0) {
        if (fp) fclose(fp);
        return False;
    }

    if (fp) {
        SHA256Context sha256;
        sha256_init(&sha256);
        char buffer[BUFFER_CAPACITY];
        uint64 remaining = size;
        while (remaining > 0) {
            usize want = remaining > sizeof(buffer) ? sizeof(buffer) : (usize)remaining;
            usize read = fread(buffer, 1, want, fp);
            if (read != want || protocol_send_all(ctx->connection, buffer, read) != 0) {
                fclose(fp);
                return False;
            }
            sha256_update(&sha256, (unsigned char *)buffer, (unsigned int)read);
            remaining -= read;
        }
        byte digest[SHA256_DIGEST_SIZE];
        char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
        sha256_final(&sha256, digest);
        app_hex_encode(digest, sizeof(digest), hash);
        if (ferror(fp) || os_file_size(fp) != (int64)size) {
            fclose(fp);
            return False;
        }
        if (protocol_send_all(ctx->connection, hash, PROTOCOL_FILE_HASH_LENGTH) != 0) {
            fclose(fp);
            return False;
        }
        fclose(fp);
        ctx->files++;
        ctx->bytes += size;
    } else ctx->directories++;
    return True;
}

static void __server_send_directory_entry__(char *dir, char *name, bool folder, uint64 size, void *args) {
    _DirectoryDownloadContext *ctx = (_DirectoryDownloadContext *)args;
    if (!ctx->ok) return;

    char path[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path) || (usize)n <= ctx->root_length + 1) {
        ctx->ok = False;
        return;
    }
    const char *relative = path + ctx->root_length + 1;
    if (folder) {
        ctx->ok = __server_send_directory_record__(ctx, 1, relative, path, 0);
        if (ctx->ok && os_listdir(path, __server_send_directory_entry__, ctx) != 0) ctx->ok = False;
    } else ctx->ok = __server_send_directory_record__(ctx, 2, relative, path, size);
}

static void __server_send_directory__(ProtocolConnection *connection, char *path,
                                      const char *display_path) {
    uint64 marker = socket_htonll(DIRECTORY_DOWNLOAD_MARKER);
    if (protocol_send_all(connection, &marker, sizeof(marker)) != 0) return;

    _DirectoryDownloadContext ctx = {
        .connection = connection,
        .root = path,
        .root_length = strlen(path),
        .ok = True,
        .files = 0,
        .directories = 0,
        .bytes = 0
    };
    if (os_listdir(path, __server_send_directory_entry__, &ctx) != 0) ctx.ok = False;
    if (ctx.ok) {
        byte end = 0;
        protocol_send_all(connection, &end, sizeof(end));
        asynclog_info("Directory download completed: %llu file(s), %llu directory(s), %llu bytes.", ctx.files, ctx.directories, ctx.bytes);
    } else asynclog_warning("Directory download interrupted for \"%s\".", display_path);
    protocol_shutdown(connection);
}

void app_server_download(ProtocolConnection *connection, TransportProtocolHeader *meta) {
    char filepath[PATH_LENGTH_RESTRICTION];

    if (!__storage_path__(filepath, sizeof(filepath), meta->filename, False)) {
        __send_download_error__(connection);
        return;
    }

    PathRWLock tree_lock = {0};
    if (!__storage_tree_lock__(&tree_lock, False)) {
        __send_download_error__(connection);
        return;
    }
    FILE *fp = NULL;
    int64 size = -1;
    _StorageDirectory source_directory = {0};
    #if defined(__OS_UNIX__)
        source_directory.fd = -1;
    #elif defined(__OS_WINDOWS__)
        source_directory.handle = INVALID_HANDLE_VALUE;
    #endif
    bool directory = __storage_open_directory__(meta->filename, &source_directory);
    if (directory) {
        __storage_directory_close__(&source_directory);
        /* Hold the shared tree lock while streaming so no snapshot directory is needed. */
        __server_send_directory__(connection, filepath, meta->filename);
        os_path_rwunlock(&tree_lock);
        return;
    } else {
        __storage_open_regular__(meta->filename, &fp, &size);
    }
    os_path_rwunlock(&tree_lock);

    if (!fp || size < 0) {
        if (fp) fclose(fp);
        asynclog_warning("Download failed: file \"%s\" not found.", meta->filename);
        __send_download_error__(connection);
        return;
    }

    uint64 total_size = (uint64)size;

    uint64 network_total_size = socket_htonll(total_size);
    if (protocol_send_all(connection, &network_total_size, sizeof(network_total_size)) != 0) {
        fclose(fp);
        return;
    }

    uint64 network_offset;
    if (protocol_recv_all(connection, &network_offset, sizeof(network_offset)) != 0) {
        asynclog_warning("Failed to receive offset from client.");
        fclose(fp);
        return;
    }
    uint64 offset = socket_ntohll(network_offset);

    if (offset > total_size) {
        asynclog_warning("Client supplied invalid download offset %llu for %llu-byte file.", offset, total_size);
        fclose(fp);
        protocol_shutdown(connection);
        return;
    }
    SHA256Context sha256;
    sha256_init(&sha256);
    char buffer[BUFFER_CAPACITY];
    if (offset > 0 && !__hash_open_file_prefix__(&sha256, fp, offset, connection,
                                                 __send_download_hash_progress__)) {
        asynclog_warning("Failed to hash the resumed download prefix.");
        fclose(fp);
        protocol_shutdown(connection);
        return;
    }
    if (offset > 0 && os_fseek(fp, (int64)offset, SEEK_SET) != 0) {
        asynclog_warning("Failed to seek download offset.");
        fclose(fp);
        protocol_shutdown(connection);
        return;
    }

    if (offset > 0) {
        byte payload_ready = DOWNLOAD_PAYLOAD_READY;
        if (protocol_send_all(connection, &payload_ready, sizeof(payload_ready)) != 0) {
            fclose(fp);
            return;
        }
    }

    uint64 transmission_size = total_size - offset;
    asynclog_info("Sending file \"%s\" from offset %llu (%llu bytes remaining).", meta->filename, offset, transmission_size);

    uint64 remaining = transmission_size;
    while (remaining > 0) {
        usize want = (remaining > sizeof(buffer)) ? sizeof(buffer) : (usize)remaining;
        usize length = fread(buffer, 1, want, fp);
        if (length != want) {
            asynclog_warning("Failed to read the complete download payload.");
            fclose(fp);
            protocol_shutdown(connection);
            return;
        }
        sha256_update(&sha256, (unsigned char *)buffer, (unsigned int)length);
        if (protocol_send_all(connection, buffer, length) != 0) {
            asynclog_warning("Data transmission interrupted.");
            fclose(fp);
            return;
        }
        remaining -= length;
    }

    byte checksum_ready = DOWNLOAD_CHECKSUM_READY;
    if (protocol_send_all(connection, &checksum_ready, sizeof(checksum_ready)) != 0) {
        fclose(fp);
        return;
    }

    char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
    byte digest[SHA256_DIGEST_SIZE];
    sha256_final(&sha256, digest);
    app_hex_encode(digest, sizeof(digest), hash);
    if (protocol_send_all(connection, hash, PROTOCOL_FILE_HASH_LENGTH) != 0) {
        asynclog_warning("Failed to send file SHA-256.");
        fclose(fp);
        return;
    }

    asynclog_info("File \"%s\" transmission completed.", meta->filename);
    fclose(fp);
    
    protocol_shutdown(connection);
}

void app_server_remove(ProtocolConnection *connection, TransportProtocolHeader *meta) {
    char path[PATH_LENGTH_RESTRICTION];
    if (!__storage_path__(path, sizeof(path), meta->filename, False)) {
        __send_app_response__(connection, APP_RESPONSE_ERROR, "Invalid remote path.");
        return;
    }

    _StoragePathLock hierarchy_lock = {0};
    if (!__storage_path_lock__(&hierarchy_lock, path)) {
        __send_app_response__(connection, APP_RESPONSE_ERROR,
                              "Failed to lock the remote path hierarchy.");
        return;
    }

    char lock_file[PATH_LENGTH_RESTRICTION];
    PathLock transfer_lock = {0};
    if (!__transfer_lock_path__(lock_file, sizeof(lock_file), path) ||
        os_path_lock(&transfer_lock, lock_file) != 0) {
        __send_app_response__(connection, APP_RESPONSE_ERROR, "Remote path is busy.");
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }

    PathRWLock tree_lock = {0};
    if (!__storage_tree_lock__(&tree_lock, True)) {
        __send_app_response__(connection, APP_RESPONSE_ERROR,
                              "Failed to lock remote storage.");
        os_path_unlock(&transfer_lock);
        __storage_path_unlock__(&hierarchy_lock);
        return;
    }

    byte response_status = APP_RESPONSE_ERROR;
    const char *response;
    char buffer[PATH_LENGTH_RESTRICTION + sizeof("Delete \"\" successfully.")];
    _StorageQuotaReservation storage_reservation = {0};
    _StorageUsage previous_usage = {0};
    if (__storage_entry_state__(meta->filename) != 1) {
        response = "No such file or directory.";
    } else if (!__measure_remote_storage__(meta->filename, &previous_usage)) {
        response = "Failed to measure remote path before deletion.";
    } else if (!__reserve_storage_quota__(&storage_reservation, &previous_usage,
                                          &(_StorageUsage){0})) {
        response = "Failed to reserve remote storage accounting.";
    }
    else if (!__storage_remove__(meta->filename)) {
        response = "Failed to delete remote path.";
    } else {
        snprintf(buffer, sizeof(buffer), "Delete \"%s\" successfully.", meta->filename);
        response = buffer;
        response_status = APP_RESPONSE_OK;
    }
    os_path_rwunlock(&tree_lock);
    __finalize_storage_reservation__(&storage_reservation,
                                     response_status == APP_RESPONSE_OK);
    os_path_unlock(&transfer_lock);
    __storage_path_unlock__(&hierarchy_lock);
    __send_app_response__(connection, response_status, response);
}

void app_server_mkdir(ProtocolConnection *connection, TransportProtocolHeader *meta) {
    char path[PATH_LENGTH_RESTRICTION];
    _StoragePathLock hierarchy_lock = {0};
    bool ready = __storage_path__(path, sizeof(path), meta->filename, False) &&
        __storage_path_lock__(&hierarchy_lock, path);
    if (ready) {
        PathRWLock tree_lock = {0};
        if (__storage_tree_lock__(&tree_lock, True)) {
            ready = __storage_mkdir__(meta->filename);
            os_path_rwunlock(&tree_lock);
        } else ready = False;
    }
    if (hierarchy_lock.count) __storage_path_unlock__(&hierarchy_lock);
    if (!ready) {
        asynclog_warning("failed to create remote directory \"%s\".", meta->filename);
        __send_app_response__(connection, APP_RESPONSE_ERROR, "Failed to create remote directory.");
    } else __send_app_response__(connection, APP_RESPONSE_OK, "Remote directory is ready.");
}

void app_server_default(ProtocolConnection *connection) {
    __send_app_response__(connection, APP_RESPONSE_ERROR, "Unknown command.");
}

void __app_server_list__(char *dir, char *name, bool folder, uint64 size, void *args) {
    (void)dir;
    _ServerListContext *ctx = (_ServerListContext *)args;
    if (!ctx->ok) return;

    int length;
    char unit[12];
    char buffer[PATH_LENGTH_RESTRICTION];
    parse_unit_convert(size, unit, sizeof(unit));
    if (folder) length = snprintf(buffer, sizeof(buffer), "\x1b[1;36m%-12s%s\x1b[0m\n", "", name);
    else length = snprintf(buffer, sizeof(buffer), "%-12s%s\n", unit, name);
    if (length <= 0 || length >= (int)sizeof(buffer) || length > (int)UINT16_MAX) {
        ctx->ok = False;
        return;
    }

    byte type = APP_LIST_ENTRY;
    uint16 network_length = socket_htons((uint16)length);
    ctx->ok = protocol_send_all(ctx->connection, &type, sizeof(type)) == 0 &&
        protocol_send_all(ctx->connection, &network_length, sizeof(network_length)) == 0 &&
        protocol_send_all(ctx->connection, buffer, (usize)length) == 0;
}
