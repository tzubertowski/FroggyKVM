#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include <kni.h>
#include <ROMStructs.h>
#include <commonKNIMacros.h>

typedef jint SuiteIdType;

#define MAX_RMS_HANDLES 64
static int g_rms_fds[MAX_RMS_HANDLES] = { [0 ... MAX_RMS_HANDLES - 1] = -1 };
static char g_rms_dir[512];
#define rms_trace(...) ((void)0)

static void ensure_dir_exists(const char* dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

void gb300_rms_configure(const char* save_dir, const char* game_path) {
    const char* name = game_path ? strrchr(game_path, '/') : NULL;
    const char* extension;
    char game[192] = "default";

    name = name ? name + 1 : game_path;
    if (name && *name) {
        extension = strrchr(name, '.');
        size_t i = 0;
        while (name[i] && name + i != extension && i + 1 < sizeof(game)) {
            unsigned char c = (unsigned char)name[i];
            game[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
            i++;
        }
        game[i] = '\0';
        if (!game[0]) snprintf(game, sizeof(game), "default");
    }

    snprintf(g_rms_dir, sizeof(g_rms_dir), "%s%s%s",
             save_dir && *save_dir ? save_dir : FROGGY_SD_ROOT "/saves/j2me",
             save_dir && *save_dir && save_dir[strlen(save_dir) - 1] == '/' ? "" : "/",
             game);
    ensure_dir_exists(g_rms_dir);
}

static const char* get_rms_dir(void) {
    if (g_rms_dir[0] == '\0') {
        const char* custom = getenv("GB300_RMS_DIR");
        if (custom && custom[0]) {
            snprintf(g_rms_dir, sizeof(g_rms_dir), "%s", custom);
        } else {
            snprintf(g_rms_dir, sizeof(g_rms_dir), FROGGY_SD_ROOT "/saves/j2me/default");
        }
        ensure_dir_exists(g_rms_dir);
    }
    return g_rms_dir;
}

void gb300_rms_shutdown(void) {
    for (int i = 0; i < MAX_RMS_HANDLES; i++) {
        if (g_rms_fds[i] >= 0) {
            fsync(g_rms_fds[i]);
            close(g_rms_fds[i]);
            g_rms_fds[i] = -1;
        }
    }
}

static int get_string_utf8(jobject string_handle, char* buf, size_t buf_size) {
    if (!string_handle || KNI_IsNullHandle(string_handle)) {
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }
    jsize len = KNI_GetStringLength(string_handle);
    if (len < 0) {
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }
    jchar ubuf[256];
    if (len >= 256) len = 255;
    KNI_GetStringRegion(string_handle, 0, len, ubuf);
    size_t out_idx = 0;
    for (jsize i = 0; i < len && out_idx + 1 < buf_size; i++) {
        jchar c = ubuf[i];
        if (c < 128) {
            buf[out_idx++] = (char)c;
        } else {
            buf[out_idx++] = '_';
        }
    }
    buf[out_idx] = '\0';
    return 1;
}

static void encode_hex(const char* in, char* out, size_t out_size) {
    size_t in_len = strlen(in);
    size_t oi = 0;
    for (size_t i = 0; i < in_len && oi + 3 < out_size; i++) {
        sprintf(out + oi, "%02x", (unsigned char)in[i]);
        oi += 2;
    }
    out[oi] = '\0';
}

static int decode_hex(const char* in, char* out, size_t out_size) {
    size_t in_len = strlen(in);
    if (in_len % 2 != 0) return 0;
    size_t oi = 0;
    for (size_t i = 0; i < in_len && oi + 1 < out_size; i += 2) {
        unsigned int byte_val = 0;
        if (sscanf(in + i, "%02x", &byte_val) != 1) return 0;
        out[oi++] = (char)byte_val;
    }
    out[oi] = '\0';
    return 1;
}

static int build_store_path(jint suiteId, jobject name_h, int extension, char* path, size_t path_size, char* out_name, size_t out_name_size) {
    char name_utf8[256];
    if (!get_string_utf8(name_h, name_utf8, sizeof(name_utf8))) {
        return 0;
    }
    if (out_name && out_name_size > 0) {
        strncpy(out_name, name_utf8, out_name_size - 1);
        out_name[out_name_size - 1] = '\0';
    }
    char name_hex[512];
    encode_hex(name_utf8, name_hex, sizeof(name_hex));
    snprintf(path, path_size, "%s/suite_%d_%s_%d.rms", get_rms_dir(), (int)suiteId, name_hex, extension);
    return 1;
}

static int alloc_rms_handle(int fd) {
    for (int i = 0; i < MAX_RMS_HANDLES; i++) {
        if (g_rms_fds[i] < 0) {
            g_rms_fds[i] = fd;
            return i + 1; // 1-based handle
        }
    }
    return -1;
}

static int get_rms_fd(int handle) {
    if (handle < 1 || handle > MAX_RMS_HANDLES) {
        return -1;
    }
    return g_rms_fds[handle - 1];
}

static int is_rms_file(const char* name, const char* prefix, size_t prefix_len) {
    size_t len = strlen(name);
    return len > prefix_len + 4 &&
           strncmp(name, prefix, prefix_len) == 0 &&
           strcasecmp(name + len - 4, ".rms") == 0;
}

static int count_record_stores(jint suiteId) {
    DIR* d = opendir(get_rms_dir());
    if (!d) return 0;

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "suite_%d_", (int)suiteId);
    size_t prefix_len = strlen(prefix);

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        rms_trace("[RMS] scan entry='%s' prefix='%s' match=%d\\n",
                  ent->d_name, prefix, is_rms_file(ent->d_name, prefix, prefix_len));
        if (is_rms_file(ent->d_name, prefix, prefix_len)) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", get_rms_dir(), ent->d_name);
            struct stat st;
            int stat_result = stat(full_path, &st);
            rms_trace("[RMS] scan stat='%s' result=%d mode=%o size=%ld errno=%d\\n",
                      full_path, stat_result, stat_result == 0 ? (unsigned)(st.st_mode & S_IFMT) : 0,
                      stat_result == 0 ? (long)st.st_size : -1L, stat_result == 0 ? 0 : errno);
            if (stat_result == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                count++;
        }
    }
    closedir(d);
    return count;
}

static void fill_record_stores_list(jint suiteId, jobjectArray names, jobject tempStr, jsize max_count) {
    DIR* d = opendir(get_rms_dir());
    if (!d) return;

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "suite_%d_", (int)suiteId);
    size_t prefix_len = strlen(prefix);

    struct dirent* ent;
    jsize idx = 0;
    while ((ent = readdir(d)) != NULL && idx < max_count) {
        if (!is_rms_file(ent->d_name, prefix, prefix_len))
            continue;

        size_t len = strlen(ent->d_name);
        size_t encoded_len = len - 4 - prefix_len;
        if (encoded_len < 3 ||
            ent->d_name[prefix_len + encoded_len - 2] != '_' ||
            ent->d_name[prefix_len + encoded_len - 1] != '0')
            continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", get_rms_dir(), ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
            continue;

        char hex_str[512];
        size_t hex_len = encoded_len - 2;
        if (hex_len < sizeof(hex_str)) {
            memcpy(hex_str, ent->d_name + prefix_len, hex_len);
            hex_str[hex_len] = '\0';
            char decoded_name[256];
            if (decode_hex(hex_str, decoded_name, sizeof(decoded_name))) {
                KNI_NewStringUTF(decoded_name, tempStr);
                KNI_SetObjectArrayElement(names, idx, tempStr);
                idx++;
            }
        }
    }
    closedir(d);
}

static void delete_all_record_stores(jint suiteId) {
    DIR* d = opendir(get_rms_dir());
    if (!d) return;

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "suite_%d_", (int)suiteId);
    size_t prefix_len = strlen(prefix);

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".rms") == 0) {
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", get_rms_dir(), ent->d_name);
                unlink(full_path);
            }
        }
    }
    closedir(d);
}

/* ========================================================================= */
/* KNI Native Implementations                                                */
/* ========================================================================= */

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_rms_RecordStoreFile_openRecordStoreFile) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    int extension = KNI_GetParameterAsInt(3);
    char path[512] = {0};
    char name_utf8[256] = {0};
    int handle = -1;
    int open_errno = 0;

    KNI_StartHandles(1);
    KNI_DeclareHandle(name_h);
    KNI_GetParameterAsObject(2, name_h);

    if (build_store_path(suiteId, name_h, extension, path, sizeof(path), name_utf8, sizeof(name_utf8))) {
        int fd = open(path, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            handle = alloc_rms_handle(fd);
            if (handle < 0) {
                close(fd);
            }
        } else {
            open_errno = errno;
        }
    } else {
        open_errno = EINVAL;
    }
    KNI_EndHandles();

    rms_trace("[RMS] openRecordStoreFile(suite=%d, name='%s', ext=%d, path='%s') -> handle=%d (fd=%d, errno=%d: %s)\n",
            (int)suiteId, name_utf8, extension, path, handle, handle > 0 ? get_rms_fd(handle) : -1,
            open_errno, open_errno ? strerror(open_errno) : "ok");

    if (handle < 0) {
        KNI_ThrowNew("java/io/IOException", "Cannot open record store file");
        KNI_ReturnInt(-1);
    }

    KNI_ReturnInt((jint)handle);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_rms_RecordStoreFile_spaceAvailableRecordStore) {
    KNI_ReturnInt((jint)(16 * 1024 * 1024));
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_rms_RecordStoreFile_spaceAvailableNewRecordStore) {
    KNI_ReturnInt((jint)(16 * 1024 * 1024));
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_setPosition) {
    int handle = KNI_GetParameterAsInt(1);
    int pos = KNI_GetParameterAsInt(2);
    int fd = get_rms_fd(handle);

    off_t result = fd < 0 ? (off_t)-1 : lseek(fd, (off_t)pos, SEEK_SET);
    rms_trace("[RMS] seek(handle=%d, pos=%d) -> %ld errno=%d\n",
              handle, pos, (long)result, result < 0 ? errno : 0);
    if (result == (off_t)-1) {
        KNI_ThrowNew("java/io/IOException", "lseek failed on record store file");
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_writeBytes) {
    int handle = KNI_GetParameterAsInt(1);
    int offset = KNI_GetParameterAsInt(3);
    int length = KNI_GetParameterAsInt(4);
    int fd = get_rms_fd(handle);

    if (fd < 0) {
        KNI_ThrowNew("java/io/IOException", "Invalid record store handle");
        KNI_ReturnVoid();
    }

    if (length == 0) {
        KNI_ReturnVoid();
    }

    ssize_t written = 0;
    KNI_StartHandles(1);
    KNI_DeclareHandle(buffer);
    KNI_GetParameterAsObject(2, buffer);

    jbyte* data = JavaByteArray(buffer);
    written = write(fd, data + offset, (size_t)length);
    KNI_EndHandles();

    rms_trace("[RMS] write(handle=%d, offset=%d, length=%d) -> %ld errno=%d\n",
              handle, offset, length, (long)written, written < 0 ? errno : 0);
    if (written < 0 || written != length) {
        KNI_ThrowNew("java/io/IOException", "write failed on record store file");
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_commitWrite) {
    int handle = KNI_GetParameterAsInt(1);
    int fd = get_rms_fd(handle);
    if (fd >= 0) {
#ifndef SF2000
        fsync(fd);
#endif
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_rms_RecordStoreFile_readBytes) {
    int handle = KNI_GetParameterAsInt(1);
    int offset = KNI_GetParameterAsInt(3);
    int length = KNI_GetParameterAsInt(4);
    int fd = get_rms_fd(handle);

    if (fd < 0) {
        KNI_ThrowNew("java/io/IOException", "Invalid record store handle");
        KNI_ReturnInt(0);
    }

    if (length == 0) {
        KNI_ReturnInt(0);
    }

    ssize_t bytesRead = 0;
    KNI_StartHandles(1);
    KNI_DeclareHandle(buffer);
    KNI_GetParameterAsObject(2, buffer);

    jbyte* data = JavaByteArray(buffer);
    bytesRead = read(fd, data + offset, (size_t)length);
    KNI_EndHandles();

    rms_trace("[RMS] read(handle=%d, offset=%d, length=%d) -> %ld errno=%d\n",
              handle, offset, length, (long)bytesRead, bytesRead < 0 ? errno : 0);
    if (bytesRead < 0) {
        KNI_ThrowNew("java/io/IOException", "read failed on record store file");
        KNI_ReturnInt(0);
    } else if (bytesRead == 0) {
        KNI_ReturnInt(-1); // Java EOF
    }

    KNI_ReturnInt((jint)bytesRead);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_closeFile) {
    int handle = KNI_GetParameterAsInt(1);
    rms_trace("[RMS] closeFile(handle=%d)\n", handle);
    if (handle >= 1 && handle <= MAX_RMS_HANDLES) {
        int idx = handle - 1;
        if (g_rms_fds[idx] >= 0) {
            fsync(g_rms_fds[idx]);
            close(g_rms_fds[idx]);
            g_rms_fds[idx] = -1;
        }
    }
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_truncateFile) {
    int handle = KNI_GetParameterAsInt(1);
    int size = KNI_GetParameterAsInt(2);
    int fd = get_rms_fd(handle);

#ifndef SF2000
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        KNI_ThrowNew("java/io/IOException", "ftruncate failed on record store file");
    }
#else
    if (fd < 0) {
        KNI_ThrowNew("java/io/IOException", "Invalid record store file");
    }
    lseek(fd, (off_t)size, SEEK_SET);
#endif
    KNI_ReturnVoid();
}

struct Java_com_sun_midp_rms_RecordStoreFile {
    void* __do_not_use__;
    jint handle;
};

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_finalize) {
    KNI_StartHandles(1);
    KNI_DeclareHandle(this_h);
    KNI_GetThisPointer(this_h);
    if (!KNI_IsNullHandle(this_h)) {
        struct Java_com_sun_midp_rms_RecordStoreFile* f =
            (struct Java_com_sun_midp_rms_RecordStoreFile*)unhand(struct Java_com_sun_midp_rms_RecordStoreFile, this_h);
        if (f && f->handle >= 1 && f->handle <= MAX_RMS_HANDLES) {
            int idx = f->handle - 1;
            if (g_rms_fds[idx] >= 0) {
                fsync(g_rms_fds[idx]);
                close(g_rms_fds[idx]);
                g_rms_fds[idx] = -1;
            }
            f->handle = -1;
        }
    }
    KNI_EndHandles();
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_midp_rms_RecordStoreFile_getNumberOfStores) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    int count = count_record_stores(suiteId);
    rms_trace("[RMS] getNumberOfStores(suite=%d) -> %d\n", (int)suiteId, count);
    KNI_ReturnInt((jint)count);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_getRecordStoreList) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    KNI_StartHandles(2);
    KNI_DeclareHandle(names);
    KNI_DeclareHandle(tempStr);
    KNI_GetParameterAsObject(2, names);

    jsize arrayLen = KNI_GetArrayLength(names);
    fill_record_stores_list(suiteId, names, tempStr, arrayLen);

    KNI_EndHandles();
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreFile_removeRecordStores) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    rms_trace("[RMS] removeRecordStores(suite=%d)\n", (int)suiteId);
    delete_all_record_stores(suiteId);
    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_rms_RecordStoreUtil_exists) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    int extension = KNI_GetParameterAsInt(3);
    char path[512];
    char name_utf8[256] = {0};
    jboolean exists = KNI_FALSE;

    KNI_StartHandles(1);
    KNI_DeclareHandle(name_h);
    KNI_GetParameterAsObject(2, name_h);

    if (build_store_path(suiteId, name_h, extension, path, sizeof(path), name_utf8, sizeof(name_utf8))) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            exists = KNI_TRUE;
        }
    }
    KNI_EndHandles();

    rms_trace("[RMS] RecordStoreUtil.exists(suite=%d, name='%s', ext=%d) -> %d\n",
            (int)suiteId, name_utf8, extension, exists);

    KNI_ReturnBoolean(exists);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_midp_rms_RecordStoreUtil_deleteFile) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    int extension = KNI_GetParameterAsInt(3);
    char path[512];
    char name_utf8[256] = {0};

    KNI_StartHandles(1);
    KNI_DeclareHandle(name_h);
    KNI_GetParameterAsObject(2, name_h);

    if (build_store_path(suiteId, name_h, extension, path, sizeof(path), name_utf8, sizeof(name_utf8))) {
        unlink(path);
    }
    KNI_EndHandles();

    rms_trace("[RMS] RecordStoreUtil.deleteFile(suite=%d, name='%s', ext=%d)\n",
            (int)suiteId, name_utf8, extension);

    KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_rms_RecordStoreFactory_suiteHasRmsData) {
    SuiteIdType suiteId = KNI_GetParameterAsInt(1);
    int count = count_record_stores(suiteId);
    KNI_ReturnBoolean(count > 0 ? KNI_TRUE : KNI_FALSE);
}
