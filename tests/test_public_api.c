#include <libmdf/mdf.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct counting_allocator {
    size_t allocs;
    size_t reallocs;
    size_t frees;
    size_t allocs_26;
} counting_allocator;

typedef struct grow_sink {
    char *buf;
    size_t len;
    size_t cap;
} grow_sink;

typedef struct cstr_source {
    const char *src;
    size_t len;
    size_t off;
} cstr_source;

typedef struct one_chunk_then_fail_source {
    const char *src;
    size_t len;
    int reads;
} one_chunk_then_fail_source;

typedef struct failing_allocator {
    size_t alloc_calls;
    size_t realloc_calls;
    size_t fail_after;
} failing_allocator;

typedef struct sink_oom_allocator {
    int armed;
    size_t output_failures;
} sink_oom_allocator;

typedef struct owned_buffer_probe {
    void *owned;
    size_t owned_frees;
} owned_buffer_probe;

typedef struct tracking_allocator {
    size_t blocks;
    size_t bytes;
} tracking_allocator;

typedef union tracking_header {
    struct {
        size_t size;
    } h;
    long double align;
} tracking_header;

typedef struct failing_sink {
    size_t writes;
    size_t fail_after;
    grow_sink capture;
} failing_sink;

typedef struct armed_failing_sink {
    int armed;
    grow_sink capture;
} armed_failing_sink;

static void *test_alloc(void *userdata, size_t size)
{
    counting_allocator *c;

    c = (counting_allocator *)userdata;
    c->allocs++;
    if (size == 26) {
        c->allocs_26++;
    }
    return malloc(size);
}

static void *test_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    counting_allocator *c;

    (void)old_size;
    c = (counting_allocator *)userdata;
    c->reallocs++;
    if (ptr == NULL) {
        c->allocs++;
    }
    return realloc(ptr, new_size);
}

static void test_free(void *userdata, void *ptr, size_t size)
{
    counting_allocator *c;

    (void)size;
    c = (counting_allocator *)userdata;
    if (ptr != NULL) {
        c->frees++;
        free(ptr);
    }
}

static void *owned_probe_alloc(void *userdata, size_t size)
{
    (void)userdata;
    return malloc(size);
}

static void *owned_probe_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    (void)userdata;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void owned_probe_free(void *userdata, void *ptr, size_t size)
{
    owned_buffer_probe *probe;

    (void)size;
    probe = (owned_buffer_probe *)userdata;
    if (ptr == NULL) {
        return;
    }
    if (ptr == probe->owned) {
        probe->owned_frees++;
        return;
    }
    free(ptr);
}

static void *tracking_alloc(void *userdata, size_t size)
{
    tracking_allocator *tracker;
    tracking_header *header;

    tracker = (tracking_allocator *)userdata;
    header = (tracking_header *)malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->h.size = size;
    tracker->blocks++;
    tracker->bytes += size;
    return (void *)(header + 1);
}

static void *tracking_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    tracking_allocator *tracker;
    tracking_header *header;
    tracking_header *next;

    (void)old_size;
    if (ptr == NULL) {
        return tracking_alloc(userdata, new_size);
    }
    tracker = (tracking_allocator *)userdata;
    header = ((tracking_header *)ptr) - 1;
    next = (tracking_header *)realloc(header, sizeof(*header) + new_size);
    if (next == NULL) {
        return NULL;
    }
    tracker->bytes -= next->h.size;
    next->h.size = new_size;
    tracker->bytes += new_size;
    return (void *)(next + 1);
}

static void tracking_free(void *userdata, void *ptr, size_t size)
{
    tracking_allocator *tracker;
    tracking_header *header;

    (void)size;
    if (ptr == NULL) {
        return;
    }
    tracker = (tracking_allocator *)userdata;
    header = ((tracking_header *)ptr) - 1;
    tracker->blocks--;
    tracker->bytes -= header->h.size;
    free(header);
}

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

static int grow_write(void *userdata, const char *src, size_t len)
{
    grow_sink *sink;
    char *next;
    size_t cap;

    sink = (grow_sink *)userdata;
    if (sink->len + len + 1 < sink->len) {
        return -1;
    }
    if (sink->len + len + 1 > sink->cap) {
        cap = sink->cap == 0 ? 256 : sink->cap;
        while (cap < sink->len + len + 1) {
            cap *= 2;
        }
        next = (char *)realloc(sink->buf, cap);
        if (next == NULL) {
            return -1;
        }
        sink->buf = next;
        sink->cap = cap;
    }
    memcpy(sink->buf + sink->len, src, len);
    sink->len += len;
    sink->buf[sink->len] = '\0';
    return 0;
}

static void grow_free(grow_sink *sink)
{
    free(sink->buf);
    sink->buf = NULL;
    sink->len = 0;
    sink->cap = 0;
}

static size_t cstr_read(void *userdata, char *dst, size_t cap, int *err)
{
    cstr_source *src;
    size_t n;

    (void)err;
    src = (cstr_source *)userdata;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->len - src->off;
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, src->src + src->off, n);
    src->off += n;
    return n;
}

static size_t fail_read(void *userdata, char *dst, size_t cap, int *err)
{
    (void)userdata;
    (void)dst;
    (void)cap;
    *err = 5;
    return 0;
}

static size_t one_chunk_then_fail_read(void *userdata, char *dst, size_t cap, int *err)
{
    one_chunk_then_fail_source *src;
    size_t n;

    src = (one_chunk_then_fail_source *)userdata;
    src->reads++;
    if (src->reads == 1) {
        n = src->len;
        if (n > cap) {
            n = cap;
        }
        memcpy(dst, src->src, n);
        return n;
    }
    *err = 5;
    return 0;
}

static int fail_write(void *userdata, const char *src, size_t len)
{
    (void)userdata;
    (void)src;
    (void)len;
    return -1;
}

static int fail_after_write(void *userdata, const char *src, size_t len)
{
    failing_sink *sink;

    sink = (failing_sink *)userdata;
    sink->writes++;
    if (sink->writes >= sink->fail_after) {
        return -1;
    }
    return grow_write(&sink->capture, src, len);
}

static int armed_fail_write(void *userdata, const char *src, size_t len)
{
    armed_failing_sink *sink;

    sink = (armed_failing_sink *)userdata;
    if (sink->armed) {
        return -1;
    }
    return grow_write(&sink->capture, src, len);
}

static int discard_write(void *userdata, const char *src, size_t len)
{
    (void)userdata;
    (void)src;
    (void)len;
    return 0;
}

static void *failing_alloc(void *userdata, size_t size)
{
    failing_allocator *state;

    (void)size;
    state = (failing_allocator *)userdata;
    state->alloc_calls++;
    if (state->alloc_calls + state->realloc_calls >= state->fail_after) {
        return NULL;
    }
    return malloc(size == 0 ? 1 : size);
}

static void *failing_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    failing_allocator *state;

    (void)old_size;
    state = (failing_allocator *)userdata;
    state->realloc_calls++;
    if (state->alloc_calls + state->realloc_calls >= state->fail_after) {
        return NULL;
    }
    return realloc(ptr, new_size == 0 ? 1 : new_size);
}

static void failing_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

static void *sink_oom_alloc(void *userdata, size_t size)
{
    (void)userdata;
    return malloc(size == 0 ? 1 : size);
}

static void *sink_oom_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    sink_oom_allocator *state;

    state = (sink_oom_allocator *)userdata;
    if (state->armed && ptr == NULL && old_size == 0 && new_size == 256) {
        state->output_failures++;
        return NULL;
    }
    return realloc(ptr, new_size == 0 ? 1 : new_size);
}

static void sink_oom_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

int main(void)
{
    int fails;
    mdf_options opts;
    counting_allocator allocs;
    failing_allocator fail_allocs;
    sink_oom_allocator sink_oom_allocs;
    failing_sink fail_sink;
    armed_failing_sink armed_sink;
    one_chunk_then_fail_source read_fail_after_alloc;
    mdf *inst;
    char *out;
    char long_markdown[400];
    mdf_status st;
    mdf_token tok;
    cstr_source src_data;
    mdf_source src;
    grow_sink sink_data;
    mdf_sink sink;
    size_t before_allocs;

    fails = 0;
    inst = NULL;
    out = NULL;
    memset(long_markdown, 'a', sizeof(long_markdown));
    long_markdown[0] = '#';
    long_markdown[1] = ' ';
    long_markdown[sizeof(long_markdown) - 2] = '\n';
    long_markdown[sizeof(long_markdown) - 1] = '\0';
    memset(&allocs, 0, sizeof(allocs));
    memset(&fail_allocs, 0, sizeof(fail_allocs));
    memset(&fail_sink, 0, sizeof(fail_sink));
    memset(&armed_sink, 0, sizeof(armed_sink));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;

    st = mdf_create(MDF_FORMAT_ANSI, &opts, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "create rejects null out");

    st = mdf_create((mdf_format)99, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID && inst == NULL, "create rejects invalid format");

    fails += expect(strcmp(mdf_status_string(MDF_ERROR_INVALID), "invalid argument") == 0,
                    "status string exposes invalid argument");
    fails += expect(strcmp(mdf_status_string((mdf_status)99), "unknown status") == 0,
                    "status string exposes unknown fallback");
    fails += expect(mdf_theme_count() > 0, "theme count is non-zero");
    fails += expect(mdf_theme_name(mdf_theme_count()) == NULL,
                    "theme_name rejects out-of-range index");
    fails += expect(mdf_theme_exists(NULL), "theme_exists treats null as default");
    fails += expect(mdf_theme_exists(""), "theme_exists treats empty string as default");
    fails += expect(!mdf_theme_exists("not-a-theme"), "theme_exists rejects unknown theme");
    opts.theme_name = "not-a-theme";
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID && inst == NULL,
                    "create rejects unknown theme option");
    opts.theme_name = "default";
    {
        char mutable_theme[16];

        strcpy(mutable_theme, "default");
        opts.theme_name = mutable_theme;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "create accepts caller-owned theme option storage");
        strcpy(mutable_theme, "not-a-theme");
        if (inst != NULL) {
            st = inst->render_cstr(inst, "theme lifetime\n", &out);
            fails += expect(st == MDF_OK && out != NULL && strstr(out, "theme lifetime") != NULL,
                            "renderer does not re-read caller-owned theme option storage");
            inst->string_free(inst, out);
            out = NULL;
            inst->destroy(inst);
            inst = NULL;
        }
        opts.theme_name = "default";
    }
    fails += expect(mdf_terminal_width(-1, 0) == 80,
                    "terminal width normalizes non-positive fallback");
    mdf_options_init(NULL);

    st = mdf_create(MDF_FORMAT_ANSI, NULL, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds with default options");
    st = inst->render_cstr(inst, "default ansi\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "default ansi") != NULL,
                    "ansi default-options handle renders");
    inst->string_free(inst, out);
    out = NULL;
    memset(long_markdown, ' ', 256);
    long_markdown[256] = 'X';
    long_markdown[257] = '\n';
    long_markdown[258] = '\0';
    st = inst->render_cstr(inst, long_markdown, &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "X") != NULL,
                    "ansi preserves byte that overflows undecided prefix buffer");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    st = mdf_create(MDF_FORMAT_HTML, NULL, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds with default options");
    st = inst->render_cstr(inst, "default html\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "default html") != NULL,
                    "html default-options handle renders");
    fails += expect(strstr(out, "<!doctype html>") != NULL,
                    "html default-options handle emits shell");
    inst->string_free(inst, out);
    out = NULL;
    st = inst->render_cstr(inst, long_markdown, &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "X") != NULL,
                    "html preserves byte that overflows undecided prefix buffer");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;
    memset(long_markdown, 'a', sizeof(long_markdown));
    long_markdown[0] = '#';
    long_markdown[1] = ' ';
    long_markdown[sizeof(long_markdown) - 2] = '\n';
    long_markdown[sizeof(long_markdown) - 1] = '\0';

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "ansi create succeeds with custom alloc/free and inherited default realloc");
    before_allocs = allocs.allocs;
    st = inst->render_cstr(inst, long_markdown, &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "aaa") != NULL,
                    "ansi render succeeds with custom alloc/free and inherited default realloc");
    fails += expect(allocs.allocs > before_allocs,
                    "custom alloc/free without explicit realloc uses alloc-copy-free growth");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = NULL;
    opts.allocator.free = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds with partial allocator callbacks");
    st = inst->render_cstr(inst, "partial allocator\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "partial allocator") != NULL,
                    "ansi render succeeds with allocator fallback normalization");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = NULL;
    opts.allocator.free = NULL;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds with partial allocator callbacks");
    st = inst->render_cstr(inst, "partial html allocator\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "partial html allocator") != NULL,
                    "html render succeeds with allocator fallback normalization");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds");
    fails += expect(allocs.allocs >= 3,
                    "ansi create allocates the default per-instance emission buffer");
    fails += expect(inst->write_token != NULL &&
                    inst->finish != NULL &&
                    inst->render != NULL &&
                    inst->render_cstr != NULL &&
                    inst->error != NULL &&
                    inst->destroy != NULL &&
                    inst->string_free != NULL,
                    "single handle populates receiver methods");
    fails += expect(strcmp(inst->error(NULL), "invalid instance") == 0,
                    "error on null instance is stable");
    inst->destroy(NULL);
    out = (char *)malloc(4);
    fails += expect(out != NULL, "malloc for null-instance string_free test");
    if (out != NULL) {
        memcpy(out, "abc", 4);
        inst->string_free(NULL, out);
        out = NULL;
    }

    st = inst->render(NULL, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null self");
    st = inst->render(inst, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null source and sink");
    src.userdata = NULL;
    src.read = NULL;
    sink.userdata = NULL;
    sink.write = grow_write;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null source callback");
    sink.write = NULL;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null sink callback");
    st = inst->render_cstr(inst, NULL, &out);
    fails += expect(st == MDF_ERROR_INVALID && out == NULL, "render_cstr rejects null markdown");
    st = inst->render_cstr(inst, "# hi\n", NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render_cstr rejects null out");
    st = inst->write_token(NULL, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null self");
    st = inst->write_token(inst, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null token and sink");
    sink.userdata = NULL;
    sink.write = NULL;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null sink callback");
    st = inst->finish(NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null self");
    st = inst->finish(inst, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null sink");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null sink callback");

    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = NULL;
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "text token without bytes rejects");

    st = inst->render_cstr(inst, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "render_cstr returns ansi output");
    fails += expect(strstr(out, "Title") != NULL && strstr(out, "Body.") != NULL,
                    "render_cstr ansi output contains content");
    inst->string_free(inst, out);
    out = NULL;

    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = 20;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create accepts narrow width");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "123456789 123456789 123456789\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strcmp(out, "123456789 123456789\n123456789\n") == 0,
                        "ansi narrow width wraps long line");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    opts.width = 0;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi zero width restores default width");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "123456789 123456789 123456789\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strcmp(out, "123456789 123456789 123456789\n") == 0,
                        "ansi zero width uses default wrapping instead of disabling wrap");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = 10;
    opts.margin_left = 2;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi left margin renderer create");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "alpha\n\nbeta\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strcmp(out, "  alpha\n\n  beta\n") == 0,
                        "ansi left margin prefixes nonblank lines lazily");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.width = 20;
    opts.margin_left = 2;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi styled heading margin renderer create");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "# Heading\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strncmp(out, "  \033[1;32m#", 10) == 0 &&
                        strstr(out, "\n#") == NULL,
                        "ansi left margin is absolute before styled heading marker");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = 10;
    opts.margin_right = 2;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi right margin renderer create");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "12345678 123\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strcmp(out, "12345678\n123\n") == 0,
                        "ansi right margin reduces wrap width");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = 10;
    opts.margin_left = 2;
    opts.margin_right = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi combined margin renderer create");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "1234567 12\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strcmp(out, "  1234567\n  12\n") == 0,
                        "ansi combined margins indent and wrap at remaining content width");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.width = 10;
    opts.margin_left = 5;
    opts.margin_right = 5;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID, "ansi rejects margins that consume width");

    mdf_options_init(&opts);
    opts.margin_left = 4;
    opts.margin_right = 4;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html accepts ignored ansi margins");
    if (inst != NULL) {
        char *html_with_margins;
        char *html_without_margins;
        mdf *plain_html;

        html_with_margins = NULL;
        html_without_margins = NULL;
        plain_html = NULL;
        st = inst->render_cstr(inst, "alpha\n", &html_with_margins);
        fails += expect(st == MDF_OK && html_with_margins != NULL, "html render with ansi margins");
        mdf_options_init(&opts);
        st = mdf_create(MDF_FORMAT_HTML, &opts, &plain_html);
        fails += expect(st == MDF_OK && plain_html != NULL, "html renderer without ansi margins");
        if (plain_html != NULL) {
            st = plain_html->render_cstr(plain_html, "alpha\n", &html_without_margins);
            fails += expect(st == MDF_OK && html_without_margins != NULL &&
                            html_with_margins != NULL &&
                            strcmp(html_with_margins, html_without_margins) == 0,
                            "html output ignores ansi margins");
            plain_html->string_free(plain_html, html_without_margins);
            plain_html->destroy(plain_html);
        }
        inst->string_free(inst, html_with_margins);
        inst->destroy(inst);
        inst = NULL;
    }

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 0;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    {
        char fixed_emit[128];

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "ansi create accepts developer fixed emission buffer");
        fails += expect(allocs.allocs == 2,
                        "developer fixed emission buffer avoids default emission allocation");
        if (inst != NULL) {
            st = inst->render_cstr(inst, "# Fixed Buffer\n", &out);
            fails += expect(st == MDF_OK && out != NULL &&
                            strstr(out, "Fixed") != NULL,
                            "developer fixed emission buffer renders when large enough");
            inst->string_free(inst, out);
            out = NULL;
            inst->destroy(inst);
            inst = NULL;
        }
    }

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.osc8 = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    {
        char fixed_emit[256];
        size_t osc8_start_allocs_before_render;

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "osc8 fixed emission buffer renderer create");
        if (inst != NULL) {
            cstr_source link_src;

            memset(&sink_data, 0, sizeof(sink_data));
            link_src.src = "[link](https://example.com)\n";
            link_src.len = strlen(link_src.src);
            link_src.off = 0;
            src.userdata = &link_src;
            src.read = cstr_read;
            sink.userdata = &sink_data;
            sink.write = grow_write;
            st = inst->render(inst, &src, &sink);
            fails += expect(st == MDF_OK &&
                            sink_data.buf != NULL &&
                            strstr(sink_data.buf, "\033]8;;https://example.com\033\\") != NULL,
                            "osc8 link warms parser buffers with supplied emission buffer");
            grow_free(&sink_data);
            memset(&sink_data, 0, sizeof(sink_data));
            link_src.off = 0;
            osc8_start_allocs_before_render = allocs.allocs_26;
            st = inst->render(inst, &src, &sink);
            fails += expect(st == MDF_OK &&
                            sink_data.buf != NULL &&
                            strstr(sink_data.buf, "\033]8;;https://example.com\033\\") != NULL,
                            "osc8 link rerenders with supplied emission buffer");
            fails += expect(allocs.allocs_26 == osc8_start_allocs_before_render,
                            "osc8 link start composition does not allocate outside emission buffer after warmup");
            grow_free(&sink_data);
            inst->destroy(inst);
            inst = NULL;
        }
    }

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 0;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    {
        char fixed_emit[128];

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.max_cap = 8;
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "ansi create accepts supplied emission buffer with smaller max");
        if (inst != NULL) {
            st = inst->render_cstr(inst, "# Too Large\n", &out);
            fails += expect(st != MDF_OK && out == NULL,
                            "supplied emission buffer honors configured max below capacity");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 0;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    {
        char fixed_emit[8];

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "ansi create accepts tiny fixed emission buffer");
        if (inst != NULL) {
            st = inst->render_cstr(inst, "# Too Large\n", &out);
            fails += expect(st != MDF_OK && out == NULL,
                            "tiny fixed emission buffer fails instead of growing");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    {
        char fixed_emit[8];

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "ansi create accepts tiny fixed emission buffer for direct emit path");
        if (inst != NULL) {
            st = inst->render_cstr(inst, "https://example.com/abcdefghijklmnopqrstuvwxyz0123456789\n", &out);
            fails += expect(st != MDF_OK && out == NULL,
                            "direct emission path honors tiny fixed emission buffer");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 0;
    opts.emission_buffer.initial_cap = 8;
    opts.emission_buffer.max_cap = 8;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "ansi create accepts bounded growable emission buffer");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "# Too Large\n", &out);
        fails += expect(st != MDF_OK && out == NULL,
                        "bounded growable emission buffer fails at configured max");
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.emission_buffer.initial_cap = 8;
    opts.emission_buffer.max_cap = 1024;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 8;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "ansi create succeeds before direct emission buffer growth exhaustion");
    if (inst != NULL) {
        fail_allocs.alloc_calls = 0;
        fail_allocs.realloc_calls = 0;
        fail_allocs.fail_after = 1;
        st = inst->render_cstr(inst, "https://example.com/abcdefghijklmnopqrstuvwxyz0123456789\n", &out);
        fails += expect(st == MDF_ERROR_NOMEM && out == NULL,
                        "direct emission growth surfaces allocator exhaustion as oom");
        fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                        "direct emission growth exposes allocator exhaustion text");
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.boring = 0;
    opts.emission_buffer.initial_cap = 8;
    opts.emission_buffer.max_cap = 1024;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 8;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "ansi create succeeds before composed emission buffer growth exhaustion");
    if (inst != NULL) {
        fail_allocs.alloc_calls = 0;
        fail_allocs.realloc_calls = 0;
        fail_allocs.fail_after = 1;
        st = inst->render_cstr(inst, "# Too Large\n", &out);
        fails += expect(st == MDF_ERROR_NOMEM && out == NULL,
                        "composed emission growth surfaces allocator exhaustion as oom");
        fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                        "composed emission growth exposes allocator exhaustion text");
        inst->destroy(inst);
        inst = NULL;
    }

    {
        owned_buffer_probe owned_probe;
        char *owned_emit;

        memset(&owned_probe, 0, sizeof(owned_probe));
        owned_emit = (char *)malloc(256);
        fails += expect(owned_emit != NULL, "allocate owned emission buffer for table child renderer test");
        if (owned_emit != NULL) {
            mdf_options owned_opts;
            mdf *owned_inst;

            mdf_options_init(&owned_opts);
            owned_opts.boring = 1;
            owned_opts.allocator.userdata = &owned_probe;
            owned_opts.allocator.alloc = owned_probe_alloc;
            owned_opts.allocator.realloc = owned_probe_realloc;
            owned_opts.allocator.free = owned_probe_free;
            owned_opts.emission_buffer.data = owned_emit;
            owned_opts.emission_buffer.cap = 256;
            owned_opts.emission_buffer.take_ownership = 1;
            owned_probe.owned = owned_emit;
            owned_inst = NULL;
            st = mdf_create(MDF_FORMAT_ANSI, &owned_opts, &owned_inst);
            fails += expect(st == MDF_OK && owned_inst != NULL,
                            "table renderer accepts caller-owned emission buffer");
            if (owned_inst != NULL) {
                st = owned_inst->render_cstr(owned_inst,
                                             "| A | B |\n| --- | --- |\n| *x* | y |\n",
                                             &out);
                fails += expect(st == MDF_OK && out != NULL && strstr(out, "x") != NULL,
                                "table render succeeds with caller-owned emission buffer");
                fails += expect(owned_probe.owned_frees == 0,
                                "table cell renderer does not free caller-owned parent emission buffer");
                owned_inst->string_free(owned_inst, out);
                out = NULL;
                owned_inst->destroy(owned_inst);
                fails += expect(owned_probe.owned_frees == 1,
                                "parent renderer frees caller-owned emission buffer on destroy");
            }
            free(owned_emit);
        }
    }

    {
        tracking_allocator tracker;
        mdf_options tracked_opts;
        mdf *tracked_inst;
        char adversarial[384];
        const char *prefix;
        const char *suffix;
        size_t adversarial_len;
        size_t i;
        cstr_source tracked_src;

        memset(&tracker, 0, sizeof(tracker));
        memset(adversarial, 0, sizeof(adversarial));
        prefix = "| A | B |\n| --- | --- |\n| ";
        suffix = " | x |\n";
        adversarial_len = 0;
        memcpy(adversarial + adversarial_len, prefix, strlen(prefix));
        adversarial_len += strlen(prefix);
        adversarial[adversarial_len++] = '\0';
        for (i = 0; i < 256; i++) {
            adversarial[adversarial_len++] = (char)0xff;
        }
        memcpy(adversarial + adversarial_len, suffix, strlen(suffix));
        adversarial_len += strlen(suffix);
        mdf_options_init(&tracked_opts);
        tracked_opts.boring = 1;
        tracked_opts.width = 20;
        tracked_opts.allocator.userdata = &tracker;
        tracked_opts.allocator.alloc = tracking_alloc;
        tracked_opts.allocator.realloc = tracking_realloc;
        tracked_opts.allocator.free = tracking_free;
        tracked_inst = NULL;
        st = mdf_create(MDF_FORMAT_ANSI, &tracked_opts, &tracked_inst);
        fails += expect(st == MDF_OK && tracked_inst != NULL,
                        "tracked allocator table renderer create");
        if (tracked_inst != NULL) {
            memset(&sink_data, 0, sizeof(sink_data));
            tracked_src.src = adversarial;
            tracked_src.len = adversarial_len;
            tracked_src.off = 0;
            src.userdata = &tracked_src;
            src.read = cstr_read;
            sink.userdata = &sink_data;
            sink.write = grow_write;
            st = tracked_inst->render(tracked_inst, &src, &sink);
            fails += expect(st == MDF_OK, "adversarial invalid-byte table render succeeds");
            grow_free(&sink_data);
            tracked_inst->destroy(tracked_inst);
            fails += expect(tracker.blocks == 0 && tracker.bytes == 0,
                            "adversarial table render releases all allocator-tracked buffers");
        }
    }

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds for manual token stream");

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Hello";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts text");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "world";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts second text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL, "ansi finish completes manual token stream");
    fails += expect(strstr(sink_data.buf, "Hello world") != NULL, "ansi manual token stream renders expected text");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_LIST_ITEM_START;
    tok.text = "-";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts list item start");
    tok.type = MDF_TOKEN_TASK_CHECKED;
    tok.text = "X";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts checked task marker");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts following space token");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "done";
    tok.len = 4;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts task text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "- [X] done") != NULL,
                    "ansi manual task token stream renders checked task line");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_HEADING_START;
    tok.level = 2;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts heading start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Head";
    tok.len = 4;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts heading text");
    tok.type = MDF_TOKEN_HEADING_END;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts heading end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "## Head") != NULL,
                    "ansi manual heading token stream renders heading text");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_BLOCKQUOTE_START;
    tok.level = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts blockquote start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "quote";
    tok.len = 5;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts quote text");
    tok.type = MDF_TOKEN_BLOCKQUOTE_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts blockquote end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "> quote") != NULL,
                    "ansi manual blockquote token stream renders quoted content");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_CODE_BLOCK_START;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts code block start");
    tok.type = MDF_TOKEN_CODE_TEXT;
    tok.text = "code();";
    tok.len = 7;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts code text");
    tok.type = MDF_TOKEN_CODE_BLOCK_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts code block end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "code();") != NULL,
                    "ansi manual code block token stream renders code content");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "a";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts paragraph text");
    tok.type = MDF_TOKEN_NEWLINE;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "b";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts second line text");
    tok.type = MDF_TOKEN_PARAGRAPH_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts paragraph end");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "c";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts following paragraph text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "a\nb\n\nc") != NULL,
                    "ansi manual newline and paragraph-end token stream preserves paragraph structure");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "before";
    tok.len = 6;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts leading text");
    tok.type = MDF_TOKEN_THEMATIC_BREAK;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts thematic break");
    tok.type = MDF_TOKEN_NEWLINE;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts following newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "after";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts trailing text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "before") != NULL &&
                    strstr(sink_data.buf, "after") != NULL,
                    "ansi manual thematic token stream preserves surrounding content");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_LIST_ITEM_START;
    tok.text = "1.";
    tok.len = 2;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts ordered list item start");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts following space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "first";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts item text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "1. first") != NULL,
                    "ansi manual ordered list token stream renders ordered marker");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf == NULL,
                    "ansi finish on a fresh handle is a no-op");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Again";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token starts a second manual session on the same handle");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi second manual session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL && strstr(sink_data.buf, "Again") != NULL,
                    "ansi handle resets cleanly between manual sessions");
    grow_free(&sink_data);

    memset(&fail_sink, 0, sizeof(fail_sink));
    fail_sink.fail_after = 1;
    sink.userdata = &fail_sink;
    sink.write = fail_after_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Oops";
    tok.len = 4;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token can buffer text before a finish failure");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_ERROR_IO, "ansi finish surfaces sink write failure");
    fails += expect(strcmp(inst->error(inst), "sink write failed") == 0,
                    "ansi finish failure sets sink write error text");
    grow_free(&fail_sink.capture);
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Retry";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi manual session restarts cleanly after finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi restarted manual session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL && strstr(sink_data.buf, "Retry") != NULL,
                    "ansi restarted manual session finishes after prior failure");
    grow_free(&sink_data);
    st = inst->render_cstr(inst, "after ansi finish failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "after ansi finish failure") != NULL,
                    "ansi handle recovers after manual finish failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "ansi recovery after manual finish failure clears error text");
    inst->string_free(inst, out);
    out = NULL;

    st = inst->render_cstr(inst, "", &out);
    fails += expect(st == MDF_OK && out != NULL && strcmp(out, "") == 0,
                    "empty ansi render_cstr returns empty string");
    inst->string_free(inst, out);
    out = NULL;

    inst->destroy(inst);
    inst = NULL;
    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 8;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds before write_token allocator exhaustion");
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 1;
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "x";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_NOMEM, "ansi write_token surfaces allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "ansi write_token exposes allocator exhaustion text");
    grow_free(&sink_data);
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4096;
    st = inst->render_cstr(inst, "after ansi write_token oom\n", &out);
    fails += expect(st == MDF_OK, "ansi recovery after write_token allocator failure returns ok");
    fails += expect(out != NULL, "ansi recovery after write_token allocator failure returns output");
    fails += expect(out != NULL && strstr(out, "after") != NULL,
                    "ansi recovery after write_token allocator failure preserves content");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "ansi recovery after write_token allocator failure clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds after allocator-failure coverage");

    memset(&src_data, 0, sizeof(src_data));
    src_data.src = "* item\n";
    src_data.len = strlen(src_data.src);
    src.userdata = &src_data;
    src.read = cstr_read;
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL, "render streams through source and sink");
    fails += expect(strstr(sink_data.buf, "- item") != NULL, "render sink output matches markdown content");
    grow_free(&sink_data);

    src.userdata = NULL;
    src.read = fail_read;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "render surfaces source read failure");
    fails += expect(strcmp(inst->error(inst), "source read failed") == 0,
                    "render exposes source read failure text");
    st = inst->render_cstr(inst, "after source failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "after source failure") != NULL,
                    "ansi handle recovers after source read failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "ansi success clears previous error text");
    inst->string_free(inst, out);
    out = NULL;

    inst->destroy(inst);
    inst = NULL;
    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds before delayed source read failure");
    memset(&read_fail_after_alloc, 0, sizeof(read_fail_after_alloc));
    read_fail_after_alloc.src = "---\ntitle: delayed failure\n";
    read_fail_after_alloc.len = strlen(read_fail_after_alloc.src);
    src.userdata = &read_fail_after_alloc;
    src.read = one_chunk_then_fail_read;
    sink.userdata = NULL;
    sink.write = discard_write;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "render surfaces delayed source read failure");
    fails += expect(strcmp(inst->error(inst), "source read failed") == 0,
                    "delayed source read failure sets error text");
    inst->destroy(inst);
    inst = NULL;
    fails += expect(allocs.allocs == allocs.frees,
                    "delayed source read failure releases parser-owned buffers");

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds before delayed table source failure");
    memset(&read_fail_after_alloc, 0, sizeof(read_fail_after_alloc));
    read_fail_after_alloc.src = "| A | B |\n| --- | --- |\n| 1 | 2 |\n";
    read_fail_after_alloc.len = strlen(read_fail_after_alloc.src);
    src.userdata = &read_fail_after_alloc;
    src.read = one_chunk_then_fail_read;
    sink.userdata = NULL;
    sink.write = discard_write;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "render surfaces delayed table source read failure");
    inst->destroy(inst);
    inst = NULL;
    fails += expect(allocs.allocs == allocs.frees,
                    "delayed table source read failure releases parser-owned buffers");

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ansi create succeeds after delayed source read failure");

    memset(&src_data, 0, sizeof(src_data));
    src_data.src = "* item\n";
    src_data.len = strlen(src_data.src);
    src.userdata = &src_data;
    src.read = cstr_read;
    sink.userdata = NULL;
    sink.write = fail_write;
    st = inst->render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "render surfaces sink write failure");
    fails += expect(strcmp(inst->error(inst), "sink write failed") == 0,
                    "render exposes sink write failure text");
    st = inst->render_cstr(inst, "after sink failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "after sink failure") != NULL,
                    "ansi handle recovers after sink write failure");
    inst->string_free(inst, out);
    out = NULL;

    inst->string_free(inst, NULL);
    inst->destroy(inst);
    inst = NULL;

    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds");
    st = inst->render_cstr(inst, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html render_cstr succeeds");
    fails += expect(strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "<span class=\"mdf-heading\"") != NULL,
                    "html render_cstr emits document shell and heading markup");
    inst->string_free(inst, out);
    out = NULL;

    inst->destroy(inst);
    inst = NULL;
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds for manual token stream");

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Body";
    tok.len = 4;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts plain text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL, "html finish completes manual token stream");
    fails += expect(strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "Body") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual token stream emits shell and content");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_LIST_ITEM_START;
    tok.text = "-";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts list item start");
    tok.type = MDF_TOKEN_TASK_UNCHECKED;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts unchecked task marker");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts following space token");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "todo";
    tok.len = 4;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts task text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "[ ]") != NULL &&
                    strstr(sink_data.buf, "todo") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual task token stream renders unchecked task content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_HEADING_START;
    tok.level = 3;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts heading start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Head";
    tok.len = 4;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts heading text");
    tok.type = MDF_TOKEN_HEADING_END;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts heading end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<span class=\"mdf-heading\"") != NULL &&
                    strstr(sink_data.buf, "Head") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual heading token stream renders heading content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_BLOCKQUOTE_START;
    tok.level = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts blockquote start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "quote";
    tok.len = 5;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts quote text");
    tok.type = MDF_TOKEN_BLOCKQUOTE_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts blockquote end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<span class=\"mdf-prefix\">") != NULL &&
                    strstr(sink_data.buf, "quote") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual blockquote token stream renders quoted content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_CODE_BLOCK_START;
    tok.level = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts code block start");
    tok.type = MDF_TOKEN_CODE_TEXT;
    tok.text = "code();";
    tok.len = 7;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts code text");
    tok.type = MDF_TOKEN_CODE_BLOCK_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts code block end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "code();") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual code block token stream renders code content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "a";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts paragraph text");
    tok.type = MDF_TOKEN_NEWLINE;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "b";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts second line text");
    tok.type = MDF_TOKEN_PARAGRAPH_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts paragraph end");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "c";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts following paragraph text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "a") != NULL &&
                    strstr(sink_data.buf, "b") != NULL &&
                    strstr(sink_data.buf, "c") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual newline and paragraph-end token stream keeps content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "before";
    tok.len = 6;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts leading text");
    tok.type = MDF_TOKEN_THEMATIC_BREAK;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts thematic break");
    tok.type = MDF_TOKEN_NEWLINE;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts following newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "after";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts trailing text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "before") != NULL &&
                    strstr(sink_data.buf, "after") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual thematic token stream keeps surrounding content inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_LIST_ITEM_START;
    tok.text = "1.";
    tok.len = 2;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts ordered list item start");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts following space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "first";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts item text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "1.") != NULL &&
                    strstr(sink_data.buf, "first") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual ordered list token stream renders ordered marker inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html finish on a fresh handle emits a complete shell");
    grow_free(&sink_data);

    inst->destroy(inst);
    inst = NULL;
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds for failure-mode coverage");

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Again";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token starts a second manual session on the same handle");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html second manual session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "Again") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html handle resets cleanly between manual sessions");
    grow_free(&sink_data);

    memset(&fail_sink, 0, sizeof(fail_sink));
    fail_sink.fail_after = 1;
    sink.userdata = &fail_sink;
    sink.write = fail_after_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Oops";
    tok.len = 4;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_IO, "html write_token surfaces sink write failure");
    fails += expect(strcmp(inst->error(inst), "sink write failed") == 0,
                    "html write_token failure sets sink write error text");
    grow_free(&fail_sink.capture);
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Retry";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html manual session restarts cleanly after write_token failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html restarted manual session accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "Retry") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html restarted manual session finishes after prior failure");
    grow_free(&sink_data);
    st = inst->render_cstr(inst, "after html token failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "after html token failure") != NULL &&
                    strstr(out, "</html>") != NULL,
                    "html handle recovers after manual write_token failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html recovery after manual write_token failure clears error text");
    inst->string_free(inst, out);
    out = NULL;

    memset(&armed_sink, 0, sizeof(armed_sink));
    sink.userdata = &armed_sink;
    sink.write = armed_fail_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Body";
    tok.len = 4;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token can buffer text before a finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html buffered session accepts document end before finish failure");
    armed_sink.armed = 1;
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_ERROR_IO, "html finish surfaces sink write failure");
    fails += expect(strcmp(inst->error(inst), "sink write failed") == 0,
                    "html finish failure sets sink write error text");
    grow_free(&armed_sink.capture);
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Retry";
    tok.len = 5;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html manual session restarts cleanly after finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html restarted session after finish failure accepts document end");
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "<!doctype html>") != NULL &&
                    strstr(sink_data.buf, "Retry") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html restarted session finishes after prior finish failure");
    grow_free(&sink_data);

    st = inst->render_cstr(inst, "after manual html\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "after manual html") != NULL &&
                    strstr(out, "</html>") != NULL,
                    "html handle resets cleanly after manual token stream");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html success clears previous error text");
    inst->string_free(inst, out);
    out = NULL;

    st = inst->render_cstr(inst, "", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "</html>") != NULL,
                    "empty html render_cstr still emits shell");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds before write_token allocator exhaustion");
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "x";
    tok.len = 1;
    st = inst->write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_NOMEM, "html write_token surfaces allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "html write_token exposes allocator exhaustion text");
    grow_free(&sink_data);
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4096;
    st = inst->render_cstr(inst, "after html write_token oom\n", &out);
    fails += expect(st == MDF_OK, "html recovery after write_token allocator failure returns ok");
    fails += expect(out != NULL, "html recovery after write_token allocator failure returns output");
    fails += expect(out != NULL && strstr(out, "<!doctype html>") != NULL,
                    "html recovery after write_token allocator failure emits shell");
    fails += expect(out != NULL && strstr(out, "after") != NULL,
                    "html recovery after write_token allocator failure preserves content");
    fails += expect(out != NULL && strstr(out, "</html>") != NULL,
                    "html recovery after write_token allocator failure closes document");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html recovery after write_token allocator failure clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds before finish allocator exhaustion");
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = inst->finish(inst, &sink);
    fails += expect(st == MDF_ERROR_NOMEM, "html finish on a fresh handle surfaces allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "html fresh finish exposes allocator exhaustion text");
    grow_free(&sink_data);
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4096;
    st = inst->render_cstr(inst, "after html finish oom\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "after") != NULL &&
                    strstr(out, "</html>") != NULL,
                    "html handle recovers after fresh-finish allocator failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html recovery after fresh-finish allocator failure clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.fail_after = 1;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_NOMEM && inst == NULL, "create surfaces allocator failure");

    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 5;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "create succeeds before later allocator exhaustion");
    st = inst->render_cstr(inst, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_ERROR_NOMEM && out == NULL, "render_cstr surfaces allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "render_cstr exposes allocator exhaustion text");
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 64;
    st = inst->render_cstr(inst, "after allocator exhaustion\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "after allocator exhaustion") != NULL,
                    "handle recovers after allocator-driven render_cstr failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "successful render after allocator failure clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html create succeeds before render_cstr allocator exhaustion");
    st = inst->render_cstr(inst, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_ERROR_NOMEM && out == NULL, "html render_cstr surfaces allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "html render_cstr exposes allocator exhaustion text");
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 4096;
    st = inst->render_cstr(inst, "after html render_cstr oom\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "after") != NULL &&
                    strstr(out, "</html>") != NULL,
                    "html handle recovers after render_cstr allocator failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html render_cstr recovery clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    memset(&sink_oom_allocs, 0, sizeof(sink_oom_allocs));
    opts.allocator.userdata = &sink_oom_allocs;
    opts.allocator.alloc = sink_oom_alloc;
    opts.allocator.realloc = sink_oom_realloc;
    opts.allocator.free = sink_oom_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "create succeeds before render_cstr output sink oom");
    sink_oom_allocs.armed = 1;
    st = inst->render_cstr(inst, "x\n", &out);
    fails += expect(sink_oom_allocs.output_failures == 1,
                    "render_cstr output sink allocation failure was exercised");
    fails += expect(st == MDF_ERROR_NOMEM && out == NULL,
                    "render_cstr output sink oom is reported as allocator exhaustion");
    fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                    "render_cstr output sink oom exposes allocator exhaustion text");
    sink_oom_allocs.armed = 0;
    st = inst->render_cstr(inst, "after output sink oom\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "after output sink oom") != NULL,
                    "handle recovers after render_cstr output sink oom");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "render_cstr output sink oom recovery clears error text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    {
        static const char repeated_ansi[] =
            "# The Outcome-Based Agile Framework\n\n"
            "**\"Governance exists to support autonomy\"** and [link](https://example.com).\n";
        static const char repeated_html[] =
            "# HTML Warmup\n\n"
            "**bold** and [link](https://example.com).\n";
        static const char repeated_table[] =
            "| A | B | C |\n"
            "|---|---|---|\n"
            "| alpha | beta | gamma |\n"
            "| long long long long long | two | three |\n";
        size_t before_allocs;
        size_t before_reallocs;
        size_t before_frees;
        size_t late_table_allocs;
        int i;

        memset(&allocs, 0, sizeof(allocs));
        mdf_options_init(&opts);
        opts.width = 40;
        opts.allocator.userdata = &allocs;
        opts.allocator.alloc = test_alloc;
        opts.allocator.realloc = test_realloc;
        opts.allocator.free = test_free;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "memory workspace ansi create");
        if (inst != NULL) {
            for (i = 0; i < 8; i++) {
                src_data.src = repeated_ansi;
                src_data.len = strlen(repeated_ansi);
                src_data.off = 0;
                src.userdata = &src_data;
                src.read = cstr_read;
                sink.userdata = NULL;
                sink.write = discard_write;
                before_allocs = allocs.allocs;
                before_reallocs = allocs.reallocs;
                before_frees = allocs.frees;
                st = inst->render(inst, &src, &sink);
                fails += expect(st == MDF_OK, "memory workspace repeated ansi render succeeds");
                if (i >= 4) {
                    fails += expect(allocs.allocs == before_allocs &&
                                    allocs.reallocs == before_reallocs &&
                                    allocs.frees == before_frees,
                                    "memory workspace avoids backing allocator churn for warm ansi renders");
                }
            }
            inst->destroy(inst);
            inst = NULL;
        }

        memset(&allocs, 0, sizeof(allocs));
        mdf_options_init(&opts);
        opts.allocator.userdata = &allocs;
        opts.allocator.alloc = test_alloc;
        opts.allocator.realloc = test_realloc;
        opts.allocator.free = test_free;
        st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "memory workspace html create");
        if (inst != NULL) {
            for (i = 0; i < 8; i++) {
                src_data.src = repeated_html;
                src_data.len = strlen(repeated_html);
                src_data.off = 0;
                src.userdata = &src_data;
                src.read = cstr_read;
                sink.userdata = NULL;
                sink.write = discard_write;
                before_allocs = allocs.allocs;
                before_reallocs = allocs.reallocs;
                before_frees = allocs.frees;
                st = inst->render(inst, &src, &sink);
                fails += expect(st == MDF_OK, "memory workspace repeated html render succeeds");
                if (i >= 4) {
                    fails += expect(allocs.allocs == before_allocs &&
                                    allocs.reallocs == before_reallocs &&
                                    allocs.frees == before_frees,
                                    "memory workspace avoids backing allocator churn for warm html renders");
                }
            }
            inst->destroy(inst);
            inst = NULL;
        }

        memset(&allocs, 0, sizeof(allocs));
        mdf_options_init(&opts);
        opts.width = 40;
        opts.allocator.userdata = &allocs;
        opts.allocator.alloc = test_alloc;
        opts.allocator.realloc = test_realloc;
        opts.allocator.free = test_free;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "memory workspace table create");
        if (inst != NULL) {
            late_table_allocs = 0;
            for (i = 0; i < 20; i++) {
                src_data.src = repeated_table;
                src_data.len = strlen(repeated_table);
                src_data.off = 0;
                src.userdata = &src_data;
                src.read = cstr_read;
                sink.userdata = NULL;
                sink.write = discard_write;
                before_allocs = allocs.allocs;
                before_frees = allocs.frees;
                st = inst->render(inst, &src, &sink);
                fails += expect(st == MDF_OK, "memory workspace repeated table render succeeds");
                if (i >= 10) {
                    late_table_allocs += allocs.allocs - before_allocs;
                    fails += expect(allocs.frees == before_frees,
                                    "memory workspace retains table scratch instead of freeing per render");
                }
            }
            fails += expect(late_table_allocs <= 4,
                            "memory workspace bounds late table backing allocations");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    fails += expect(allocs.allocs > 0 && allocs.frees > 0, "custom allocator observed allocations");
    fails += expect(allocs.allocs == allocs.frees, "custom allocator balanced after destroy");
    return fails == 0 ? 0 : 1;
}
