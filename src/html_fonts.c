#include "libmdf/mdf.h"
#include "mdf_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "html_embedded/jetbrains_regular.h"
#include "html_embedded/jetbrains_italic.h"

/* These immutable upstream variable WOFF2 files are byte-identical to the
 * embedded arrays below.  The previous v2.304 static Regular/Italic URLs lost
 * the weight axis, so external HTML could not render real 700-weight headings.
 * SHA-256: regular 31ec365b93e4bad6f202ce23352a56d01ca4462b2afc782ed2cf6fa42ca9ac0e
 *          italic  76a805b6ea613ce2e3973f1bac6fa29db23116b2881390b59247d22890844ecc */
#define MDF_JETBRAINS_MONO_REGULAR_URL \
    "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/02bb50b082dad9ef8a0f33ac393839202b760223/fonts/webfonts/JetBrainsMono%5Bwght%5D.woff2"
#define MDF_JETBRAINS_MONO_ITALIC_URL \
    "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/02bb50b082dad9ef8a0f33ac393839202b760223/fonts/webfonts/JetBrainsMono-Italic%5Bwght%5D.woff2"

void mdf_html_jetbrains_mono_font(mdf_html_font *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = "JetBrains Mono";
    out->regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
    out->regular.data = libmdf_html_jetbrains_regular;
    out->regular.data_len = (size_t)libmdf_html_jetbrains_regular_len;
    out->italic.format = MDF_HTML_FONT_FORMAT_WOFF2;
    out->italic.data = libmdf_html_jetbrains_italic;
    out->italic.data_len = (size_t)libmdf_html_jetbrains_italic_len;
}

static mdf_status mdf_write_html_font_file(const char *path,
                                           const unsigned char *data,
                                           size_t data_len,
                                           int force)
{
    FILE *fp;
    struct stat st;
    int fd;

    if (path == NULL || path[0] == '\0') {
        return MDF_ERROR_INVALID;
    }
    if (force) {
        /* Do not let force mode turn a font dump into a write to a FIFO,
         * device, or a symlink target.  Opening without O_TRUNC lets us prove
         * the opened object is a regular file before changing its contents.
         * O_NONBLOCK also keeps a raced FIFO from waiting for a reader. */
        if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
            return MDF_ERROR_IO;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0666);
        if (fd < 0) {
            return MDF_ERROR_IO;
        }
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || ftruncate(fd, 0) != 0) {
            close(fd);
            return MDF_ERROR_IO;
        }
        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            close(fd);
            return MDF_ERROR_IO;
        }
    } else {
        /* O_EXCL makes preservation a single filesystem operation: a file
         * that appears after this call starts cannot be truncated here. */
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            if (errno == EEXIST && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                return MDF_OK;
            }
            return MDF_ERROR_IO;
        }
        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            close(fd);
            return MDF_ERROR_IO;
        }
    }
    if (fp == NULL) {
        return MDF_ERROR_IO;
    }
    if (fwrite(data, 1, data_len, fp) != data_len) {
        fclose(fp);
        return MDF_ERROR_IO;
    }
    if (fclose(fp) != 0) {
        return MDF_ERROR_IO;
    }
    return MDF_OK;
}

/* Compare either final file identities or, for new files, the resolved parent
 * directory identity plus basename.  The latter catches aliases such as
 * "fonts/face.woff2" and "fonts/./face.woff2" before either file is made.
 * The dump operation repeats this identity check after its first write: only
 * then can a case-insensitive filesystem reveal aliases between two absent
 * names that differ solely by case. */
static int mdf_html_font_parent_stat(const char *path,
                                     struct stat *st,
                                     const char **basename_out)
{
    const char *slash;
    size_t parent_len;
    char parent[4096];

    slash = strrchr(path, '/');
    if (slash == NULL) {
        parent[0] = '.';
        parent[1] = '\0';
        *basename_out = path;
    } else {
        *basename_out = slash + 1;
        if (slash == path) {
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            parent_len = (size_t)(slash - path);
            if (parent_len >= sizeof(parent)) {
                return -1;
            }
            memcpy(parent, path, parent_len);
            parent[parent_len] = '\0';
        }
    }
    if ((*basename_out)[0] == '\0') {
        return -1;
    }
    return stat(parent, st) == 0 ? 0 : -1;
}

int mdf_paths_alias(const char *regular_path, const char *italic_path)
{
    struct stat regular_st;
    struct stat italic_st;
    const char *regular_basename;
    const char *italic_basename;
    int regular_exists;
    int italic_exists;

    if (regular_path == NULL || italic_path == NULL ||
        regular_path[0] == '\0' || italic_path[0] == '\0') {
        return 0;
    }
    /* Exact non-empty path equality is an alias even when the destination and
     * its parent do not exist yet, so it must not depend on stat(). */
    if (strcmp(regular_path, italic_path) == 0) {
        return 1;
    }
    regular_exists = stat(regular_path, &regular_st) == 0;
    italic_exists = stat(italic_path, &italic_st) == 0;
    if (regular_exists && italic_exists) {
        return regular_st.st_dev == italic_st.st_dev && regular_st.st_ino == italic_st.st_ino;
    }
    if (regular_exists || italic_exists ||
        mdf_html_font_parent_stat(regular_path, &regular_st, &regular_basename) != 0 ||
        mdf_html_font_parent_stat(italic_path, &italic_st, &italic_basename) != 0) {
        return 0;
    }
    return regular_st.st_dev == italic_st.st_dev &&
           regular_st.st_ino == italic_st.st_ino &&
           strcmp(regular_basename, italic_basename) == 0;
}

static mdf_status mdf_dump_html_jetbrains_mono_font_impl(const char *regular_path,
                                                         const char *italic_path,
                                                         int force)
{
    mdf_status st;

    if (regular_path == NULL || regular_path[0] == '\0' ||
        italic_path == NULL || italic_path[0] == '\0' ||
        strcmp(regular_path, italic_path) == 0 ||
        mdf_paths_alias(regular_path, italic_path)) {
        return MDF_ERROR_INVALID;
    }
    st = mdf_write_html_font_file(regular_path,
                                  libmdf_html_jetbrains_regular,
                                  (size_t)libmdf_html_jetbrains_regular_len,
                                  force);
    if (st != MDF_OK) {
        return st;
    }
    /* A case-insensitive filesystem can resolve two initially absent names to
     * one file only after the first create.  Refuse it before the italic write
     * can preserve or overwrite the regular face through that alias. */
    if (mdf_paths_alias(regular_path, italic_path)) {
        return MDF_ERROR_INVALID;
    }
    return mdf_write_html_font_file(italic_path,
                                    libmdf_html_jetbrains_italic,
                                    (size_t)libmdf_html_jetbrains_italic_len,
                                    force);
}

mdf_status mdf_dump_html_jetbrains_mono_font(const char *regular_path,
                                             const char *italic_path)
{
    return mdf_dump_html_jetbrains_mono_font_impl(regular_path, italic_path, 0);
}

mdf_status mdf_dump_html_jetbrains_mono_font_force(const char *regular_path,
                                                   const char *italic_path)
{
    return mdf_dump_html_jetbrains_mono_font_impl(regular_path, italic_path, 1);
}

static int mdf_html_font_path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t len;
    int need_slash;

    if (dst == NULL || cap == 0 || dir == NULL || dir[0] == '\0' || name == NULL) {
        return -1;
    }
    len = strlen(dir);
    need_slash = dir[len - 1] != '/';
    if (len + (size_t)need_slash + strlen(name) + 1 > cap) {
        return -1;
    }
    memcpy(dst, dir, len);
    if (need_slash) {
        dst[len++] = '/';
    }
    strcpy(dst + len, name);
    return 0;
}

static mdf_status mdf_dump_html_jetbrains_mono_font_to_paths_impl(const char *font_path,
                                                                   const char *regular_path,
                                                                   const char *italic_path,
                                                                   int force)
{
    char regular_path_buf[4096];
    char italic_path_buf[4096];

    if (font_path != NULL) {
        if (mdf_html_font_path_join(regular_path_buf, sizeof(regular_path_buf),
                                    font_path, "JetBrainsMono-Regular.woff2") != 0 ||
            mdf_html_font_path_join(italic_path_buf, sizeof(italic_path_buf),
                                    font_path, "JetBrainsMono-Italic.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        if (regular_path == NULL) {
            regular_path = regular_path_buf;
        }
        if (italic_path == NULL) {
            italic_path = italic_path_buf;
        }
    }
    return mdf_dump_html_jetbrains_mono_font_impl(regular_path, italic_path, force);
}

mdf_status mdf_dump_html_jetbrains_mono_font_to_paths(const char *font_path,
                                                      const char *regular_path,
                                                      const char *italic_path)
{
    return mdf_dump_html_jetbrains_mono_font_to_paths_impl(font_path, regular_path, italic_path, 0);
}

mdf_status mdf_dump_html_jetbrains_mono_font_to_paths_force(const char *font_path,
                                                            const char *regular_path,
                                                            const char *italic_path)
{
    return mdf_dump_html_jetbrains_mono_font_to_paths_impl(font_path, regular_path, italic_path, 1);
}

static mdf_status mdf_html_font_uri_copy(mdf_impl *impl,
                                         char **dst,
                                         size_t *dst_cap,
                                         const char *src)
{
    size_t len;
    char *copy;

    len = strlen(src);
    if (len + 1 < len) {
        return MDF_ERROR_NOMEM;
    }
    copy = (char *)mdf_realloc_mem(&impl->allocator, NULL, 0, len + 1);
    if (copy == NULL) {
        return MDF_ERROR_NOMEM;
    }
    memcpy(copy, src, len + 1);
    *dst = copy;
    *dst_cap = len + 1;
    return MDF_OK;
}

static int mdf_html_font_ascii_equal_n(const char *a, const char *b, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char ca;
        unsigned char cb;

        ca = (unsigned char)a[i];
        cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

static int mdf_html_font_uri_has_scheme(const char *uri)
{
    size_t i;
    unsigned char c;

    if (uri == NULL ||
        !((uri[0] >= 'A' && uri[0] <= 'Z') || (uri[0] >= 'a' && uri[0] <= 'z'))) {
        return 0;
    }
    for (i = 1; uri[i] != '\0'; i++) {
        c = (unsigned char)uri[i];
        if (c == ':') {
            return 1;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
            return 0;
        }
    }
    return 0;
}

/* A font URI must name a resource. Empty, query-only, and fragment-only values
 * are rejected instead of becoming document-root references or implicit root
 * filesystem dump destinations. */
static int mdf_html_font_uri_is_invalid(const char *uri)
{
    return uri != NULL && (uri[0] == '\0' || uri[0] == '?' || uri[0] == '#');
}

/* HTML references interpret query and fragment delimiters as URI suffixes,
 * including relative and root-relative bases. Dump destination resolution is
 * deliberately separate, so local filesystem paths retain their own rules. */
static int mdf_html_font_uri_join(char *dst, size_t cap, const char *base, const char *name)
{
    const char *suffix;
    size_t base_len;
    size_t name_len;
    int need_slash;

    suffix = strpbrk(base, "?#");
    base_len = suffix == NULL ? strlen(base) : (size_t)(suffix - base);
    name_len = strlen(name);
    need_slash = base_len == 0 || base[base_len - 1] != '/';
    if (base_len + (size_t)need_slash + name_len + (suffix == NULL ? 0 : strlen(suffix)) + 1 > cap) {
        return -1;
    }
    memcpy(dst, base, base_len);
    if (need_slash) dst[base_len++] = '/';
    memcpy(dst + base_len, name, name_len);
    base_len += name_len;
    if (suffix != NULL) strcpy(dst + base_len, suffix);
    else dst[base_len] = '\0';
    return 0;
}

static int mdf_html_font_dump_uri_join(char *dst, size_t cap, const char *base, const char *name)
{
    return mdf_html_font_uri_join(dst, cap, base, name);
}

static int mdf_html_font_hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Resolve a local filesystem path or file:// URI. Remote and other URI schemes
 * are deliberately rejected: dump targets must name local files. */
static int mdf_html_font_uri_to_path(char *dst, size_t cap, const char *uri)
{
    const char *src;
    const char *end;
    size_t out_len;
    int decode_percent;

    if (dst == NULL || cap == 0 || uri == NULL || uri[0] == '\0') {
        return -1;
    }
    src = uri;
    decode_percent = 0;
    if (strlen(uri) >= 7 && mdf_html_font_ascii_equal_n(uri, "file://", 7)) {
        src = uri + 7;
        decode_percent = 1;
        end = strpbrk(src, "?#");
        if (end == NULL) end = src + strlen(src);
        if ((size_t)(end - src) >= 10 && mdf_html_font_ascii_equal_n(src, "localhost/", 10)) {
            src += 9;
        } else if (src[0] != '/') {
            return -1;
        }
    } else {
        if (mdf_html_font_uri_has_scheme(uri)) {
            return -1;
        }
        end = src + strlen(src);
    }
    if (!decode_percent) {
        out_len = strlen(src);
        if (out_len == 0 || out_len >= cap) {
            return -1;
        }
        memcpy(dst, src, out_len + 1);
        return 0;
    }
    out_len = 0;
    while (src < end) {
        unsigned char c;

        if (*src == '%' && src + 2 < end) {
            int hi;
            int lo;

            hi = mdf_html_font_hex_value((unsigned char)src[1]);
            lo = mdf_html_font_hex_value((unsigned char)src[2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            c = (unsigned char)((hi << 4) | lo);
            src += 3;
        } else {
            c = (unsigned char)*src++;
        }
        if (c == '\0' || out_len + 1 >= cap) {
            return -1;
        }
        dst[out_len++] = (char)c;
    }
    if (out_len == 0) {
        return -1;
    }
    dst[out_len] = '\0';
    return 0;
}

/* Convert a configured font reference to a dump path. URI suffixes select the
 * fetched representation, not a filesystem pathname component. Explicit dump
 * paths use mdf_html_font_uri_to_path directly and keep literal delimiters. */
static int mdf_html_font_reference_to_path(char *dst, size_t cap, const char *uri)
{
    const char *suffix;
    char path[4096];
    size_t len;

    suffix = strpbrk(uri, "?#");
    if (suffix == NULL) {
        return mdf_html_font_uri_to_path(dst, cap, uri);
    }
    len = (size_t)(suffix - uri);
    if (len == 0 || len >= sizeof(path)) {
        return -1;
    }
    memcpy(path, uri, len);
    path[len] = '\0';
    return mdf_html_font_uri_to_path(dst, cap, path);
}

mdf_status mdf_html_font_resolve_dump_paths(const mdf_options *opts,
                                            char *regular_path,
                                            size_t regular_path_cap,
                                            char *italic_path,
                                            size_t italic_path_cap)
{
    const char *regular_uri;
    const char *italic_uri;
    const char *regular_dst;
    const char *italic_dst;
    char regular_uri_buf[4096];
    char italic_uri_buf[4096];
    char dump_dir_buf[4096];

    if (opts == NULL || regular_path == NULL || regular_path_cap == 0 ||
        italic_path == NULL || italic_path_cap == 0 ||
        !(opts->html_dump_font || opts->html_dump_font_force)) {
        return MDF_ERROR_INVALID;
    }
    regular_uri = opts->html_font_regular_uri;
    italic_uri = opts->html_font_italic_uri;
    if (mdf_html_font_uri_is_invalid(regular_uri) ||
        mdf_html_font_uri_is_invalid(italic_uri) ||
        mdf_html_font_uri_is_invalid(opts->html_font_uri)) {
        return MDF_ERROR_INVALID;
    }
    regular_dst = opts->html_font_dump_regular_path;
    italic_dst = opts->html_font_dump_italic_path;
    if ((regular_dst != NULL && regular_dst[0] == '\0') ||
        (italic_dst != NULL && italic_dst[0] == '\0')) {
        return MDF_ERROR_INVALID;
    }
    if (opts->html_font_dump_path != NULL) {
        if (mdf_html_font_uri_to_path(dump_dir_buf, sizeof(dump_dir_buf),
                                      opts->html_font_dump_path) != 0) {
            return MDF_ERROR_INVALID;
        }
        if (regular_dst == NULL &&
            mdf_html_font_path_join(regular_path, regular_path_cap,
                                    dump_dir_buf, "JetBrainsMono-Regular.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        if (regular_dst == NULL) {
            regular_dst = regular_path;
        }
        if (italic_dst == NULL &&
            mdf_html_font_path_join(italic_path, italic_path_cap,
                                    dump_dir_buf, "JetBrainsMono-Italic.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        if (italic_dst == NULL) {
            italic_dst = italic_path;
        }
    }
    if (regular_dst == NULL) {
        if (regular_uri == NULL && opts->html_font_uri != NULL) {
            if (mdf_html_font_dump_uri_join(regular_uri_buf, sizeof(regular_uri_buf),
                                            opts->html_font_uri,
                                            "JetBrainsMono-Regular.woff2") != 0) {
                return MDF_ERROR_INVALID;
            }
            regular_uri = regular_uri_buf;
        }
        if (regular_uri == NULL ||
            mdf_html_font_reference_to_path(regular_path, regular_path_cap, regular_uri) != 0) {
            return MDF_ERROR_INVALID;
        }
    } else if (regular_dst != regular_path) {
        if (strlen(regular_dst) + 1 > regular_path_cap) {
            return MDF_ERROR_INVALID;
        }
        strcpy(regular_path, regular_dst);
    }
    if (italic_dst == NULL) {
        if (italic_uri == NULL && opts->html_font_uri != NULL) {
            if (mdf_html_font_dump_uri_join(italic_uri_buf, sizeof(italic_uri_buf),
                                            opts->html_font_uri,
                                            "JetBrainsMono-Italic.woff2") != 0) {
                return MDF_ERROR_INVALID;
            }
            italic_uri = italic_uri_buf;
        }
        if (italic_uri == NULL ||
            mdf_html_font_reference_to_path(italic_path, italic_path_cap, italic_uri) != 0) {
            return MDF_ERROR_INVALID;
        }
    } else if (italic_dst != italic_path) {
        if (strlen(italic_dst) + 1 > italic_path_cap) {
            return MDF_ERROR_INVALID;
        }
        strcpy(italic_path, italic_dst);
    }
    return MDF_OK;
}

mdf_status mdf_configure_html_font(mdf_impl *impl)
{
    const char *regular_uri;
    const char *italic_uri;
    const char *dump_regular_path;
    const char *dump_italic_path;
    mdf_html_font_source source;
    mdf_status st;
    char regular_uri_buf[4096];
    char italic_uri_buf[4096];
    char dump_dir_buf[4096];
    char dump_regular_buf[4096];
    char dump_italic_buf[4096];
    int dump_requested;

    if (impl == NULL ||
        (impl->format != MDF_FORMAT_HTML && impl->format != MDF_FORMAT_HTML_DECK)) {
        return MDF_ERROR_INVALID;
    }
    source = impl->opts.html_font_source;
    if (source != MDF_HTML_FONT_SOURCE_EMBEDDED && source != MDF_HTML_FONT_SOURCE_EXTERNAL) {
        return MDF_ERROR_INVALID;
    }
    regular_uri = impl->opts.html_font_regular_uri;
    italic_uri = impl->opts.html_font_italic_uri;
    if (mdf_html_font_uri_is_invalid(regular_uri) ||
        mdf_html_font_uri_is_invalid(italic_uri) ||
        mdf_html_font_uri_is_invalid(impl->opts.html_font_uri)) {
        return MDF_ERROR_INVALID;
    }
    dump_requested = impl->opts.html_dump_font || impl->opts.html_dump_font_force;
    if (regular_uri != NULL || italic_uri != NULL || impl->opts.html_font_uri != NULL || dump_requested) {
        source = MDF_HTML_FONT_SOURCE_EXTERNAL;
    }
    if (dump_requested && impl->opts.html_font_dump_path != NULL) {
        if (mdf_html_font_uri_to_path(dump_dir_buf, sizeof(dump_dir_buf),
                                      impl->opts.html_font_dump_path) != 0 ||
            mdf_html_font_path_join(dump_regular_buf, sizeof(dump_regular_buf),
                                    dump_dir_buf, "JetBrainsMono-Regular.woff2") != 0 ||
            mdf_html_font_path_join(dump_italic_buf, sizeof(dump_italic_buf),
                                    dump_dir_buf, "JetBrainsMono-Italic.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        dump_regular_path = impl->opts.html_font_dump_regular_path != NULL ?
            impl->opts.html_font_dump_regular_path : dump_regular_buf;
        dump_italic_path = impl->opts.html_font_dump_italic_path != NULL ?
            impl->opts.html_font_dump_italic_path : dump_italic_buf;
        if (regular_uri == NULL && impl->opts.html_font_uri == NULL &&
            impl->opts.html_font_dump_regular_path != NULL) {
            regular_uri = impl->opts.html_font_dump_regular_path;
        } else if (regular_uri == NULL && impl->opts.html_font_uri == NULL) {
            if (mdf_html_font_path_join(regular_uri_buf, sizeof(regular_uri_buf),
                                        impl->opts.html_font_dump_path,
                                        "JetBrainsMono-Regular.woff2") != 0) {
                return MDF_ERROR_INVALID;
            }
            regular_uri = regular_uri_buf;
        }
        if (italic_uri == NULL && impl->opts.html_font_uri == NULL &&
            impl->opts.html_font_dump_italic_path != NULL) {
            italic_uri = impl->opts.html_font_dump_italic_path;
        } else if (italic_uri == NULL && impl->opts.html_font_uri == NULL) {
            if (mdf_html_font_path_join(italic_uri_buf, sizeof(italic_uri_buf),
                                        impl->opts.html_font_dump_path,
                                        "JetBrainsMono-Italic.woff2") != 0) {
                return MDF_ERROR_INVALID;
            }
            italic_uri = italic_uri_buf;
        }
    } else {
        dump_regular_path = NULL;
        dump_italic_path = NULL;
        if (dump_requested) {
            dump_regular_path = impl->opts.html_font_dump_regular_path;
            dump_italic_path = impl->opts.html_font_dump_italic_path;
            if (regular_uri == NULL && impl->opts.html_font_uri == NULL &&
                dump_regular_path != NULL) {
                regular_uri = dump_regular_path;
            }
            if (italic_uri == NULL && impl->opts.html_font_uri == NULL &&
                dump_italic_path != NULL) {
                italic_uri = dump_italic_path;
            }
        }
    }
    if (regular_uri == NULL && impl->opts.html_font_uri != NULL) {
        if (mdf_html_font_uri_join(regular_uri_buf, sizeof(regular_uri_buf),
                                   impl->opts.html_font_uri,
                                   "JetBrainsMono-Regular.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        regular_uri = regular_uri_buf;
    }
    if (italic_uri == NULL && impl->opts.html_font_uri != NULL) {
        if (mdf_html_font_uri_join(italic_uri_buf, sizeof(italic_uri_buf),
                                   impl->opts.html_font_uri,
                                   "JetBrainsMono-Italic.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        italic_uri = italic_uri_buf;
    }
    if (dump_requested) {
        st = mdf_html_font_resolve_dump_paths(&impl->opts,
                                              dump_regular_buf, sizeof(dump_regular_buf),
                                              dump_italic_buf, sizeof(dump_italic_buf));
        if (st != MDF_OK) {
            return st;
        }
        st = mdf_dump_html_jetbrains_mono_font_impl(dump_regular_buf, dump_italic_buf,
                                                    impl->opts.html_dump_font_force);
        if (st != MDF_OK) {
            return st;
        }
    }
    if (source == MDF_HTML_FONT_SOURCE_EMBEDDED) {
        return MDF_OK;
    }
    mdf_html_jetbrains_mono_font(&impl->opts.html_font);
    if (regular_uri != NULL) {
        st = mdf_html_font_uri_copy(impl, &impl->html_font_regular_uri,
                                    &impl->html_font_regular_uri_cap, regular_uri);
    } else {
        st = mdf_html_font_uri_copy(impl, &impl->html_font_regular_uri,
                                    &impl->html_font_regular_uri_cap,
                                    MDF_JETBRAINS_MONO_REGULAR_URL);
    }
    if (st != MDF_OK) {
        return st;
    }
    if (italic_uri != NULL) {
        st = mdf_html_font_uri_copy(impl, &impl->html_font_italic_uri,
                                    &impl->html_font_italic_uri_cap, italic_uri);
    } else {
        st = mdf_html_font_uri_copy(impl, &impl->html_font_italic_uri,
                                    &impl->html_font_italic_uri_cap,
                                    MDF_JETBRAINS_MONO_ITALIC_URL);
    }
    if (st != MDF_OK) {
        mdf_free_mem(&impl->allocator, impl->html_font_regular_uri,
                     impl->html_font_regular_uri_cap);
        impl->html_font_regular_uri = NULL;
        impl->html_font_regular_uri_cap = 0;
        return st;
    }
    return MDF_OK;
}
