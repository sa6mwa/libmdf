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

mdf_status mdf_set_html_jetbrains_mono_font_uris(mdf *self,
                                                 const char *regular_uri,
                                                 const char *italic_uri)
{
    mdf_impl *impl;

    if (self == NULL || self->impl == NULL) {
        return MDF_ERROR_INVALID;
    }
    impl = (mdf_impl *)self->impl;
    if (impl->format != MDF_FORMAT_HTML && impl->format != MDF_FORMAT_HTML_DECK) {
        return MDF_ERROR_INVALID;
    }
    if (impl->html_open ||
        ((regular_uri == NULL) != (italic_uri == NULL)) ||
        (regular_uri != NULL && (regular_uri[0] == '\0' || italic_uri[0] == '\0'))) {
        return MDF_ERROR_INVALID;
    }
    if (regular_uri == NULL) {
        regular_uri = MDF_JETBRAINS_MONO_REGULAR_URL;
        italic_uri = MDF_JETBRAINS_MONO_ITALIC_URL;
    }
    impl->html_font_regular_uri = regular_uri;
    impl->html_font_italic_uri = italic_uri;
    mdf_html_jetbrains_mono_font(&impl->opts.html_font);
    return MDF_OK;
}
