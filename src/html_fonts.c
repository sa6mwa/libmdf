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
        return MDF_OK;
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

static mdf_status mdf_html_font_uri_join(mdf_impl *impl,
                                         char **dst,
                                         size_t *dst_cap,
                                         const char *base,
                                         const char *name)
{
    size_t base_len;
    size_t name_len;
    size_t len;
    int need_slash;
    char *copy;

    if (base == NULL || base[0] == '\0' || name == NULL) {
        return MDF_ERROR_INVALID;
    }
    base_len = strlen(base);
    name_len = strlen(name);
    need_slash = base[base_len - 1] != '/';
    if (base_len > (size_t)-1 - (size_t)need_slash ||
        base_len + (size_t)need_slash > (size_t)-1 - name_len ||
        base_len + (size_t)need_slash + name_len == (size_t)-1) {
        return MDF_ERROR_NOMEM;
    }
    len = base_len + (size_t)need_slash + name_len;
    copy = (char *)mdf_realloc_mem(&impl->allocator, NULL, 0, len + 1);
    if (copy == NULL) {
        return MDF_ERROR_NOMEM;
    }
    memcpy(copy, base, base_len);
    if (need_slash) {
        copy[base_len++] = '/';
    }
    memcpy(copy + base_len, name, name_len + 1);
    *dst = copy;
    *dst_cap = len + 1;
    return MDF_OK;
}

mdf_status mdf_configure_html_font(mdf_impl *impl)
{
    const char *regular_uri;
    const char *italic_uri;
    mdf_html_font_source source;
    mdf_status st;

    if (impl == NULL ||
        (impl->format != MDF_FORMAT_HTML && impl->format != MDF_FORMAT_HTML_DECK)) {
        return MDF_ERROR_INVALID;
    }
    if (impl->opts.html_dump_font || impl->opts.html_dump_font_force) {
        st = impl->opts.html_dump_font_force ?
            mdf_dump_html_jetbrains_mono_font_to_paths_force(impl->opts.html_font_dump_path,
                                                              impl->opts.html_font_dump_regular_path,
                                                              impl->opts.html_font_dump_italic_path) :
            mdf_dump_html_jetbrains_mono_font_to_paths(impl->opts.html_font_dump_path,
                                                        impl->opts.html_font_dump_regular_path,
                                                        impl->opts.html_font_dump_italic_path);
        if (st != MDF_OK) {
            return st;
        }
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
    if (regular_uri != NULL || italic_uri != NULL || impl->opts.html_font_uri != NULL) {
        source = MDF_HTML_FONT_SOURCE_EXTERNAL;
    }
    if (source == MDF_HTML_FONT_SOURCE_EMBEDDED) {
        return MDF_OK;
    }
    mdf_html_jetbrains_mono_font(&impl->opts.html_font);
    if (regular_uri != NULL) {
        st = mdf_html_font_uri_copy(impl, &impl->html_font_regular_uri,
                                    &impl->html_font_regular_uri_cap, regular_uri);
    } else if (impl->opts.html_font_uri != NULL) {
        st = mdf_html_font_uri_join(impl, &impl->html_font_regular_uri,
                                    &impl->html_font_regular_uri_cap, impl->opts.html_font_uri,
                                    "JetBrainsMono-Regular.woff2");
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
    } else if (impl->opts.html_font_uri != NULL) {
        st = mdf_html_font_uri_join(impl, &impl->html_font_italic_uri,
                                    &impl->html_font_italic_uri_cap, impl->opts.html_font_uri,
                                    "JetBrainsMono-Italic.woff2");
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
