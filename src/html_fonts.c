#include "libmdf/mdf.h"
#include "mdf_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "html_embedded/jetbrains_regular.h"
#include "html_embedded/jetbrains_italic.h"

#define MDF_JETBRAINS_MONO_REGULAR_URL \
    "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/webfonts/JetBrainsMono-Regular.woff2"
#define MDF_JETBRAINS_MONO_ITALIC_URL \
    "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/webfonts/JetBrainsMono-Italic.woff2"

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

    if (path == NULL || path[0] == '\0') {
        return MDF_ERROR_INVALID;
    }
    if (!force && stat(path, &st) == 0) {
        return S_ISREG(st.st_mode) ? MDF_OK : MDF_ERROR_IO;
    }
    fp = fopen(path, "wb");
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

static mdf_status mdf_dump_html_jetbrains_mono_font_impl(const char *regular_path,
                                                         const char *italic_path,
                                                         int force)
{
    mdf_status st;

    if (regular_path == NULL || regular_path[0] == '\0' ||
        italic_path == NULL || italic_path[0] == '\0' ||
        strcmp(regular_path, italic_path) == 0) {
        return MDF_ERROR_INVALID;
    }
    st = mdf_write_html_font_file(regular_path,
                                  libmdf_html_jetbrains_regular,
                                  (size_t)libmdf_html_jetbrains_regular_len,
                                  force);
    if (st != MDF_OK) {
        return st;
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

static int mdf_html_font_uri_is_http(const char *uri)
{
    return uri != NULL &&
           ((strlen(uri) >= 7 && mdf_html_font_ascii_equal_n(uri, "http://", 7)) ||
            (strlen(uri) >= 8 && mdf_html_font_ascii_equal_n(uri, "https://", 8)));
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
        if (strlen(src) >= 10 && mdf_html_font_ascii_equal_n(src, "localhost/", 10)) {
            src += 9;
        } else if (src[0] != '/') {
            return -1;
        }
    } else {
        if (mdf_html_font_uri_is_http(uri) || strstr(uri, "://") != NULL) {
            return -1;
        }
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
    while (*src != '\0') {
        unsigned char c;

        if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
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
    if ((regular_uri != NULL && regular_uri[0] == '\0') ||
        (italic_uri != NULL && italic_uri[0] == '\0') ||
        (impl->opts.html_font_uri != NULL && impl->opts.html_font_uri[0] == '\0')) {
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
        if (mdf_html_font_path_join(regular_uri_buf, sizeof(regular_uri_buf),
                                    impl->opts.html_font_uri,
                                    "JetBrainsMono-Regular.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        regular_uri = regular_uri_buf;
    }
    if (italic_uri == NULL && impl->opts.html_font_uri != NULL) {
        if (mdf_html_font_path_join(italic_uri_buf, sizeof(italic_uri_buf),
                                    impl->opts.html_font_uri,
                                    "JetBrainsMono-Italic.woff2") != 0) {
            return MDF_ERROR_INVALID;
        }
        italic_uri = italic_uri_buf;
    }
    if (dump_requested) {
        if (dump_regular_path == NULL) {
            if (mdf_html_font_uri_to_path(dump_regular_buf, sizeof(dump_regular_buf), regular_uri) != 0) {
                return MDF_ERROR_INVALID;
            }
            dump_regular_path = dump_regular_buf;
        }
        if (dump_italic_path == NULL) {
            if (mdf_html_font_uri_to_path(dump_italic_buf, sizeof(dump_italic_buf), italic_uri) != 0) {
                return MDF_ERROR_INVALID;
            }
            dump_italic_path = dump_italic_buf;
        }
        st = mdf_dump_html_jetbrains_mono_font_impl(dump_regular_path, dump_italic_path,
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
