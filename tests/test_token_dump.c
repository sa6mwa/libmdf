#include <libmdf/mdf.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chunk_source {
    FILE *fp;
    size_t chunk;
} chunk_source;

static size_t chunk_read(void *userdata, char *dst, size_t cap, int *err)
{
    chunk_source *src;
    size_t want;
    size_t n;

    src = (chunk_source *)userdata;
    want = src->chunk == 0 ? cap : src->chunk;
    if (want > cap) {
        want = cap;
    }
    n = fread(dst, 1, want, src->fp);
    if (n == 0 && ferror(src->fp)) {
        *err = errno == 0 ? EIO : errno;
    }
    return n;
}

typedef struct dump_renderer {
    mdf base;
    mdf *inner;
} dump_renderer;

static void dump_escaped(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\\') {
            fputs("\\\\", stdout);
        } else if (s[i] == '\n') {
            fputs("\\n", stdout);
        } else if (s[i] == '\r') {
            fputs("\\r", stdout);
        } else if (s[i] == '\t') {
            fputs("\\t", stdout);
        } else {
            fputc((unsigned char)s[i], stdout);
        }
    }
}

static mdf_status dump_write_token(mdf *self, const mdf_token *token, mdf_sink *sink)
{
    (void)self;
    (void)sink;
    printf("token\t%d\t", (int)token->type);
    if (token->text != NULL) {
        dump_escaped(token->text, token->len);
    }
    printf("\t\t\t%d\n", token->type == MDF_TOKEN_CODE_TEXT ? 1 : 0);
    return MDF_OK;
}

static mdf_status dump_finish(mdf *self, mdf_sink *sink)
{
    (void)self;
    (void)sink;
    return MDF_OK;
}

static mdf_status dump_render(mdf *self, mdf_source *source, mdf_sink *sink)
{
    dump_renderer *renderer;

    renderer = (dump_renderer *)self;
    if (renderer->inner == NULL) {
        return MDF_ERROR_INVALID;
    }
    return renderer->inner->render(renderer->inner, source, sink);
}

static mdf_status dump_render_cstr(mdf *self, const char *markdown, char **out)
{
    dump_renderer *renderer;

    renderer = (dump_renderer *)self;
    if (renderer->inner == NULL) {
        if (out != NULL) {
            *out = NULL;
        }
        return MDF_ERROR_INVALID;
    }
    return renderer->inner->render_cstr(renderer->inner, markdown, out);
}

static const char *dump_error(const mdf *self)
{
    const dump_renderer *renderer;

    renderer = (const dump_renderer *)self;
    if (renderer->inner == NULL) {
        return "invalid instance";
    }
    return renderer->inner->error(renderer->inner);
}

static void dump_destroy(mdf *self)
{
    dump_renderer *renderer;

    renderer = (dump_renderer *)self;
    if (renderer->inner != NULL) {
        renderer->inner->destroy(renderer->inner);
        renderer->inner = NULL;
    }
}

static void dump_string_free(mdf *self, char *s)
{
    dump_renderer *renderer;

    renderer = (dump_renderer *)self;
    if (renderer->inner != NULL) {
        renderer->inner->string_free(renderer->inner, s);
    } else {
        free(s);
    }
}

static int sink_write(void *userdata, const char *src, size_t len)
{
    (void)userdata;
    return fwrite(src, 1, len, stdout) == len ? 0 : -1;
}

int main(int argc, char **argv)
{
    chunk_source src_data;
    mdf_options opts;
    mdf_source src;
    mdf_sink sink;
    dump_renderer renderer;
    FILE *fp;
    int rc;
    mdf_status st;

    if (argc != 3) {
        fprintf(stderr, "usage: test_token_dump CHUNK FILE\n");
        return 2;
    }
    fp = fopen(argv[2], "rb");
    if (fp == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    memset(&renderer, 0, sizeof(renderer));
    mdf_options_init(&opts);
    opts.boring = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer.inner);
    if (st != MDF_OK || renderer.inner == NULL) {
        fclose(fp);
        return 1;
    }
    renderer.base.write_token = dump_write_token;
    renderer.base.finish = dump_finish;
    renderer.base.render = dump_render;
    renderer.base.render_cstr = dump_render_cstr;
    renderer.base.error = dump_error;
    renderer.base.destroy = dump_destroy;
    renderer.base.string_free = dump_string_free;
    renderer.base.impl = renderer.inner->impl;
    src_data.fp = fp;
    src_data.chunk = (size_t)strtoul(argv[1], NULL, 10);
    src.userdata = &src_data;
    src.read = chunk_read;
    sink.userdata = NULL;
    sink.write = sink_write;
    rc = 1;
    if (renderer.inner->render((mdf *)&renderer, &src, &sink) == MDF_OK) {
        rc = 0;
    }
    renderer.base.destroy((mdf *)&renderer);
    fclose(fp);
    return rc;
}
