#include "mdf_internal.h"

#include <string.h>
#include <unistd.h>

/* Normalize a path against the process working directory without resolving
 * symlinks.  That matches how cmdf interprets relative input and output paths:
 * the generated HTML should retain a usable relative route from the requested
 * output location to the requested local asset location. */
static int mdf_normalize_absolute_path(char *dst, size_t cap, const char *path)
{
    char source[4096];
    char cwd[4096];
    const char *p;
    size_t len;
    size_t out_len;

    if (dst == NULL || cap < 2 || path == NULL || path[0] == '\0') {
        return -1;
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(source)) {
            return -1;
        }
        strcpy(source, path);
    } else {
        if (getcwd(cwd, sizeof(cwd)) == NULL ||
            strlen(cwd) + 1 + strlen(path) + 1 > sizeof(source)) {
            return -1;
        }
        strcpy(source, cwd);
        strcat(source, "/");
        strcat(source, path);
    }
    dst[0] = '/';
    out_len = 1;
    p = source;
    while (*p != '\0') {
        const char *component;

        while (*p == '/') p++;
        component = p;
        while (*p != '\0' && *p != '/') p++;
        len = (size_t)(p - component);
        if (len == 0 || (len == 1 && component[0] == '.')) {
            continue;
        }
        if (len == 2 && component[0] == '.' && component[1] == '.') {
            while (out_len > 1 && dst[out_len - 1] != '/') out_len--;
            if (out_len > 1) out_len--;
            continue;
        }
        if (out_len + (out_len > 1 ? 1 : 0) + len + 1 > cap) {
            return -1;
        }
        if (out_len > 1) dst[out_len++] = '/';
        memcpy(dst + out_len, component, len);
        out_len += len;
    }
    dst[out_len] = '\0';
    return 0;
}

/* Return target_path as a URI path relative to from_dir.  This is deliberately
 * lexical: command-line paths are interpreted relative to the invocation
 * directory, and resolving symlinks here would make the HTML reference depend
 * on a different path model than the dump operation itself. */
int mdf_path_relative_to(char *dst, size_t cap, const char *from_dir, const char *target_path)
{
    char from[4096];
    char target[4096];
    const char *from_part;
    const char *target_part;
    size_t out_len;

    if (dst == NULL || cap == 0 ||
        mdf_normalize_absolute_path(from, sizeof(from), from_dir) != 0 ||
        mdf_normalize_absolute_path(target, sizeof(target), target_path) != 0) {
        return -1;
    }
    from_part = from + 1;
    target_part = target + 1;
    while (*from_part != '\0' && *target_part != '\0') {
        const char *from_end;
        const char *target_end;
        size_t from_len;
        size_t target_len;

        from_end = strchr(from_part, '/');
        target_end = strchr(target_part, '/');
        if (from_end == NULL) from_end = from_part + strlen(from_part);
        if (target_end == NULL) target_end = target_part + strlen(target_part);
        from_len = (size_t)(from_end - from_part);
        target_len = (size_t)(target_end - target_part);
        if (from_len != target_len || strncmp(from_part, target_part, from_len) != 0) {
            break;
        }
        from_part = *from_end == '\0' ? from_end : from_end + 1;
        target_part = *target_end == '\0' ? target_end : target_end + 1;
    }
    out_len = 0;
    while (*from_part != '\0') {
        const char *from_end;

        from_end = strchr(from_part, '/');
        if (from_end == NULL) from_end = from_part + strlen(from_part);
        if (out_len + 3 + 1 > cap) return -1;
        memcpy(dst + out_len, "../", 3);
        out_len += 3;
        from_part = *from_end == '\0' ? from_end : from_end + 1;
    }
    if (*target_part != '\0') {
        size_t target_len;

        target_len = strlen(target_part);
        if (out_len + target_len + 1 > cap) return -1;
        memcpy(dst + out_len, target_part, target_len);
        out_len += target_len;
    }
    if (out_len == 0) {
        if (cap < 2) return -1;
        dst[out_len++] = '.';
    }
    dst[out_len] = '\0';
    return 0;
}

/* URI references derived from filesystem paths must not let pathname bytes
 * such as '#', '?', '%', or ':' change URL parsing. */
int mdf_path_relative_uri(char *dst, size_t cap, const char *from_dir, const char *target_path)
{
    char relative_path[4096];
    size_t i;
    size_t out_len;

    if (mdf_path_relative_to(relative_path, sizeof(relative_path), from_dir, target_path) != 0) {
        return -1;
    }
    out_len = 0;
    for (i = 0; relative_path[i] != '\0'; i++) {
        unsigned char c;

        c = (unsigned char)relative_path[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~' || c == '/') {
            if (out_len + 2 > cap) return -1;
            dst[out_len++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";

            if (out_len + 4 > cap) return -1;
            dst[out_len++] = '%';
            dst[out_len++] = hex[c >> 4];
            dst[out_len++] = hex[c & 0x0f];
        }
    }
    if (out_len >= cap) return -1;
    dst[out_len] = '\0';
    return 0;
}
