#include <libmdf/mdf.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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
    size_t writes;
} grow_sink;

typedef struct emission_log {
    char **chunks;
    size_t *lengths;
    size_t count;
    size_t cap;
    size_t bytes;
    size_t max_events;
    size_t max_bytes;
} emission_log;

typedef struct width_change_output_sink {
    mdf *renderer;
    emission_log capture;
    int calls;
    mdf_status width_status;
} width_change_output_sink;

typedef struct cstr_source {
    const char *src;
    size_t len;
    size_t off;
} cstr_source;

typedef struct chunked_cstr_source {
    const char *src;
    size_t len;
    size_t off;
    size_t max_chunk;
} chunked_cstr_source;

typedef struct width_change_source {
    mdf *renderer;
    int reads;
    mdf_status width_status;
} width_change_source;

typedef struct reflow_failure_source {
    mdf *renderer;
    int reads;
    mdf_status width_status;
} reflow_failure_source;

typedef struct render_control_source {
    mdf *renderer;
    const mdf_sink *replacement;
    int reads;
    mdf_status reset_status;
    mdf_status sink_status;
} render_control_source;

typedef struct early_render_control_source {
    mdf *renderer;
    const mdf_sink *replacement;
    const char *markdown;
    int reads;
    mdf_status reset_status;
    mdf_status sink_status;
} early_render_control_source;

typedef struct incremental_control_sink {
    mdf *renderer;
    const mdf_sink *replacement;
    grow_sink capture;
    int calls;
    mdf_status reset_status;
    mdf_status sink_status;
} incremental_control_sink;

typedef struct teardown_control_sink {
    mdf *renderer;
    const mdf_sink *replacement;
    grow_sink capture;
    int calls;
    mdf_status feed_status;
    mdf_status sink_status;
} teardown_control_sink;

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
    size_t alloc_calls;
    size_t fail_after;
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

typedef struct source_offset_probe_sink {
    grow_sink capture;
    const chunked_cstr_source *source;
    const char *needle;
    size_t needle_source_off;
    int saw_needle;
} source_offset_probe_sink;

static int join_path(char *dst, size_t dst_cap, const char *dir, const char *name)
{
    size_t dir_len;
    size_t name_len;

    if (dst_cap == 0) {
        return -1;
    }
    dir_len = strlen(dir);
    name_len = strlen(name);
    if (dir_len >= dst_cap || name_len >= dst_cap - dir_len - 1) {
        return -1;
    }
    memcpy(dst, dir, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, name, name_len + 1);
    return 0;
}

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
    tracker->alloc_calls++;
    if (tracker->fail_after != 0 && tracker->alloc_calls == tracker->fail_after) {
        return NULL;
    }
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

static size_t count_substrings(const char *haystack, const char *needle)
{
    size_t count;
    size_t needle_len;
    const char *p;

    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    count = 0;
    needle_len = strlen(needle);
    p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static int expect_malformed_front_matter_theme_is_safe(const char *name, const char *src)
{
    mdf_options opts;
    mdf *inst;
    mdf_status st;
    char *out;
    int fails;

    mdf_options_init(&opts);
    inst = NULL;
    out = NULL;
    fails = 0;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, name);
    if (st != MDF_OK || inst == NULL) {
        return fails;
    }
    st = inst->render_cstr(inst, src, &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck malformed front matter theme render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 1 &&
                    strstr(out, "Malformed Theme") != NULL &&
                    strstr(out, "Visible body") != NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "color:rgb(255,175,215)") == NULL,
                    "html deck malformed front matter theme is consumed without applying theme");
    if (out != NULL) {
        inst->string_free(inst, out);
    }
    inst->destroy(inst);
    return fails;
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
    sink->writes++;
    return 0;
}

static void grow_free(grow_sink *sink)
{
    free(sink->buf);
    sink->buf = NULL;
    sink->len = 0;
    sink->cap = 0;
    sink->writes = 0;
}

static int emission_log_append(emission_log *log, const char *src, size_t len)
{
    char **chunks;
    size_t *lengths;
    char *copy;
    size_t cap;

    if ((log->max_events > 0 && log->count == log->max_events) ||
        (log->max_bytes > 0 &&
         (len > log->max_bytes || log->bytes > log->max_bytes - len))) {
        return -1;
    }
    if (log->count == log->cap) {
        cap = log->cap == 0 ? 8 : log->cap * 2;
        chunks = (char **)realloc(log->chunks, cap * sizeof(*chunks));
        if (chunks == NULL) {
            return -1;
        }
        lengths = (size_t *)realloc(log->lengths, cap * sizeof(*lengths));
        if (lengths == NULL) {
            log->chunks = chunks;
            return -1;
        }
        log->chunks = chunks;
        log->lengths = lengths;
        log->cap = cap;
    }
    copy = (char *)malloc(len == 0 ? 1 : len);
    if (copy == NULL) {
        return -1;
    }
    if (len > 0) {
        memcpy(copy, src, len);
    }
    log->chunks[log->count] = copy;
    log->lengths[log->count] = len;
    log->count++;
    log->bytes += len;
    return 0;
}

static int emission_log_write(void *userdata, const char *src, size_t len)
{
    return emission_log_append((emission_log *)userdata, src, len);
}

static int emission_log_trace(void *userdata, mdf_format format, const char *src, size_t len)
{
    (void)format;
    return emission_log_append((emission_log *)userdata, src, len);
}

static int emission_logs_equal(const emission_log *first, const emission_log *second)
{
    size_t i;

    if (first->count != second->count) {
        return 0;
    }
    for (i = 0; i < first->count; i++) {
        if (first->lengths[i] != second->lengths[i] ||
            memcmp(first->chunks[i], second->chunks[i], first->lengths[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static int emission_log_equals_bytes(const emission_log *log, const char *src, size_t len)
{
    size_t i;
    size_t off;

    off = 0;
    for (i = 0; i < log->count; i++) {
        if (log->lengths[i] > len - off ||
            memcmp(log->chunks[i], src + off, log->lengths[i]) != 0) {
            return 0;
        }
        off += log->lengths[i];
    }
    return off == len;
}

static void emission_log_free(emission_log *log)
{
    size_t i;

    for (i = 0; i < log->count; i++) {
        free(log->chunks[i]);
    }
    free(log->chunks);
    free(log->lengths);
    memset(log, 0, sizeof(*log));
}

static int width_change_output_write(void *userdata, const char *src, size_t len)
{
    width_change_output_sink *sink;

    sink = (width_change_output_sink *)userdata;
    if (sink->calls == 0) {
        sink->width_status = sink->renderer->set_width(sink->renderer, 3);
    }
    sink->calls++;
    return emission_log_append(&sink->capture, src, len);
}

static int incremental_control_write(void *userdata, const char *src, size_t len)
{
    incremental_control_sink *sink;

    sink = (incremental_control_sink *)userdata;
    if (sink->calls == 0) {
        sink->reset_status = sink->renderer->reset(sink->renderer);
        sink->sink_status = sink->renderer->set_sink(sink->renderer, sink->replacement);
    }
    sink->calls++;
    return grow_write(&sink->capture, src, len);
}

static int teardown_control_write(void *userdata, const char *src, size_t len)
{
    teardown_control_sink *sink;

    sink = (teardown_control_sink *)userdata;
    if (sink->calls < 2) {
        sink->renderer->destroy(sink->renderer);
        sink->feed_status = sink->renderer->feed(sink->renderer, "nested ", 7);
        sink->sink_status = sink->renderer->set_sink(sink->renderer, sink->replacement);
    }
    sink->calls++;
    return grow_write(&sink->capture, src, len);
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

static size_t chunked_cstr_read(void *userdata, char *dst, size_t cap, int *err)
{
    chunked_cstr_source *src;
    size_t n;

    (void)err;
    src = (chunked_cstr_source *)userdata;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->len - src->off;
    if (n > cap) {
        n = cap;
    }
    if (src->max_chunk > 0 && n > src->max_chunk) {
        n = src->max_chunk;
    }
    memcpy(dst, src->src + src->off, n);
    src->off += n;
    return n;
}

static size_t width_change_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    width_change_source *src;
    static const char first[] = "alpha ";
    static const char second[] = "beta\n";
    const char *chunk;
    size_t len;

    (void)err;
    src = (width_change_source *)userdata;
    if (src->reads == 0) {
        chunk = first;
    } else if (src->reads == 1) {
        src->width_status = src->renderer->set_width(src->renderer, 5);
        chunk = second;
    } else {
        return 0;
    }
    len = strlen(chunk);
    if (cap < len) {
        return 0;
    }
    memcpy(dst, chunk, len);
    src->reads++;
    return len;
}

static size_t reflow_failure_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    reflow_failure_source *src;
    static const char first[] = "`abcdefghij`";
    static const char second[] = " hi\n";
    const char *chunk;
    size_t len;

    (void)err;
    src = (reflow_failure_source *)userdata;
    if (src->reads == 0) {
        chunk = first;
    } else if (src->reads == 1) {
        src->width_status = src->renderer->set_width(src->renderer, 80);
        chunk = second;
    } else {
        return 0;
    }
    len = strlen(chunk);
    if (cap < len) {
        return 0;
    }
    memcpy(dst, chunk, len);
    src->reads++;
    return len;
}

static size_t render_control_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    render_control_source *src;
    static const char first[] = "alpha ";
    static const char second[] = "beta\n";
    const char *chunk;
    size_t len;

    (void)err;
    src = (render_control_source *)userdata;
    if (src->reads == 0) {
        chunk = first;
    } else if (src->reads == 1) {
        src->reset_status = src->renderer->reset(src->renderer);
        src->sink_status = src->renderer->set_sink(src->renderer, src->replacement);
        chunk = second;
    } else {
        return 0;
    }
    len = strlen(chunk);
    if (cap < len) {
        return 0;
    }
    memcpy(dst, chunk, len);
    src->reads++;
    return len;
}

static size_t early_render_control_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    early_render_control_source *src;
    size_t len;

    src = (early_render_control_source *)userdata;
    *err = 0;
    if (src->reads != 0) {
        return 0;
    }
    src->reset_status = src->renderer->reset(src->renderer);
    src->sink_status = src->renderer->set_sink(src->renderer, src->replacement);
    len = strlen(src->markdown);
    if (cap < len) {
        *err = 1;
        return 0;
    }
    memcpy(dst, src->markdown, len);
    src->reads = 1;
    return len;
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

static int source_offset_probe_write(void *userdata, const char *src, size_t len)
{
    source_offset_probe_sink *sink;
    int rc;

    sink = (source_offset_probe_sink *)userdata;
    rc = grow_write(&sink->capture, src, len);
    if (rc == 0 && !sink->saw_needle && sink->capture.buf != NULL && strstr(sink->capture.buf, sink->needle) != NULL) {
        sink->saw_needle = 1;
        sink->needle_source_off = sink->source->off;
    }
    return rc;
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
    mdf_html_font jetbrains_font;
    char regular_font_path[128];
    char italic_font_path[128];
    char resolved_regular_font_path[128];
    char resolved_italic_font_path[128];
    char font_dump_dir_path[160];
    char regular_font_uri[192];
    char italic_font_uri[192];
    char deck_regular_font_uri[96];
    char deck_italic_font_uri[96];
    char font_dump_alias_path[192];
    struct stat regular_font_st;
    struct stat italic_font_st;
    FILE *font_fp;
    int first_font_byte;
    int font_paths_alias;
    static const unsigned char deck_font_bytes[] = {1, 2, 3};

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
    memset(&jetbrains_font, 0, sizeof(jetbrains_font));
    mdf_html_jetbrains_mono_font(&jetbrains_font);
    fails += expect(jetbrains_font.family != NULL &&
                    strcmp(jetbrains_font.family, "JetBrains Mono") == 0 &&
                    jetbrains_font.regular.format == MDF_HTML_FONT_FORMAT_WOFF2 &&
                    jetbrains_font.regular.data_len > 0 &&
                    jetbrains_font.italic.format == MDF_HTML_FONT_FORMAT_WOFF2 &&
                    jetbrains_font.italic.data_len > 0,
                    "built-in JetBrains Mono font descriptor exposes both WOFF2 faces");
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    fails += expect(st == MDF_OK, "built-in JetBrains Mono fonts dump to explicit paths");
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'w', "dumped regular JetBrains Mono font is WOFF2 data");
    font_fp = fopen(italic_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'w', "dumped italic JetBrains Mono font is WOFF2 data");
    font_fp = fopen(regular_font_path, "wb");
    if (font_fp != NULL) {
        fputs("preserve", font_fp);
        fclose(font_fp);
    }
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    fails += expect(st == MDF_OK, "default font dump preserves existing files");
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'p', "default font dump does not replace an existing font");
    st = mdf_dump_html_jetbrains_mono_font_force(regular_font_path, italic_font_path);
    fails += expect(st == MDF_OK, "forced font dump replaces existing files");
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'w', "forced font dump restores WOFF2 data");
    remove(regular_font_path);
    remove(italic_font_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-fifo-%ld", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    remove(regular_font_path);
    remove(italic_font_path);
    fails += expect(mkfifo(regular_font_path, 0600) == 0,
                    "font dump FIFO destination setup succeeds");
    st = mdf_dump_html_jetbrains_mono_font_force(regular_font_path, italic_font_path);
    fails += expect(st == MDF_ERROR_IO,
                    "forced font dump refuses FIFO destinations without blocking");
    unlink(regular_font_path);
    snprintf(font_dump_alias_path, sizeof(font_dump_alias_path),
             "/tmp/libmdf-html-font-force-target-%ld.woff2", (long)getpid());
    remove(font_dump_alias_path);
    font_fp = fopen(font_dump_alias_path, "wb");
    if (font_fp != NULL) {
        fputs("preserve", font_fp);
        fclose(font_fp);
    }
    fails += expect(symlink(font_dump_alias_path, regular_font_path) == 0,
                    "forced font dump symlink destination setup succeeds");
    st = mdf_dump_html_jetbrains_mono_font_force(regular_font_path, italic_font_path);
    fails += expect(st == MDF_ERROR_IO,
                    "forced font dump refuses symlink destinations");
    font_fp = fopen(font_dump_alias_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'p',
                    "forced font dump leaves symlink targets unchanged");
    unlink(regular_font_path);
    remove(font_dump_alias_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-case-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/LIBMDF-HTML-FONT-CASE-%ld.WOFF2", (long)getpid());
    remove(regular_font_path);
    remove(italic_font_path);
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    font_paths_alias = stat(regular_font_path, &regular_font_st) == 0 &&
                       stat(italic_font_path, &italic_font_st) == 0 &&
                       regular_font_st.st_dev == italic_font_st.st_dev &&
                       regular_font_st.st_ino == italic_font_st.st_ino;
    fails += expect((font_paths_alias && st == MDF_ERROR_INVALID) ||
                    (!font_paths_alias && st == MDF_OK),
                    "font dump rejects case-insensitive destination aliases");
    remove(regular_font_path);
    remove(italic_font_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/./libmdf-html-font-regular-%ld.woff2", (long)getpid());
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    fails += expect(st == MDF_ERROR_INVALID,
                    "font dump rejects lexically aliased destinations");
    fails += expect(access(regular_font_path, F_OK) != 0,
                    "font dump rejects lexical aliases before writing either face");
    snprintf(font_dump_dir_path, sizeof(font_dump_dir_path), "%s-directory", regular_font_path);
    snprintf(font_dump_alias_path, sizeof(font_dump_alias_path), "%s-link", regular_font_path);
    fails += expect(mkdir(font_dump_dir_path, 0700) == 0,
                    "font dump symlink alias directory setup succeeds");
    fails += expect(symlink(font_dump_dir_path, font_dump_alias_path) == 0,
                    "font dump symlink alias setup succeeds");
    if (join_path(regular_font_path, sizeof(regular_font_path), font_dump_dir_path, "face.woff2") != 0 ||
        join_path(italic_font_path, sizeof(italic_font_path), font_dump_alias_path, "face.woff2") != 0) {
        return 1;
    }
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    fails += expect(st == MDF_ERROR_INVALID,
                    "font dump rejects symlink-aliased destinations");
    fails += expect(access(regular_font_path, F_OK) != 0,
                    "font dump rejects symlink aliases before writing either face");
    unlink(font_dump_alias_path);
    rmdir(font_dump_dir_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    fails += expect(mkdir(regular_font_path, 0700) == 0,
                    "font dump directory destination setup succeeds");
    st = mdf_dump_html_jetbrains_mono_font(regular_font_path, italic_font_path);
    fails += expect(st == MDF_ERROR_IO,
                    "font dump rejects an existing directory destination");
    rmdir(regular_font_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    mdf_options_init(&opts);
    opts.html_font_uri = "fonts";
    opts.html_font_dump_regular_path = regular_font_path;
    opts.html_font_dump_italic_path = italic_font_path;
    opts.html_dump_font = 1;
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_OK && strcmp(resolved_regular_font_path, regular_font_path) == 0 &&
                    strcmp(resolved_italic_font_path, italic_font_path) == 0,
                    "public font dump preflight resolves explicit destinations without writing");
    fails += expect(access(regular_font_path, F_OK) != 0 && access(italic_font_path, F_OK) != 0,
                    "public font dump preflight does not create font files");
    fails += expect(mdf_paths_alias(regular_font_path,
                                    "/tmp/./libmdf-html-font-regular-placeholder.woff2") == 0,
                    "public path alias check distinguishes different absent destinations");
    fails += expect(mdf_paths_alias(NULL, regular_font_path) == 0 &&
                    mdf_paths_alias(regular_font_path, "") == 0,
                    "public path alias check rejects null and empty paths without probing them");
    snprintf(resolved_regular_font_path, sizeof(resolved_regular_font_path),
             "/tmp/libmdf-html-font-missing-%ld/face.woff2", (long)getpid());
    fails += expect(mdf_paths_alias(resolved_regular_font_path, resolved_regular_font_path),
                    "public path alias check detects identical absent paths with missing parent");
    snprintf(resolved_regular_font_path, sizeof(resolved_regular_font_path),
             "/tmp/./libmdf-html-font-regular-%ld.woff2", (long)getpid());
    fails += expect(mdf_paths_alias(regular_font_path, resolved_regular_font_path),
                    "public path alias check detects equivalent absent destinations");
    opts.html_dump_font = 0;
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight requires an enabled dump");
    opts.html_dump_font = 1;
    opts.html_font_dump_regular_path = "";
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight rejects empty explicit destinations");
    mdf_options_init(&opts);
    opts.html_dump_font = 1;
    opts.html_font_uri = "https:relative-fonts";
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight rejects URI schemes without an authority");
    opts.html_font_uri = "data:font/woff2;base64,AAAA";
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight rejects data URI destinations");
    opts.html_font_uri = "?v=1";
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight rejects query-only font URI bases");
    opts.html_font_uri = "#theme";
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          resolved_regular_font_path, sizeof(resolved_regular_font_path),
                                          resolved_italic_font_path, sizeof(resolved_italic_font_path));
    fails += expect(st == MDF_ERROR_INVALID,
                    "public font dump preflight rejects fragment-only font URI bases");
    mdf_options_init(&opts);
    opts.html_font_uri = "?v=1";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID && inst == NULL,
                    "HTML renderer rejects query-only font URI bases");
    opts.html_font_uri = NULL;
    opts.html_font_regular_uri = "#theme";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID && inst == NULL,
                    "HTML renderer rejects fragment-only per-face font URIs");
    mdf_options_init(&opts);
    opts.html_font_dump_regular_path = regular_font_path;
    opts.html_font_dump_italic_path = italic_font_path;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "dump paths without the dump boolean create an HTML renderer");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "passive dump paths HTML\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, "data:font/woff2;base64,") != NULL,
                        "C API dump paths do not disable embedded fonts without the dump boolean");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    fails += expect(access(regular_font_path, F_OK) != 0 && access(italic_font_path, F_OK) != 0,
                    "C API dump paths do not write files without the dump boolean");
    snprintf(font_dump_dir_path, sizeof(font_dump_dir_path), "%s#directory", regular_font_path);
    fails += expect(mkdir(font_dump_dir_path, 0700) == 0,
                    "font dump delimiter directory setup succeeds");
    mdf_options_init(&opts);
    opts.html_font_uri = "https://example.invalid/fonts";
    opts.html_font_dump_path = font_dump_dir_path;
    opts.html_dump_font = 1;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "font dump accepts a local directory containing URI delimiter characters");
    if (inst != NULL) {
        inst->destroy(inst);
        inst = NULL;
    }
    if (join_path(regular_font_path, sizeof(regular_font_path),
                  font_dump_dir_path, "JetBrainsMono-Regular.woff2") != 0) {
        return 1;
    }
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w',
                    "font dump delimiter directory receives the regular WOFF2 file");
    remove(regular_font_path);
    if (join_path(italic_font_path, sizeof(italic_font_path),
                  font_dump_dir_path, "JetBrainsMono-Italic.woff2") != 0) {
        return 1;
    }
    remove(italic_font_path);
    rmdir(font_dump_dir_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    snprintf(font_dump_dir_path, sizeof(font_dump_dir_path), "%s-uri-parent", regular_font_path);
    snprintf(regular_font_uri, sizeof(regular_font_uri), "%s/a:", font_dump_dir_path);
    if (join_path(font_dump_alias_path, sizeof(font_dump_alias_path), regular_font_uri, "/b") != 0) {
        return 1;
    }
    fails += expect(mkdir(font_dump_dir_path, 0700) == 0,
                    "font dump embedded-scheme parent directory setup succeeds");
    fails += expect(mkdir(regular_font_uri, 0700) == 0,
                    "font dump embedded-scheme intermediate directory setup succeeds");
    fails += expect(mkdir(font_dump_alias_path, 0700) == 0,
                    "font dump embedded-scheme directory setup succeeds");
    mdf_options_init(&opts);
    opts.html_font_uri = font_dump_alias_path;
    opts.html_dump_font = 1;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "implicit font dump accepts a local directory containing embedded URI syntax");
    if (inst != NULL) {
        inst->destroy(inst);
        inst = NULL;
    }
    if (join_path(regular_font_path, sizeof(regular_font_path),
                  font_dump_alias_path, "JetBrainsMono-Regular.woff2") != 0) {
        return 1;
    }
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w',
                    "implicit font dump preserves embedded URI syntax in local destinations");
    remove(regular_font_path);
    if (join_path(italic_font_path, sizeof(italic_font_path),
                  font_dump_alias_path, "JetBrainsMono-Italic.woff2") != 0) {
        return 1;
    }
    remove(italic_font_path);
    rmdir(font_dump_alias_path);
    rmdir(regular_font_uri);
    rmdir(font_dump_dir_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    snprintf(font_dump_dir_path, sizeof(font_dump_dir_path), "%s%%zz", regular_font_path);
    fails += expect(mkdir(font_dump_dir_path, 0700) == 0,
                    "font dump literal-percent directory setup succeeds");
    mdf_options_init(&opts);
    opts.html_font_uri = font_dump_dir_path;
    opts.html_font_dump_path = font_dump_dir_path;
    opts.html_dump_font = 1;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "font dump accepts a local directory containing literal percent escapes");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "literal percent font path HTML\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, font_dump_dir_path) != NULL,
                        "literal-percent dump path remains the HTML font reference");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    if (join_path(regular_font_path, sizeof(regular_font_path),
                  font_dump_dir_path, "JetBrainsMono-Regular.woff2") != 0) {
        return 1;
    }
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w',
                    "font dump preserves literal percent characters in local destinations");
    remove(regular_font_path);
    if (join_path(italic_font_path, sizeof(italic_font_path),
                  font_dump_dir_path, "JetBrainsMono-Italic.woff2") != 0) {
        return 1;
    }
    remove(italic_font_path);
    rmdir(font_dump_dir_path);
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-html-font-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-html-font-italic-%ld.woff2", (long)getpid());
    mdf_options_init(&opts);
    opts.html_font_source = MDF_HTML_FONT_SOURCE_EXTERNAL;
    opts.html_font_uri = "assets/fonts";
    opts.html_dump_font = 1;
    opts.html_font_dump_regular_path = regular_font_path;
    opts.html_font_dump_italic_path = italic_font_path;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "HTML font options dump built-in faces before renderer creation");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "dumped external font HTML\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, "assets/fonts/JetBrainsMono-Regular.woff2") != NULL &&
                        strstr(out, "assets/fonts/JetBrainsMono-Italic.woff2") != NULL &&
                        strstr(out, "data:font/woff2;base64,") == NULL,
                        "explicit dump paths preserve the configured HTML font references");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) {
        fclose(font_fp);
    }
    fails += expect(first_font_byte == 'w', "HTML font dump pre-operation writes regular WOFF2 data");
    remove(regular_font_path);
    remove(italic_font_path);

    snprintf(regular_font_uri, sizeof(regular_font_uri), "file://LOCALHOST%s", regular_font_path);
    snprintf(italic_font_uri, sizeof(italic_font_uri), "file://LOCALHOST%s", italic_font_path);
    mdf_options_init(&opts);
    opts.html_font_uri = NULL;
    opts.html_font_regular_uri = regular_font_uri;
    opts.html_font_italic_uri = italic_font_uri;
    opts.html_dump_font = 1;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "case-insensitive localhost file URI references derive local dump destinations");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "file URI font HTML\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, regular_font_uri) != NULL &&
                        strstr(out, italic_font_uri) != NULL &&
                        strstr(out, "data:font/woff2;base64,") == NULL,
                        "localhost file URI dump keeps matching external HTML references");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "localhost file URI dump writes the resolved regular destination");
    remove(regular_font_path);
    remove(italic_font_path);

    {
        tracking_allocator tracker;
        mdf_options tracked_opts;
        mdf *tracked_inst;

        memset(&tracker, 0, sizeof(tracker));
        tracker.fail_after = 5;
        mdf_options_init(&tracked_opts);
        tracked_opts.allocator.userdata = &tracker;
        tracked_opts.allocator.alloc = tracking_alloc;
        tracked_opts.allocator.realloc = tracking_realloc;
        tracked_opts.allocator.free = tracking_free;
        tracked_opts.html_font_source = MDF_HTML_FONT_SOURCE_EXTERNAL;
        tracked_opts.html_font_uri = "fonts";
        tracked_inst = NULL;
        st = mdf_create(MDF_FORMAT_HTML, &tracked_opts, &tracked_inst);
        fails += expect(st == MDF_ERROR_NOMEM && tracked_inst == NULL,
                        "external font setup reports emission-buffer allocation failure");
        fails += expect(tracker.blocks == 0 && tracker.bytes == 0,
                        "external font setup releases URI allocations after create failure");
    }

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
    fails += expect(strstr(out, "@font-face{font-family:\"JetBrains Mono\";src:url(data:font/woff2;base64,") != NULL,
                    "html defaults to embedded JetBrains Mono");
    inst->string_free(inst, out);
    out = NULL;
    st = inst->render_cstr(inst,
                           "# One\n"
                           "\n"
                           "## Two\n"
                           "\n"
                           "### Three\n"
                           "\n"
                           "#### Four\n"
                           "\n"
                           "##### Five\n"
                           "\n"
                           "###### Six\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html document heading scale render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "font-size:32pt;font-weight:700;--mdf-heading-indent:2ch;\"># One</span>") != NULL &&
                    strstr(out, "font-size:22pt;font-weight:700;--mdf-heading-indent:3ch;\">## Two</span>") != NULL &&
                    strstr(out, "font-size:15pt;font-weight:700;--mdf-heading-indent:4ch;\">### Three</span>") != NULL &&
                    strstr(out, "font-size:14pt;font-weight:700;--mdf-heading-indent:5ch;\">#### Four</span>") != NULL &&
                    strstr(out, "font-size:13pt;font-weight:700;--mdf-heading-indent:6ch;\">##### Five</span>") != NULL &&
                    strstr(out, "font-size:12.5pt;font-weight:700;--mdf-heading-indent:7ch;\">###### Six</span>") != NULL,
                    "html document heading scale uses restored ATX heading sizes");
    inst->string_free(inst, out);
    out = NULL;
    st = inst->render_cstr(inst, long_markdown, &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "X") != NULL,
                    "html preserves byte that overflows undecided prefix buffer");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.html_font_uri = "fonts";
    opts.html_font_regular_uri = "fonts/JetBrainsMono-Regular.woff2";
    opts.html_font_italic_uri = "fonts/JetBrainsMono-Italic.woff2";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "external html font renderer create succeeds");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "external font html\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "external font HTML renders");
        fails += expect(out != NULL &&
                        strstr(out, "src:url(\"fonts/JetBrainsMono-Regular.woff2\") format('woff2')") != NULL &&
                        strstr(out, "src:url(\"fonts/JetBrainsMono-Italic.woff2\") format('woff2')") != NULL &&
                        strstr(out, "data:font/woff2;base64,") == NULL,
                        "external font URIs replace embedded font data");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    mdf_options_init(&opts);
    opts.html_font_regular_uri = "fonts/</style><script>bad</script>";
    opts.html_font_italic_uri = "fonts/italic.woff2";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "CSS-escaped external URI renderer create succeeds");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "escaped external font HTML\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, "</style><script>") == NULL &&
                        strstr(out, "\\3c /style>") != NULL,
                        "external font URIs cannot terminate the HTML style element");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    mdf_options_init(&opts);
    opts.html_font_source = MDF_HTML_FONT_SOURCE_EXTERNAL;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "default external html font renderer create succeeds");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "default external font html\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "default external font HTML renders");
        fails += expect(out != NULL &&
                        strstr(out, "JetBrainsMono%5Bwght%5D.woff2") != NULL &&
                        strstr(out, "JetBrainsMono-Italic%5Bwght%5D.woff2") != NULL &&
                        strstr(out, "data:font/woff2;base64,") == NULL,
                        "default external font URIs use the embedded font's exact variable files");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    strcpy(deck_regular_font_uri, "deck-owned/regular.woff2");
    strcpy(deck_italic_font_uri, "deck-owned/italic.woff2");
    mdf_options_init(&opts);
    opts.html_font_regular_uri = deck_regular_font_uri;
    opts.html_font_italic_uri = deck_italic_font_uri;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "html deck copies external font URIs during creation");
    strcpy(deck_regular_font_uri, "caller-reused/regular.woff2");
    strcpy(deck_italic_font_uri, "caller-reused/italic.woff2");
    if (inst != NULL) {
        st = inst->render_cstr(inst, "# URI Lifetime Deck\n", &out);
        fails += expect(st == MDF_OK && out != NULL &&
                        strstr(out, "deck-owned/regular.woff2") != NULL &&
                        strstr(out, "deck-owned/italic.woff2") != NULL &&
                        strstr(out, "caller-reused/regular.woff2") == NULL &&
                        strstr(out, "caller-reused/italic.woff2") == NULL,
                        "html deck uses the instance-owned resolved font URIs");
        inst->string_free(inst, out);
        out = NULL;
        inst->destroy(inst);
        inst = NULL;
    }
    mdf_options_init(&opts);
    opts.html_font_source = MDF_HTML_FONT_SOURCE_EXTERNAL;
    opts.html_font_uri = "deck-fonts";
    opts.deck_transition = MDF_DECK_TRANSITION_CROSS;
    opts.slide_numbers = 1;
    opts.deck_center_front_text = 1;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck create succeeds");
    st = inst->render_cstr(inst,
                           "---   \n"
                           "theme: \"tokyo-night\"\n"
                           "--- \t\n"
                           "# Front\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# Chart\n"
                           "\n"
                           "```mdf-bar-chart\n"
                           "A,1\n"
                           "B,2\n"
                           "```\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck render_cstr succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->") != NULL &&
                    strstr(out, "<main class=\"mdf-deck\" data-transition=\"cross\"") != NULL &&
                    strstr(out, ".mdf-document .mdf-heading") == NULL &&
                    strstr(out, ".mdf-deck[data-transition=\"cross\"] .mdf-slide{transition:opacity 1600ms ease,visibility 0s linear 1600ms;}") != NULL &&
                    strstr(out, ".mdf-deck[data-transition=\"cross\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 1600ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(out, "deck-fonts/JetBrainsMono-Regular.woff2") != NULL &&
                    strstr(out, "deck-fonts/JetBrainsMono-Italic.woff2") != NULL &&
                    strstr(out, "data:font/woff2;base64,") == NULL &&
                    strstr(out, "<section class=\"mdf-slide mdf-slide-front mdf-center-front-text\" data-slide=\"1\" aria-hidden=\"false\">") != NULL &&
                    strstr(out, "<section class=\"mdf-slide\" data-slide=\"2\" aria-hidden=\"true\">") != NULL,
                    "html deck emits shell, front slide, and second slide");
    fails += expect(out != NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "tokyo-night") == NULL &&
                    strstr(out, "color:rgb(255,175,215)") != NULL,
                    "html deck consumes front matter with spaced delimiters and applies theme default");
    fails += expect(out != NULL &&
                    strstr(out, "mdf-slide-content mdf-slide-content-centered") != NULL &&
                    strstr(out, ".mdf-slide-content-centered:has(.mdf-chart-block){width:100%;}") != NULL &&
                    strstr(out, "<div class=\"mdf-slide-header\"><span class=\"mdf-heading\"") != NULL &&
                    strstr(out, "</div><div class=\"mdf-slide-body\"><div class=\"mdf-slide-body-inner\">") != NULL,
                    "html deck centers non-front body content by default");
    fails += expect(out != NULL &&
                    strstr(out, "<div class=\"mdf-slide-number\" aria-hidden=\"true\"></div>") != NULL &&
                    strstr(out, "padStart(w,'0')+'/'+String(slides.length).padStart(w,'0')") != NULL,
                    "html deck emits browser-filled slide number placeholders");
    fails += expect(out != NULL &&
                    strstr(out, ".mdf-slide-number{font-weight:700;opacity:.72;font-size:clamp(.8rem,1.6vw,1.1rem);color:rgb(135,175,215);}") != NULL,
                    "html deck slide numbers use the resolved theme heading color");
    fails += expect(out != NULL &&
                    strstr(out, "touchstart") != NULL &&
                    strstr(out, "touchend") != NULL &&
                    strstr(out, "Math.abs(dx)>=48") != NULL &&
                    strstr(out, "Math.abs(dy)<48") != NULL &&
                    strstr(out, "box.scrollHeight>box.clientHeight+4") != NULL &&
                    strstr(out, "show(cur+1,true)") != NULL &&
                    strstr(out, "show(cur-1,true)") != NULL,
                    "html deck emits swipe navigation handlers");
    fails += expect(out != NULL &&
                    strstr(out, "mdf-fullscreen-button") != NULL &&
                    strstr(out, "toggleFullscreen") != NULL &&
                    strstr(out, "case'f'") != NULL &&
                    strstr(out, "requestFullscreen") != NULL &&
                    strstr(out, "mdf-fallback-fullscreen") != NULL,
                    "html deck emits fullscreen control and fallback");
    fails += expect(out != NULL &&
                    strstr(out, ".mdf-deck.mdf-cursor-hidden,.mdf-deck.mdf-cursor-hidden *{cursor:none!important;}") != NULL &&
                    strstr(out, "var cursorTimer=0;function showCursor()") != NULL &&
                    strstr(out, "addEventListener('pointermove',showCursor,{passive:true})") != NULL,
                    "html deck emits cursor inactivity hiding");
    fails += expect(out != NULL &&
                    strstr(out, "function syncSlides()") != NULL &&
                    strstr(out, "data-mdf-tabindex") != NULL &&
                    strstr(out, "el.setAttribute('tabindex','-1')") != NULL &&
                    strstr(out, "t.closest('a[href],button,summary,input,select,textarea')") != NULL &&
                    strstr(out, "deck.dataset.current=String(cur+1)") != NULL,
                    "html deck synchronizes inactive slide focusability");
    fails += expect(out != NULL &&
                    strstr(out, "@media (max-height:520px)") != NULL &&
                    strstr(out, ".mdf-slide-header .mdf-heading[style*=\"font-size:32pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:32pt\"]{font-size:min(7.4vmin,7vw)!important;}") != NULL &&
                    strstr(out, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:6ch\"]{font-size:1.05em!important;}") != NULL &&
                    strstr(out, "var dense=box.textContent.length>520;var scale=Math.min(vw,vh);var base=scale*(slide.classList.contains('mdf-slide-front') ? .072 : (dense ? .038 : .047));") != NULL &&
                    strstr(out, "var vh=Math.max") != NULL &&
                    strstr(out, "if(vh<520)") != NULL,
                    "html deck emits viewport-adaptive autofit rules");
    fails += expect(out != NULL &&
                    strstr(out, "<div class=\"mdf-chart-block\"") != NULL &&
                    strstr(out, "66.7%") != NULL,
                    "html deck supports charts inside slides");
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "manual";
    tok.len = strlen(tok.text);
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_INVALID && sink_data.len == 0,
                    "html deck write_token rejects manual token streaming");
    fails += expect(strcmp(inst->error(inst), "deck renderers do not support token streaming") == 0,
                    "html deck write_token reports unsupported token streaming");
    st = mdf_finish(inst, &sink);
    fails += expect(st == MDF_ERROR_INVALID && sink_data.len == 0,
                    "html deck finish rejects manual token streaming");
    grow_free(&sink_data);
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html styled link labels create succeeds");
    st = inst->render_cstr(inst,
                           "[`code`](https://example.com) [*emphasis*](https://example.com) [**strong**](https://example.com)\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html styled link labels render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<a href=\"https://example.com\"") != NULL &&
                    strstr(out, "`code`") == NULL &&
                    strstr(out, "*emphasis*") == NULL &&
                    strstr(out, "**strong**") == NULL,
                    "html styled link labels consume Markdown delimiters");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.theme_name = "ayu-light";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ayu-light html inline code renderer create succeeds");
    st = inst->render_cstr(inst, "Use `mdf`.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ayu-light html inline code render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "color:rgb(135,175,175);font-size:12pt;font-weight:700;\">mdf</span>") != NULL,
                    "ayu-light html inline code retains its body-sized heading style");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck lower heading create succeeds");
    st = inst->render_cstr(inst,
                           "# Front\n"
                           "\n"
                           "---\n"
                           "\n"
                           "## Second-Level Slide Header\n"
                           "\n"
                           "Second-level body.\n"
                           "\n"
                           "---\n"
                           "\n"
                           "### Third-Level Slide Header\n"
                           "\n"
                           "Third-level body.\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck lower heading render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<div class=\"mdf-slide-header\"><span class=\"mdf-heading\"") != NULL &&
                    strstr(out, ">## Second-Level Slide Header</span></div><div class=\"mdf-slide-body\"") != NULL &&
                    strstr(out, ">### Third-Level Slide Header</span></div><div class=\"mdf-slide-body\"") != NULL,
                    "html deck promotes lower-level first headings to slide headers");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html normal link target create succeeds");
    st = inst->render_cstr(inst, "[Plain Link](https://example.com)\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html normal link target render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<a href=\"https://example.com\"") != NULL &&
                    strstr(out, "target=\"_blank\"") == NULL &&
                    strstr(out, "rel=\"noopener noreferrer\"") == NULL,
                    "html normal links do not force a new tab");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.theme_name = "horizon";
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "horizon html deck styled link labels create succeeds");
    st = inst->render_cstr(inst,
                           "# Horizon\n\nUse `operationId`; [`code`](https://example.com) [*emphasis*](https://example.com) [**strong**](https://example.com)\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "horizon html deck styled link labels render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "`operationId`") == NULL &&
                    strstr(out, "*emphasis*") == NULL &&
                    strstr(out, "**strong**") == NULL,
                    "horizon html deck styled link labels consume Markdown delimiters");
    fails += expect(out != NULL &&
                    strstr(out, "color:rgb(255,135,95);font-size:12pt;font-weight:700;") == NULL,
                    "horizon html deck inline code is not mistaken for a heading");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck link target create succeeds");
    st = inst->render_cstr(inst, "# Links\n\n[Deck Link](https://example.com)\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck link target render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<a href=\"https://example.com\" target=\"_blank\" rel=\"noopener noreferrer\"") != NULL,
                    "html deck links open in a new tab with opener protection");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck raw html semantics create succeeds");
    st = inst->render_cstr(inst,
                           "# Raw HTML\n"
                           "\n"
                           "<p data-deck-fixture=\"raw-html\">Raw inline HTML</p>\n"
                           "\n"
                           "<details><summary>Disclosure content</summary>Body</details>\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck raw html semantics render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "&lt;p data-deck-fixture=&#34;raw-html&#34;&gt;Raw inline HTML&lt;/p&gt;") != NULL &&
                    strstr(out, "&lt;details&gt;&lt;summary&gt;Disclosure content&lt;/summary&gt;Body&lt;/details&gt;") != NULL &&
                    strstr(out, "<p data-deck-fixture=\"raw-html\">") == NULL &&
                    strstr(out, "<details><summary>Disclosure content</summary>") == NULL,
                    "html deck preserves escaped raw HTML renderer semantics");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck explicit title create succeeds");
    st = mdf_set_html_title(inst, "Deck & <API>");
    fails += expect(st == MDF_OK, "html deck explicit title setter succeeds");
    st = inst->render_cstr(inst, "# Titled Deck\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck explicit title render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<title>Deck &amp; &lt;API&gt;</title>") != NULL &&
                    strstr(out, "<main class=\"mdf-deck\"") != NULL,
                    "html deck explicit title is escaped in shell");
    inst->string_free(inst, out);
    out = NULL;
    st = mdf_set_html_title(inst, NULL);
    fails += expect(st == MDF_OK, "html deck title clear succeeds");
    st = inst->render_cstr(inst, "# Cleared Deck Title\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck cleared title render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<title>Cleared Deck Title</title>") != NULL &&
                    strstr(out, "<title>Deck &amp; &lt;API&gt;</title>") == NULL &&
                    strstr(out, "<main class=\"mdf-deck\"") != NULL,
                    "html deck cleared title returns to automatic shell title");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.boring = 1;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck boring create succeeds");
    st = inst->render_cstr(inst,
                           "# Boring Deck\n"
                           "\n"
                           "```mdf-bar-chart\n"
                           "A,1\n"
                           "B,2\n"
                           "```\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck boring render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "html,body{margin:0;min-height:100%;background:transparent;}") != NULL &&
                    strstr(out, "body{color:rgb(0,0,0);") != NULL &&
                    strstr(out, "<main class=\"mdf-deck\"") != NULL,
                    "html deck boring mode applies base html colors");
    fails += expect(out != NULL &&
                    strstr(out, "<div class=\"mdf-chart-block\"") != NULL &&
                    strstr(out, "background-color:rgb(205,0,205)") == NULL &&
                    strstr(out, "background-color:rgb(59,156,255)") == NULL,
                    "html deck boring mode suppresses themed chart colors");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.html_content_width_ch = 72.0;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck content width create succeeds");
    st = inst->render_cstr(inst, "# Width Deck\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck content width render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "--mdf-content-max-width:72ch;") != NULL &&
                    strstr(out, "<main class=\"mdf-deck\"") != NULL,
                    "html deck preserves html content width setting");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.html_font.family = "Deck Mono";
    opts.html_font.regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
    opts.html_font.regular.data = deck_font_bytes;
    opts.html_font.regular.data_len = sizeof(deck_font_bytes);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck embedded font create succeeds");
    st = inst->render_cstr(inst, "# Font Deck\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck embedded font render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "@font-face{font-family:\"Deck Mono\";src:url(data:font/woff2;base64,AQID)") != NULL &&
                    strstr(out, "font-family:\"Deck Mono\",monospace") != NULL &&
                    strstr(out, "<main class=\"mdf-deck\"") != NULL,
                    "html deck preserves embedded html font settings");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck unknown front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "title: Metadata Title\n"
                           "author: Deck Author\n"
                           "date: 2026-07-02\n"
                           "deck: reserved metadata\n"
                           "future_option: ignored value\n"
                           "---\n"
                           "# Unknown Metadata\n"
                           "\n"
                           "Visible body\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck unknown front matter render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "Unknown Metadata") != NULL &&
                    strstr(out, "Visible body") != NULL &&
                    strstr(out, "deck:") == NULL &&
                    strstr(out, "title:") == NULL &&
                    strstr(out, "author:") == NULL &&
                    strstr(out, "date:") == NULL &&
                    strstr(out, "Metadata Title") == NULL &&
                    strstr(out, "Deck Author") == NULL &&
                    strstr(out, "2026-07-02") == NULL &&
                    strstr(out, "future_option:") == NULL &&
                    strstr(out, "reserved metadata") == NULL &&
                    strstr(out, "ignored value") == NULL,
                    "html deck ignores reserved unknown front matter keys");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck YAML list front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "title: Metadata Title\n"
                           "tags:\n"
                           "  - alpha\n"
                           "  - beta\n"
                           "---\n"
                           "# YAML Metadata\n"
                           "\n"
                           "Visible body\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck YAML list front matter render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 1 &&
                    strstr(out, "YAML Metadata") != NULL &&
                    strstr(out, "Visible body") != NULL &&
                    strstr(out, "title:") == NULL &&
                    strstr(out, "tags:") == NULL &&
                    strstr(out, "alpha") == NULL &&
                    strstr(out, "beta") == NULL &&
                    strstr(out, "Metadata Title") == NULL,
                    "html deck consumes YAML list continuation front matter");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.slide_numbers = 1;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck CRLF front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\r\n"
                           "theme: \"tokyo-night\"\r\n"
                           "---\r\n"
                           "# Front\r\n"
                           "\r\n"
                           "---\r\n"
                           "\r\n"
                           "# Body\r\n"
                           "\r\n"
                           "Content\r\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck CRLF front matter render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "color:rgb(255,175,215)") != NULL &&
                    strstr(out, ".mdf-slide-number{font-weight:700;opacity:.72;font-size:clamp(.8rem,1.6vw,1.1rem);color:rgb(135,175,215);}") != NULL,
                    "html deck CRLF front matter applies theme");
    fails += expect(out != NULL &&
                    strstr(out, "mdf-slide-content mdf-slide-content-centered") != NULL &&
                    strstr(out, "<div class=\"mdf-slide-body\">") != NULL,
                    "html deck CRLF front matter keeps centered body default");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck commented front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "theme: \"tokyo-night\"\n"
                           "# unindented comment\n"
                           "---\n"
                           "# Commented Metadata\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# Body\n"
                           "\n"
                           "Content\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck commented front matter render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "unindented comment") == NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "color:rgb(255,175,215)") != NULL &&
                    strstr(out, "mdf-slide-content mdf-slide-content-centered") != NULL,
                    "html deck consumes commented front matter and applies options");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck invalid front matter theme create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "theme: \"not-a-theme\"\n"
                           "---\n"
                           "# Invalid Theme Metadata\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# Body\n"
                           "\n"
                           "Content\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck invalid front matter theme render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "not-a-theme") == NULL &&
                    strstr(out, "Invalid Theme Metadata") != NULL &&
                    strstr(out, "color:rgb(255,175,215)") == NULL &&
                    strstr(out, ".mdf-slide-number{font-weight:700;opacity:.72;font-size:clamp(.8rem,1.6vw,1.1rem);color:rgb(135,175,215);}") == NULL,
                    "html deck consumes invalid front matter theme and keeps default theme");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    fails += expect_malformed_front_matter_theme_is_safe(
        "html deck single double-quote front matter theme create succeeds",
        "---\n"
        "theme: \"\n"
        "---\n"
        "# Malformed Theme\n"
        "\n"
        "Visible body\n");
    fails += expect_malformed_front_matter_theme_is_safe(
        "html deck single single-quote front matter theme create succeeds",
        "---\n"
        "theme: '\n"
        "---\n"
        "# Malformed Theme\n"
        "\n"
        "Visible body\n");
    fails += expect_malformed_front_matter_theme_is_safe(
        "html deck empty double-quoted front matter theme create succeeds",
        "---\n"
        "theme: \"\"\n"
        "---\n"
        "# Malformed Theme\n"
        "\n"
        "Visible body\n");
    fails += expect_malformed_front_matter_theme_is_safe(
        "html deck unterminated quoted front matter theme create succeeds",
        "---\n"
        "theme: \"tokyo-night\n"
        "---\n"
        "# Malformed Theme\n"
        "\n"
        "Visible body\n");

    mdf_options_init(&opts);
    opts.theme_name = "default";
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck explicit default theme create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "theme: \"tokyo-night\"\n"
                           "---\n"
                           "# Explicit Default\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck explicit default theme render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "theme:") == NULL &&
                    strstr(out, "color:rgb(255,175,215)") == NULL,
                    "html deck explicit theme option overrides front matter theme");
    fails += expect(out != NULL &&
                    strstr(out, "color:rgb(135,175,215)") == NULL,
                    "html deck explicit theme option overrides slide-number theme color");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck default create succeeds");
    st = inst->render_cstr(inst, "# One\n\n---\n\n# Two\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck default render succeeds");
    fails += expect(out != NULL &&
                    strstr(out, "<main class=\"mdf-deck\" data-transition=\"fade\"") != NULL &&
                    strstr(out, ".mdf-deck[data-transition=\"fade\"] .mdf-slide{transition:opacity 520ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(out, ".mdf-deck[data-transition=\"fade\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 520ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(out, "matchMedia('(prefers-reduced-motion: reduce)')") != NULL &&
                    strstr(out, "deck.dataset.transition==='fade'&&!reduceMotion&&old!==cur") != NULL &&
                    strstr(out, "setTimeout(function(){apply();void deck.offsetWidth;deck.classList.remove('mdf-blackout');},520)") != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "<div class=\"mdf-slide-number\" aria-hidden=\"true\"></div>") == NULL,
                    "html deck defaults to symmetric slower fade and omits slide numbers");
    fails += expect(out != NULL && count_substrings(out, "<section class=\"mdf-slide mdf-slide-front\"") == 1,
                    "html deck emits front-slide class only once");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.deck_transition = MDF_DECK_TRANSITION_HARD;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck hard transition create succeeds");
    st = inst->render_cstr(inst, "# Hard\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<main class=\"mdf-deck\" data-transition=\"hard\"") != NULL &&
                    strstr(out, ".mdf-deck[data-transition=\"hard\"] .mdf-slide{transition:none;}") != NULL,
                    "html deck hard transition is reflected in output");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    opts.deck_transition = (mdf_deck_transition)99;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID && inst == NULL,
                    "html deck rejects invalid transition enum");

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck separator create succeeds");
    st = inst->render_cstr(inst,
                           "# A\n"
                           "\n"
                           "***\n"
                           "\n"
                           "# B\n"
                           "\n"
                           "___\n"
                           "\n"
                           "# C\n"
                           "\n"
                           "   ---\n"
                           "\n"
                           "# D\n"
                           "\n"
                           "```mdf-bar-chart\n"
                           "A,1\n"
                           "---\n"
                           "B,2\n"
                           "```\n"
                           "\n"
                           "````text\n"
                           "```\n"
                           "---\n"
                           "inside long fence\n"
                           "````\n"
                           "\n"
                           "    ```\n"
                           "    indented code is not a deck fence\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# E\n"
                           "\n"
                           "> ---\n"
                           "\n"
                           "- list item\n"
                           "\n"
                           "  ---\n"
                           "\n"
                           "still C\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck separator render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 5 &&
                    strstr(out, "still C") != NULL &&
                    strstr(out, "# D") != NULL &&
                    strstr(out, "# E") != NULL &&
                    strstr(out, "inside long fence") != NULL &&
                    strstr(out, "indented code is not a deck fence") != NULL &&
                    strstr(out, "\">---</span>") != NULL &&
                    strstr(out, "<div class=\"mdf-chart-block\"") != NULL,
                    "html deck splits thematic separators including indentation but not fenced quoted list indented-code or chart-fence content");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck lazy blockquote separator create succeeds");
    st = inst->render_cstr(inst,
                           "> Quote\n"
                           "---\n"
                           "After\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# Next\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck lazy blockquote separator render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "Quote") != NULL &&
                    strstr(out, "After") != NULL &&
                    strstr(out, "Next") != NULL,
                    "html deck keeps lazy blockquote thematic-looking line on current slide");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck closed blockquote separator create succeeds");
    st = inst->render_cstr(inst,
                           "> Quote\n"
                           "\n"
                           "---\n"
                           "\n"
                           "# Next\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck closed blockquote separator render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "Quote") != NULL &&
                    strstr(out, "Next") != NULL,
                    "html deck resumes slide splitting after blockquote blank line");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck comment-only front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "# deck comment\n"
                           "---\n"
                           "# First\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck comment-only front matter render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 1 &&
                    strstr(out, "deck comment") != NULL &&
                    strstr(out, "# First") != NULL &&
                    strstr(out, "<section class=\"mdf-slide mdf-slide-front\"") != NULL,
                    "html deck preserves closed comment-only front matter as slide content");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck unclosed front matter ambiguity create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "theme: default\n"
                           "# First\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck unclosed front matter ambiguity render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "data-slide=\"2\"") != NULL &&
                    strstr(out, "theme: default") != NULL &&
                    strstr(out, "# First") != NULL,
                    "html deck treats unclosed metadata-looking leading separator as a slide break");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck rejected front matter create succeeds");
    st = inst->render_cstr(inst,
                           "---\n"
                           "theme: default\n"
                           "Body\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck rejected keyed front matter render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "data-slide=\"2\"") != NULL &&
                    strstr(out, "theme: default") != NULL &&
                    strstr(out, "Body") != NULL,
                    "html deck preserves rejected keyed front matter body after leading separator");
    inst->string_free(inst, out);
    out = NULL;

    st = inst->render_cstr(inst,
                           "---\n"
                           "Not metadata\n",
                           &out);
    fails += expect(st == MDF_OK && out != NULL, "html deck rejected front matter render succeeds");
    fails += expect(out != NULL &&
                    count_substrings(out, "<section class=\"mdf-slide") == 2 &&
                    strstr(out, "data-slide=\"2\"") != NULL &&
                    strstr(out, "Not metadata") != NULL,
                    "html deck treats rejected leading separator as a slide break");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck source failure create succeeds");
    src.userdata = NULL;
    src.read = fail_read;
    sink.userdata = NULL;
    sink.write = discard_write;
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "html deck render surfaces source read failure");
    fails += expect(strcmp(inst->error(inst), "source read failed") == 0,
                    "html deck source failure exposes error text");
    st = inst->render_cstr(inst, "# After Source Failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "After Source Failure") != NULL,
                    "html deck handle recovers after source read failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html deck success clears source failure text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck sink failure create succeeds");
    memset(&src_data, 0, sizeof(src_data));
    src_data.src = "# Sink Failure\n";
    src_data.len = strlen(src_data.src);
    src.userdata = &src_data;
    src.read = cstr_read;
    sink.userdata = NULL;
    sink.write = fail_write;
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "html deck render surfaces sink write failure");
    fails += expect(strcmp(inst->error(inst), "sink write failed") == 0,
                    "html deck sink failure exposes error text");
    st = inst->render_cstr(inst, "# After Sink Failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "After Sink Failure") != NULL,
                    "html deck handle recovers after sink write failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html deck success clears sink failure text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;

    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck delayed source failure create succeeds");
    memset(&read_fail_after_alloc, 0, sizeof(read_fail_after_alloc));
    read_fail_after_alloc.src = "---\ntheme: delayed failure\n";
    read_fail_after_alloc.len = strlen(read_fail_after_alloc.src);
    src.userdata = &read_fail_after_alloc;
    src.read = one_chunk_then_fail_read;
    sink.userdata = NULL;
    sink.write = discard_write;
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_IO, "html deck render surfaces delayed source read failure");
    fails += expect(strcmp(inst->error(inst), "source read failed") == 0,
                    "html deck delayed source read failure exposes error text");
    st = inst->render_cstr(inst, "# After Source Failure\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strstr(out, "After Source Failure") != NULL,
                    "html deck handle recovers after source read failure");
    fails += expect(strcmp(inst->error(inst), "") == 0,
                    "html deck success clears source read failure text");
    inst->string_free(inst, out);
    out = NULL;
    inst->destroy(inst);
    inst = NULL;
    fails += expect(allocs.allocs == allocs.frees,
                    "html deck delayed source read failure releases renderer-owned buffers");

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck streaming create succeeds");
    if (inst != NULL) {
        static const char deck_stream_markdown[] = "# First Slide\n\n---\n\n# Second Slide\n";
        chunked_cstr_source stream_src;
        source_offset_probe_sink stream_sink;
        mdf_source source;
        mdf_sink sink;
        const char *second;

        memset(&stream_src, 0, sizeof(stream_src));
        stream_src.src = deck_stream_markdown;
        stream_src.len = strlen(deck_stream_markdown);
        stream_src.max_chunk = 1;
        memset(&stream_sink, 0, sizeof(stream_sink));
        stream_sink.source = &stream_src;
        stream_sink.needle = "First Slide";
        source.userdata = &stream_src;
        source.read = chunked_cstr_read;
        sink.userdata = &stream_sink;
        sink.write = source_offset_probe_write;
        second = strstr(deck_stream_markdown, "# Second Slide");
        st = mdf_render(inst, &source, &sink);
        fails += expect(st == MDF_OK, "html deck streaming render succeeds");
        fails += expect(stream_sink.saw_needle, "html deck streaming emits first slide");
        fails += expect(second != NULL &&
                        stream_sink.needle_source_off <= (size_t)(second - deck_stream_markdown),
                        "html deck streaming emits first slide before reading second slide");
        fails += expect(stream_sink.capture.buf != NULL &&
                        strstr(stream_sink.capture.buf, "Second Slide") != NULL,
                        "html deck streaming eventually emits second slide");
        grow_free(&stream_sink.capture);
        inst->destroy(inst);
        inst = NULL;
    }

    {
        grow_sink original_capture;
        grow_sink replacement_capture;
        mdf_sink original_sink;
        mdf_sink replacement_sink;
        early_render_control_source control_source;
        const mdf_format formats[] = {MDF_FORMAT_HTML, MDF_FORMAT_HTML_DECK};
        size_t format_index;

        memset(&original_capture, 0, sizeof(original_capture));
        memset(&replacement_capture, 0, sizeof(replacement_capture));
        original_sink.userdata = &original_capture;
        original_sink.write = grow_write;
        replacement_sink.userdata = &replacement_capture;
        replacement_sink.write = grow_write;
        for (format_index = 0; format_index < sizeof(formats) / sizeof(formats[0]); format_index++) {
            mdf_options_init(&opts);
            st = mdf_create(formats[format_index], &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL,
                            "early lifecycle guard renderer creates");
            if (inst == NULL) {
                continue;
            }
            st = inst->set_sink(inst, &original_sink);
            memset(&control_source, 0, sizeof(control_source));
            control_source.renderer = inst;
            control_source.replacement = &replacement_sink;
            control_source.markdown = "# Guarded Title\n\nbody\n";
            src.userdata = &control_source;
            src.read = early_render_control_source_read;
            if (st == MDF_OK) st = inst->render(inst, &src);
            fails += expect(st == MDF_OK &&
                            control_source.reset_status == MDF_ERROR_INVALID &&
                            control_source.sink_status == MDF_ERROR_INVALID &&
                            original_capture.buf != NULL &&
                            strstr(original_capture.buf, "Guarded Title") != NULL &&
                            replacement_capture.len == 0,
                            "automatic-title source callback cannot interrupt an active render");
            inst->destroy(inst);
            inst = NULL;
            grow_free(&original_capture);
        }
        fails += expect(replacement_capture.len == 0,
                        "automatic-title lifecycle guard preserves the original sink for html and deck");
        grow_free(&replacement_capture);
    }

    {
        incremental_control_sink control_sink;
        grow_sink replacement_capture;
        mdf_sink bound_sink;
        mdf_sink replacement_sink;

        memset(&control_sink, 0, sizeof(control_sink));
        memset(&replacement_capture, 0, sizeof(replacement_capture));
        bound_sink.userdata = &control_sink;
        bound_sink.write = incremental_control_write;
        replacement_sink.userdata = &replacement_capture;
        replacement_sink.write = grow_write;
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.memory.max_retained_bytes = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "incremental callback lifecycle guard renderer creates");
        if (inst != NULL) {
            control_sink.renderer = inst;
            control_sink.replacement = &replacement_sink;
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) st = inst->feed(inst, "hello world\n", strlen("hello world\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && control_sink.calls > 0 &&
                            control_sink.reset_status == MDF_ERROR_INVALID &&
                            control_sink.sink_status == MDF_ERROR_INVALID &&
                            control_sink.capture.buf != NULL &&
                            strstr(control_sink.capture.buf, "hello world") != NULL &&
                            replacement_capture.len == 0,
                            "incremental sink callback cannot reset or replace an active renderer");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&control_sink.capture);
        grow_free(&replacement_capture);
    }

    {
        teardown_control_sink control_sink;
        grow_sink replacement_capture;
        mdf_sink bound_sink;
        mdf_sink replacement_sink;
        mdf_token token;

        memset(&control_sink, 0, sizeof(control_sink));
        memset(&replacement_capture, 0, sizeof(replacement_capture));
        bound_sink.userdata = &control_sink;
        bound_sink.write = teardown_control_write;
        replacement_sink.userdata = &replacement_capture;
        replacement_sink.write = grow_write;
        mdf_options_init(&opts);
        opts.boring = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "teardown callback lifecycle guard renderer creates");
        if (inst != NULL) {
            control_sink.renderer = inst;
            control_sink.replacement = &replacement_sink;
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) st = inst->reset(inst);
            fails += expect(st == MDF_OK && control_sink.calls == 1 &&
                            control_sink.feed_status == MDF_ERROR_INVALID &&
                            control_sink.sink_status == MDF_ERROR_INVALID,
                            "reset sink callback cannot destroy or reenter a renderer");
            if (st == MDF_OK) st = inst->set_sink(inst, &replacement_sink);
            fails += expect(st == MDF_OK && control_sink.calls == 2 &&
                            control_sink.feed_status == MDF_ERROR_INVALID &&
                            control_sink.sink_status == MDF_ERROR_INVALID,
                            "sink replacement protects its terminal cleanup callback");
            if (st == MDF_OK) st = inst->feed(inst, "fresh output\n", strlen("fresh output\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && replacement_capture.buf != NULL &&
                            strstr(replacement_capture.buf, "fresh output") != NULL,
                            "renderer remains usable after guarded terminal cleanup");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&control_sink.capture);
        grow_free(&replacement_capture);

        memset(&control_sink, 0, sizeof(control_sink));
        bound_sink.userdata = &control_sink;
        bound_sink.write = teardown_control_write;
        mdf_options_init(&opts);
        opts.boring = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "manual-token callback lifecycle guard renderer creates");
        if (inst != NULL) {
            control_sink.renderer = inst;
            control_sink.replacement = &replacement_sink;
            memset(&token, 0, sizeof(token));
            token.type = MDF_TOKEN_TEXT;
            token.text = "manual";
            token.len = 6;
            st = mdf_write_token(inst, &token, &bound_sink);
            if (st == MDF_OK) {
                memset(&token, 0, sizeof(token));
                token.type = MDF_TOKEN_DOCUMENT_END;
                st = mdf_write_token(inst, &token, &bound_sink);
            }
            if (st == MDF_OK) st = mdf_finish(inst, &bound_sink);
            fails += expect(st == MDF_OK, "manual-token render completes after guarded callback");
            fails += expect(control_sink.calls > 0,
                            "manual-token rendering invokes the control sink");
            fails += expect(control_sink.feed_status == MDF_ERROR_INVALID &&
                            control_sink.sink_status == MDF_ERROR_INVALID,
                            "manual-token sink callback cannot destroy or reenter a renderer");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&control_sink.capture);
    }

    mdf_options_init(&opts);
    fail_allocs.alloc_calls = 0;
    fail_allocs.realloc_calls = 0;
    fail_allocs.fail_after = 32;
    opts.allocator.userdata = &fail_allocs;
    opts.allocator.alloc = failing_alloc;
    opts.allocator.realloc = failing_realloc;
    opts.allocator.free = failing_free;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL,
                    "html deck create succeeds before slide renderer allocation exhaustion");
    if (inst != NULL) {
        fail_allocs.alloc_calls = 0;
        fail_allocs.realloc_calls = 0;
        fail_allocs.fail_after = 1;
        st = inst->render_cstr(inst, "# Allocation Failure\n", &out);
        fails += expect(st == MDF_ERROR_NOMEM && out == NULL,
                        "html deck slide renderer creation surfaces allocator exhaustion as oom");
        fails += expect(strcmp(inst->error(inst), "out of memory") == 0,
                        "html deck slide renderer creation exposes allocator exhaustion text");
        inst->destroy(inst);
        inst = NULL;
    }

    {
        size_t fail_after;
        int saw_front_theme_oom;

        saw_front_theme_oom = 0;
        for (fail_after = 1; fail_after < 80; fail_after++) {
            mdf_options_init(&opts);
            opts.allocator.userdata = &fail_allocs;
            opts.allocator.alloc = failing_alloc;
            opts.allocator.realloc = failing_realloc;
            opts.allocator.free = failing_free;
            fail_allocs.alloc_calls = 0;
            fail_allocs.realloc_calls = 0;
            fail_allocs.fail_after = 4096;
            st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL,
                            "html deck repeated front matter theme allocation-failure create succeeds");
            if (inst == NULL) {
                continue;
            }
            fail_allocs.alloc_calls = 0;
            fail_allocs.realloc_calls = 0;
            fail_allocs.fail_after = fail_after;
            st = inst->render_cstr(inst,
                                   "---\n"
                                   "theme: default\n"
                                   "theme: tokyo-night\n"
                                   "---\n"
                                   "# Front\n",
                                   &out);
            fails += expect(st == MDF_OK || st == MDF_ERROR_NOMEM || st == MDF_ERROR_IO,
                            "html deck repeated front matter theme allocation failure is bounded");
            if (st != MDF_OK) {
                saw_front_theme_oom = 1;
                fails += expect(out == NULL,
                                "html deck repeated front matter theme allocation failure leaves no output string");
            } else if (out != NULL) {
                inst->string_free(inst, out);
                out = NULL;
            }
            inst->destroy(inst);
            inst = NULL;
        }
        fails += expect(saw_front_theme_oom,
                        "html deck repeated front matter theme allocation failure was exercised");
    }

    mdf_options_init(&opts);
    opts.slide_numbers = 1;
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck slide-number streaming create succeeds");
    if (inst != NULL) {
        static const char deck_stream_markdown[] = "# First Numbered Slide\n\n---\n\n# Second Numbered Slide\n";
        chunked_cstr_source stream_src;
        source_offset_probe_sink stream_sink;
        mdf_source source;
        mdf_sink sink;
        const char *second;

        memset(&stream_src, 0, sizeof(stream_src));
        stream_src.src = deck_stream_markdown;
        stream_src.len = strlen(deck_stream_markdown);
        stream_src.max_chunk = 1;
        memset(&stream_sink, 0, sizeof(stream_sink));
        stream_sink.source = &stream_src;
        stream_sink.needle = "First Numbered Slide";
        source.userdata = &stream_src;
        source.read = chunked_cstr_read;
        sink.userdata = &stream_sink;
        sink.write = source_offset_probe_write;
        second = strstr(deck_stream_markdown, "# Second Numbered Slide");
        st = mdf_render(inst, &source, &sink);
        fails += expect(st == MDF_OK, "html deck slide-number streaming render succeeds");
        fails += expect(stream_sink.saw_needle, "html deck slide-number streaming emits first slide");
        fails += expect(second != NULL &&
                        stream_sink.needle_source_off <= (size_t)(second - deck_stream_markdown),
                        "html deck slide-number streaming emits first slide before reading second slide");
        fails += expect(stream_sink.capture.buf != NULL &&
                        strstr(stream_sink.capture.buf, "<div class=\"mdf-slide-number\" aria-hidden=\"true\"></div>") != NULL &&
                        strstr(stream_sink.capture.buf, "Second Numbered Slide") != NULL,
                        "html deck slide-number streaming uses placeholders and eventually emits second slide");
        grow_free(&stream_sink.capture);
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "html deck ambiguous leading separator streaming create succeeds");
    if (inst != NULL) {
        static const char deck_stream_markdown[] = "---\n# First Slide\n";
        chunked_cstr_source stream_src;
        source_offset_probe_sink stream_sink;
        mdf_source source;
        mdf_sink sink;

        memset(&stream_src, 0, sizeof(stream_src));
        stream_src.src = deck_stream_markdown;
        stream_src.len = strlen(deck_stream_markdown);
        stream_src.max_chunk = 1;
        memset(&stream_sink, 0, sizeof(stream_sink));
        stream_sink.source = &stream_src;
        stream_sink.needle = "First Slide";
        source.userdata = &stream_src;
        source.read = chunked_cstr_read;
        sink.userdata = &stream_sink;
        sink.write = source_offset_probe_write;
        st = mdf_render(inst, &source, &sink);
        fails += expect(st == MDF_OK, "html deck ambiguous leading separator streaming render succeeds");
        fails += expect(stream_sink.saw_needle, "html deck ambiguous leading separator preserves first slide");
        fails += expect(stream_sink.capture.buf != NULL &&
                        count_substrings(stream_sink.capture.buf, "<section class=\"mdf-slide") == 2,
                        "html deck ambiguous leading separator streams as a slide break");
        grow_free(&stream_sink.capture);
        inst->destroy(inst);
        inst = NULL;
    }

    mdf_options_init(&opts);
    opts.deck_transition = MDF_DECK_TRANSITION_HARD;
    opts.slide_numbers = 1;
    opts.deck_center_front_text = 1;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
    fails += expect(st == MDF_OK && inst != NULL, "ordinary html ignores deck-only options");
    st = inst->render_cstr(inst, "# Not Deck\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "mdf-deck") == NULL &&
                    strstr(out, "mdf-slide-number") == NULL &&
                    strstr(out, "mdf-slide-content") == NULL &&
                    strstr(out, "data-transition=\"hard\"") == NULL &&
                    strstr(out, "mdf-blackout") == NULL &&
                    strstr(out, "<main class=\"mdf-document\">") != NULL,
                    "ordinary html remains non-deck output with all deck options set");
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

    st = mdf_render(NULL, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null self");
    st = mdf_render(inst, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null source and sink");
    src.userdata = NULL;
    src.read = NULL;
    sink.userdata = NULL;
    sink.write = grow_write;
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null source callback");
    sink.write = NULL;
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null sink callback");
    st = inst->render_cstr(inst, NULL, &out);
    fails += expect(st == MDF_ERROR_INVALID && out == NULL, "render_cstr rejects null markdown");
    st = inst->render_cstr(inst, "# hi\n", NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render_cstr rejects null out");
    st = mdf_write_token(NULL, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null self");
    st = mdf_write_token(inst, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null token and sink");
    sink.userdata = NULL;
    sink.write = NULL;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "write_token rejects null sink callback");
    st = mdf_finish(NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null self");
    st = mdf_finish(inst, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null sink");
    st = mdf_finish(inst, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "finish rejects null sink callback");

    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = NULL;
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_ERROR_INVALID, "text token without bytes rejects");

    st = inst->render_cstr(inst, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "render_cstr returns ansi output");
    fails += expect(strstr(out, "Title") != NULL && strstr(out, "Body.") != NULL,
                    "render_cstr ansi output contains content");
    inst->string_free(inst, out);
    out = NULL;

    inst->destroy(inst);
    inst = NULL;

    {
        static const int initial_widths[] = {80, 3, 80, 3};
        static const int target_widths[] = {3, 80, 3, 80};
        static const int boring_values[] = {1, 1, 0, 0};
        static const char pending_code[] = "`abcdefghij`";
        static const char suffix[] = " text\n";
        static const char complete[] = "`abcdefghij` text\n";
        size_t case_index;

        for (case_index = 0; case_index < sizeof(initial_widths) / sizeof(initial_widths[0]); case_index++) {
            emission_log writes;
            emission_log traces;
            mdf_sink bound_sink;
            mdf *reference;
            char *expected;

            memset(&writes, 0, sizeof(writes));
            memset(&traces, 0, sizeof(traces));
            mdf_options_init(&opts);
            opts.boring = boring_values[case_index];
            opts.width = initial_widths[case_index];
            opts.write_trace.userdata = &traces;
            opts.write_trace.emit = emission_log_trace;
            bound_sink.userdata = &writes;
            bound_sink.write = emission_log_write;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL,
                            "pending inline-code width-change receiver creates");
            if (inst != NULL) {
                st = inst->set_sink(inst, &bound_sink);
                fails += expect(st == MDF_OK, "pending inline-code width-change receiver binds its sink");
                st = inst->feed(inst, pending_code, strlen(pending_code));
                fails += expect(st == MDF_OK && writes.count == 0 && traces.count == 0,
                                "complete inline code remains undecided before its following boundary");
                st = inst->set_width(inst, target_widths[case_index]);
                fails += expect(st == MDF_OK,
                                "pending inline code accepts a runtime width change before emission");
                st = inst->feed(inst, suffix, strlen(suffix));
                if (st == MDF_OK) {
                    st = inst->finish_document(inst);
                }
                fails += expect(st == MDF_OK,
                                "pending inline code completes after a runtime width change");
                fails += expect(emission_logs_equal(&writes, &traces),
                                "pending inline-code sink writes and trace events stay byte-for-byte identical");

                reference = NULL;
                expected = NULL;
                mdf_options_init(&opts);
                opts.boring = boring_values[case_index];
                opts.width = target_widths[case_index];
                st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
                if (st == MDF_OK) {
                    st = reference->render_cstr(reference, complete, &expected);
                }
                fails += expect(st == MDF_OK && expected != NULL &&
                                emission_log_equals_bytes(&writes, expected, strlen(expected)),
                                "pending inline code is re-decided using the width in effect at emission");
                if (expected != NULL) {
                    reference->string_free(reference, expected);
                }
                if (reference != NULL) {
                    reference->destroy(reference);
                }
                inst->destroy(inst);
                inst = NULL;
            }
            emission_log_free(&writes);
            emission_log_free(&traces);
        }
    }

    {
        static const char prefix[] = "a ";
        static const char pending_code[] = "`a.b.c.d.e.f`";
        static const char suffix[] = " text\n";
        static const char complete[] = "a `a.b.c.d.e.f` text\n";
        static const char expected_narrow[] = "a\na.b.\xE2\x80\xA6\ntext\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "separator reflow width-change receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, prefix, strlen(prefix));
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_code, strlen(pending_code));
            }
            fails += expect(st == MDF_OK && emission_log_equals_bytes(&writes, "a", 1) &&
                            emission_logs_equal(&writes, &traces),
                            "only the pre-code word is emitted before narrow separator reflow");
            if (st == MDF_OK) {
                st = inst->set_width(inst, 5);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK && emission_logs_equal(&writes, &traces),
                            "separator reflow keeps every sink write and trace event identical");
            fails += expect(emission_log_equals_bytes(&writes, expected_narrow, strlen(expected_narrow)),
                            "separator reflow recomputes narrow code placement rather than retaining the wide separator");

            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 5;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "separator reflow matches a fresh narrow renderer");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char markdown[] = "`abcdefghij` text\n";
        width_change_output_sink output;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;

        memset(&output, 0, sizeof(output));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &output;
        bound_sink.write = width_change_output_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "output-callback width-change receiver creates");
        if (inst != NULL) {
            output.renderer = inst;
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, markdown, strlen(markdown));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK && output.calls > 0 &&
                            output.width_status == MDF_ERROR_INVALID,
                            "output callbacks cannot change width during an emission");
            fails += expect(emission_logs_equal(&output.capture, &traces),
                            "rejected output-callback width changes preserve exact sink and trace bytes");

            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 80;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, markdown, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_log_equals_bytes(&output.capture, expected, strlen(expected)),
                            "rejected output-callback width changes leave the active decision unchanged");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&output.capture);
        emission_log_free(&traces);
    }

    {
        static const char pending_code[] = "hello `abcdefghij` ";
        static const char suffix[] = "next\n";
        static const char complete[] = "hello `abcdefghij` next\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "repeated pending-code reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_code, strlen(pending_code));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 5);
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 80);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "repeated pending-code reflows do not emit before a following boundary");
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 80;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "the final pending-code layout uses the last width before emission");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char prefix[] = "a `b`";
        static const char suffix[] = " tail\n";
        static const char complete[] = "a `b` tail\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "committed-separator reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, prefix, strlen(prefix));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 10);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "changing width reflows pending code without writing or replaying committed separators");
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 10;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "committed code separators are not replayed after runtime width changes");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_code[] = "`abcdefghij`";
        emission_log writes;
        mdf_sink bound_sink;

        memset(&writes, 0, sizeof(writes));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 3;
        opts.emission_buffer.initial_cap = 4;
        opts.emission_buffer.max_cap = 4;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "failed pending-code reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_code, strlen(pending_code));
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 80);
            }
            fails += expect(st == MDF_ERROR_NOMEM,
                            "oversized pending-code reflow reports allocation failure");
            if (st == MDF_ERROR_NOMEM) {
                st = inst->set_width(inst, 3);
            }
            fails += expect(st == MDF_ERROR_INVALID,
                            "failed pending-code reflow rejects later width changes until reset");
            if (st == MDF_ERROR_INVALID) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_ERROR_INVALID,
                            "failed pending-code reflow rejects finishing a document with lost input");
            if (st == MDF_ERROR_INVALID) {
                st = inst->reset(inst);
            }
            fails += expect(st == MDF_OK,
                            "reset recovers a renderer after failed pending-code reflow");
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
    }

    {
        static const char pending_code[] = "`abcdefghij`";
        mdf_sink bound_sink;
        mdf_token token;

        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 3;
        opts.emission_buffer.initial_cap = 4;
        opts.emission_buffer.max_cap = 4;
        bound_sink.userdata = NULL;
        bound_sink.write = discard_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "manual failed-reflow renderer creates");
        if (inst != NULL) {
            memset(&token, 0, sizeof(token));
            token.type = MDF_TOKEN_TEXT;
            token.text = pending_code;
            token.len = strlen(token.text);
            st = mdf_write_token(inst, &token, &bound_sink);
            if (st == MDF_OK) {
                st = mdf_set_width(inst, 80);
            }
            fails += expect(st == MDF_ERROR_NOMEM,
                            "manual pending-code reflow reports allocation failure");
            token.text = " tail";
            token.len = strlen(token.text);
            if (st == MDF_ERROR_NOMEM) {
                st = mdf_write_token(inst, &token, &bound_sink);
            }
            fails += expect(st == MDF_ERROR_INVALID,
                            "manual tokens reject a renderer with lost reflow input");
            token.type = MDF_TOKEN_DOCUMENT_END;
            token.text = NULL;
            token.len = 0;
            if (st == MDF_ERROR_INVALID) {
                st = mdf_write_token(inst, &token, &bound_sink);
            }
            fails += expect(st == MDF_ERROR_INVALID,
                            "manual document end rejects a renderer with lost reflow input");
            if (st == MDF_ERROR_INVALID) {
                st = mdf_finish(inst, &bound_sink);
            }
            fails += expect(st == MDF_ERROR_INVALID,
                            "manual finish rejects a renderer with lost reflow input");
            if (st == MDF_ERROR_INVALID) {
                st = mdf_reset(inst);
            }
            fails += expect(st == MDF_OK,
                            "manual reset recovers a renderer after failed reflow");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    {
        reflow_failure_source failure_source;
        mdf_sink bound_sink;

        memset(&failure_source, 0, sizeof(failure_source));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 3;
        opts.emission_buffer.initial_cap = 4;
        opts.emission_buffer.max_cap = 4;
        bound_sink.userdata = NULL;
        bound_sink.write = discard_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "source failed-reflow renderer creates");
        if (inst != NULL) {
            failure_source.renderer = inst;
            src.userdata = &failure_source;
            src.read = reflow_failure_source_read;
            st = mdf_render(inst, &src, &bound_sink);
            fails += expect(st == MDF_ERROR_INVALID && failure_source.width_status == MDF_ERROR_NOMEM,
                            "source render stops after a callback loses pending reflow input");
            st = mdf_reset(inst);
            fails += expect(st == MDF_OK,
                            "source reset recovers a renderer after failed reflow");
            inst->destroy(inst);
            inst = NULL;
        }
    }

    {
        static const char pending_link[] = "[x](#abc)";
        static const char expected[] = "x\n(#abc).\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 3;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "fragment pending-link reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 3);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, ".\n", 2);
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "fragment-link reflow preserves the unwrapped fallback decision");
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "# <http://example.com> ";
        static const char suffix[] = "tail\n";
        static const char complete[] = "# <http://example.com> tail\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "heading pending-link reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 3);
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 80);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "reflowing a pending heading link does not emit before its boundary");
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.width = 80;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "pending-link reflow retains heading styling for following text");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "<https://example.com/abcdefghij>";
        static const char complete[] = "<https://example.com/abcdefghij>\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "pending autolink reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 10);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "changing width reflows pending autolinks without emitting them");
            if (st == MDF_OK) {
                st = inst->feed(inst, "\n", 1);
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 10;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "pending autolinks are re-decided using the width in effect at emission");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char prefix[] = "hello <https://example.com>";
        static const char expected[] = "hello \nhttps://example.com\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        writes.max_events = 64;
        writes.max_bytes = 1024;
        traces.max_events = 64;
        traces.max_bytes = 1024;
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "committed-prefix autolink reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, prefix, strlen(prefix));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 22);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "changing width reflows a committed-prefix autolink without emitting it");
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "pending autolinks wrap at the new width without replaying committed separators");
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "[x](https://example.com/abcdefghij)";
        static const char expected_reflow[] =
            "x \n(https://e\nxample.com\n/abcdefghi\nj)\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        size_t write_count;
        size_t trace_count;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "pending fallback-link reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            write_count = writes.count;
            trace_count = traces.count;
            if (st == MDF_OK) {
                st = inst->set_width(inst, 10);
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 10);
            }
            fails += expect(st == MDF_OK && writes.count == write_count && traces.count == trace_count,
                            "repeated width changes reflow pending fallback links without emitting them");
            if (st == MDF_OK) {
                st = inst->feed(inst, "\n", 1);
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected_reflow,
                                                      strlen(expected_reflow)),
                            "pending fallback links reflow their retained URL without replaying committed output");
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "([foo](https://example.com)";
        static const char suffix[] = ").\n";
        static const char complete[] = "([foo](https://example.com)).\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "parenthesized pending fallback-link reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 80);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 80;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "fallback-link reflow retains only its own closing punctuation");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "<https://example.com/abcdefghij>";
        static const char suffix[] = " tail\n";
        static const char complete[] = "<https://example.com/abcdefghij> tail\n";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.width = 80;
        opts.osc8 = 0;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "styled pending autolink reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 80);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, suffix, strlen(suffix));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.width = 80;
            opts.osc8 = 0;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &reference);
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, complete, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_logs_equal(&writes, &traces) &&
                            emission_log_equals_bytes(&writes, expected, strlen(expected)),
                            "autolink reflow restores the surrounding style before following text");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char pending_link[] = "[x](https://example.com/abcdefghij)";
        emission_log writes;
        emission_log traces;
        mdf_sink bound_sink;

        memset(&writes, 0, sizeof(writes));
        memset(&traces, 0, sizeof(traces));
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.osc8 = 0;
        opts.width = 80;
        opts.emission_buffer.initial_cap = 40;
        opts.emission_buffer.max_cap = 40;
        opts.write_trace.userdata = &traces;
        opts.write_trace.emit = emission_log_trace;
        bound_sink.userdata = &writes;
        bound_sink.write = emission_log_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "bounded pending fallback-link reflow receiver creates");
        if (inst != NULL) {
            st = inst->set_sink(inst, &bound_sink);
            if (st == MDF_OK) {
                st = inst->feed(inst, pending_link, strlen(pending_link));
            }
            if (st == MDF_OK) {
                st = inst->set_width(inst, 3);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, ".\n", 2);
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK && emission_logs_equal(&writes, &traces),
                            "reflowed fallback links preserve bounded emission writes and trace parity");
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&writes);
        emission_log_free(&traces);
    }

    {
        static const char markdown[] = "HTML callback width guard\n";
        width_change_output_sink output;
        mdf_sink bound_sink;
        mdf *reference;
        char *expected;

        memset(&output, 0, sizeof(output));
        bound_sink.userdata = &output;
        bound_sink.write = width_change_output_write;
        mdf_options_init(&opts);
        opts.width = 80;
        st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "HTML output-callback width-change receiver creates");
        if (inst != NULL) {
            output.renderer = inst;
            st = mdf_set_html_title(inst, "Callback guard");
            if (st == MDF_OK) {
                st = inst->set_sink(inst, &bound_sink);
            }
            if (st == MDF_OK) {
                st = inst->feed(inst, markdown, strlen(markdown));
            }
            if (st == MDF_OK) {
                st = inst->finish_document(inst);
            }
            fails += expect(st == MDF_OK && output.calls > 0 &&
                            output.width_status == MDF_ERROR_INVALID,
                            "HTML output callbacks cannot change width during a write");

            reference = NULL;
            expected = NULL;
            mdf_options_init(&opts);
            opts.width = 80;
            st = mdf_create(MDF_FORMAT_HTML, &opts, &reference);
            if (st == MDF_OK) {
                st = mdf_set_html_title(reference, "Callback guard");
            }
            if (st == MDF_OK) {
                st = reference->render_cstr(reference, markdown, &expected);
            }
            fails += expect(st == MDF_OK && expected != NULL &&
                            emission_log_equals_bytes(&output.capture, expected, strlen(expected)),
                            "rejected HTML output-callback width changes preserve HTML bytes");
            if (expected != NULL) {
                reference->string_free(reference, expected);
            }
            if (reference != NULL) {
                reference->destroy(reference);
            }
            inst->destroy(inst);
            inst = NULL;
        }
        emission_log_free(&output.capture);
    }

    {
        grow_sink bound_capture;
        grow_sink borrowed_capture;
        grow_sink replacement_capture;
        grow_sink recovery_capture;
        armed_failing_sink failed_old_sink;
        mdf_sink bound_sink;
        mdf_sink borrowed_sink;
        mdf_sink replacement_sink;
        mdf_sink failed_old_output;
        mdf_sink recovery_sink;
        width_change_source width_source;
        render_control_source control_source;
        cstr_source receiver_source;
        size_t bound_len;

        memset(&bound_capture, 0, sizeof(bound_capture));
        memset(&borrowed_capture, 0, sizeof(borrowed_capture));
        memset(&replacement_capture, 0, sizeof(replacement_capture));
        memset(&recovery_capture, 0, sizeof(recovery_capture));
        memset(&failed_old_sink, 0, sizeof(failed_old_sink));
        bound_sink.userdata = &bound_capture;
        bound_sink.write = grow_write;
        borrowed_sink.userdata = &borrowed_capture;
        borrowed_sink.write = grow_write;
        replacement_sink.userdata = &replacement_capture;
        replacement_sink.write = grow_write;
        failed_old_output.userdata = &failed_old_sink;
        failed_old_output.write = armed_fail_write;
        recovery_sink.userdata = &recovery_capture;
        recovery_sink.write = grow_write;
        mdf_options_init(&opts);
        opts.boring = 1;
        opts.width = 80;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "bound-sink receiver renderer creates");
        if (inst != NULL) {
            fails += expect(inst->set_sink != NULL && inst->set_width != NULL &&
                            inst->reset != NULL && inst->render != NULL &&
                            inst->feed != NULL && inst->flush != NULL &&
                            inst->finish_document != NULL,
                            "bound-sink receiver methods are populated");
            st = inst->reset(inst);
            fails += expect(st == MDF_OK,
                            "receiver reset succeeds before any sink is bound");
            memset(&receiver_source, 0, sizeof(receiver_source));
            receiver_source.src = "missing sink\n";
            receiver_source.len = strlen(receiver_source.src);
            src.userdata = &receiver_source;
            src.read = cstr_read;
            st = inst->render(inst, &src);
            fails += expect(st == MDF_ERROR_INVALID,
                            "receiver render requires a previously bound sink");
            st = inst->set_sink(inst, NULL);
            fails += expect(st == MDF_ERROR_INVALID,
                            "receiver set_sink rejects a null sink");
            st = inst->set_sink(inst, &bound_sink);
            fails += expect(st == MDF_OK, "receiver set_sink binds one persistent sink");

            memset(&width_source, 0, sizeof(width_source));
            width_source.renderer = inst;
            src.userdata = &width_source;
            src.read = width_change_source_read;
            st = inst->render(inst, &src);
            fails += expect(st == MDF_OK && width_source.width_status == MDF_OK &&
                            bound_capture.buf != NULL &&
                            strcmp(bound_capture.buf, "alpha\nbeta\n") == 0,
                            "receiver source callback changes width before later layout decisions");
            st = inst->set_width(inst, 80);
            fails += expect(st == MDF_OK, "receiver restores width after a source-time change");
            st = inst->set_width(inst, 0);
            fails += expect(st == MDF_ERROR_INVALID,
                            "receiver set_width rejects an invalid width");

            grow_free(&bound_capture);
            memset(&control_source, 0, sizeof(control_source));
            control_source.renderer = inst;
            control_source.replacement = &replacement_sink;
            src.userdata = &control_source;
            src.read = render_control_source_read;
            st = inst->render(inst, &src);
            fails += expect(st == MDF_OK &&
                            control_source.reset_status == MDF_ERROR_INVALID &&
                            control_source.sink_status == MDF_ERROR_INVALID &&
                            bound_capture.buf != NULL &&
                            strstr(bound_capture.buf, "alpha beta") != NULL &&
                            replacement_capture.len == 0,
                            "synchronous render rejects reset and sink replacement from its source callback");

            memset(&receiver_source, 0, sizeof(receiver_source));
            receiver_source.src = "borrowed free sink\n";
            receiver_source.len = strlen(receiver_source.src);
            src.userdata = &receiver_source;
            src.read = cstr_read;
            bound_len = bound_capture.len;
            st = mdf_render(inst, &src, &borrowed_sink);
            fails += expect(st == MDF_OK && borrowed_capture.buf != NULL &&
                            strstr(borrowed_capture.buf, "borrowed free sink") != NULL &&
                            bound_capture.len == bound_len,
                            "explicit render sink is borrowed and does not replace the receiver sink");

            bound_len = bound_capture.len;
            st = mdf_feed(inst, "borrowed incremental sink\n",
                          strlen("borrowed incremental sink\n"), &borrowed_sink);
            if (st == MDF_OK) st = mdf_flush(inst, &borrowed_sink);
            if (st == MDF_OK) st = mdf_finish_document(inst, &borrowed_sink);
            fails += expect(st == MDF_OK && borrowed_capture.buf != NULL &&
                            strstr(borrowed_capture.buf, "borrowed incremental sink") != NULL &&
                            bound_capture.len == bound_len,
                            "explicit incremental sinks are borrowed and do not replace the receiver sink");

            memset(&receiver_source, 0, sizeof(receiver_source));
            receiver_source.src = "bound receiver sink\n";
            receiver_source.len = strlen(receiver_source.src);
            src.userdata = &receiver_source;
            src.read = cstr_read;
            st = inst->begin_document(inst);
            if (st == MDF_OK) {
                st = inst->feed(inst, receiver_source.src, receiver_source.len);
            }
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && bound_capture.buf != NULL &&
                            strstr(bound_capture.buf, "bound receiver sink") != NULL,
                            "receiver incremental methods continue through the original bound sink");

            memset(&tok, 0, sizeof(tok));
            tok.type = MDF_TOKEN_TEXT;
            tok.text = "manual receiver sink";
            tok.len = strlen(tok.text);
            st = inst->begin_document(inst);
            if (st == MDF_OK) st = inst->write_token(inst, &tok);
            if (st == MDF_OK) {
                tok.type = MDF_TOKEN_DOCUMENT_END;
                tok.text = NULL;
                tok.len = 0;
                st = inst->write_token(inst, &tok);
            }
            if (st == MDF_OK) st = inst->finish(inst);
            fails += expect(st == MDF_OK && bound_capture.buf != NULL &&
                            strstr(bound_capture.buf, "manual receiver sink") != NULL,
                            "receiver manual-token methods use the persistent sink");

            st = inst->feed(inst, "discarded", strlen("discarded"));
            if (st == MDF_OK) st = inst->reset(inst);
            if (st == MDF_OK) st = inst->feed(inst, "fresh\n", strlen("fresh\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && bound_capture.buf != NULL &&
                            strstr(bound_capture.buf, "discarded") == NULL &&
                            strstr(bound_capture.buf, "fresh") != NULL &&
                            strstr(bound_capture.buf, "\033[0m") != NULL,
                            "receiver reset closes ANSI state and requires caller-owned replay");

            st = inst->begin_document(inst);
            if (st == MDF_OK) st = inst->feed(inst, "old sink", strlen("old sink"));
            if (st == MDF_OK) st = inst->set_sink(inst, &replacement_sink);
            if (st == MDF_OK) st = inst->feed(inst, "replacement sink\n", strlen("replacement sink\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && replacement_capture.buf != NULL &&
                            strstr(replacement_capture.buf, "replacement sink") != NULL &&
                            strstr(replacement_capture.buf, "old sink") == NULL,
                            "receiver sink replacement discards state and requires replay on the new sink");

            st = inst->set_sink(inst, &failed_old_output);
            fails += expect(st == MDF_OK,
                            "receiver binds a sink that will fail terminal cleanup");
            st = inst->feed(inst, "discarded", strlen("discarded"));
            failed_old_sink.armed = 1;
            if (st == MDF_OK) st = inst->set_sink(inst, &recovery_sink);
            fails += expect(st == MDF_OK && strcmp(inst->error(inst), "") == 0,
                            "receiver replacement survives old-sink cleanup failure and clears its error");
            if (st == MDF_OK) st = inst->feed(inst, "fresh\n", strlen("fresh\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && recovery_capture.buf != NULL &&
                            strstr(recovery_capture.buf, "fresh") != NULL &&
                            strstr(recovery_capture.buf, "discarded") == NULL,
                            "receiver replacement leaves replay ownership with the caller after cleanup failure");

            st = inst->begin_document(inst);
            if (st == MDF_OK) st = inst->feed(inst, "retained", strlen("retained"));
            if (st == MDF_OK) st = inst->set_sink(inst, &recovery_sink);
            if (st == MDF_OK) st = inst->feed(inst, " input\n", strlen(" input\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && recovery_capture.buf != NULL &&
                            strstr(recovery_capture.buf, "retained input") != NULL,
                            "identical receiver sink binding preserves active incremental state");

            failed_old_sink.armed = 0;
            st = inst->set_sink(inst, &failed_old_output);
            if (st == MDF_OK) st = inst->feed(inst, "discarded", strlen("discarded"));
            failed_old_sink.armed = 1;
            if (st == MDF_OK) st = inst->reset(inst);
            fails += expect(st == MDF_ERROR_IO,
                            "receiver reset reports a failed terminal cleanup write");
            st = inst->set_sink(inst, &recovery_sink);
            if (st == MDF_OK) st = inst->feed(inst, "fresh after reset\n", strlen("fresh after reset\n"));
            if (st == MDF_OK) st = inst->finish_document(inst);
            fails += expect(st == MDF_OK && recovery_capture.buf != NULL &&
                            strstr(recovery_capture.buf, "fresh after reset") != NULL &&
                            strstr(recovery_capture.buf, "discarded") == NULL,
                            "failed receiver reset discards state and permits caller replay");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&bound_capture);
        grow_free(&borrowed_capture);
        grow_free(&replacement_capture);
        grow_free(&recovery_capture);
        grow_free(&failed_old_sink.capture);
    }

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
    opts.width = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID, "ansi rejects one-column content width");

    mdf_options_init(&opts);
    opts.width = 2;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID, "ansi rejects two-column content width");

    mdf_options_init(&opts);
    opts.width = 4;
    opts.margin_left = 1;
    opts.margin_right = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
    fails += expect(st == MDF_ERROR_INVALID, "ansi rejects margins that leave two content columns");

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
            st = mdf_render(inst, &src, &sink);
            fails += expect(st == MDF_OK &&
                            sink_data.buf != NULL &&
                            strstr(sink_data.buf, "\033]8;;https://example.com\033\\") != NULL,
                            "osc8 link warms parser buffers with supplied emission buffer");
            grow_free(&sink_data);
            memset(&sink_data, 0, sizeof(sink_data));
            link_src.off = 0;
            osc8_start_allocs_before_render = allocs.allocs_26;
            st = mdf_render(inst, &src, &sink);
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

    mdf_options_init(&opts);
    opts.boring = 1;
    {
        char fixed_emit[64];
        char source[1024];
        size_t source_len;
        size_t level;

        source_len = 0;
        source[source_len++] = '[';
        for (level = 1; level <= 17; level++) {
            memset(source + source_len, '*', level);
            source_len += level;
            source[source_len++] = 'a';
            source[source_len++] = '-';
        }
        memset(source + source_len, 'x', 256);
        source_len += 256;
        for (level = 17; level > 0; level--) {
            source[source_len++] = '-';
            source[source_len++] = 'b';
            memset(source + source_len, '*', level);
            source_len += level;
        }
        memcpy(source + source_len, "](https://x)\n", sizeof("](https://x)\n"));

        opts.emission_buffer.data = fixed_emit;
        opts.emission_buffer.cap = sizeof(fixed_emit);
        opts.emission_buffer.fixed = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL,
                        "ansi create accepts fixed emission buffer for bounded link-label fallback");
        if (inst != NULL) {
            st = inst->render_cstr(inst, source, &out);
            fails += expect(st != MDF_OK && out == NULL,
                            "bounded link-label fallback does not split an oversized emission decision");
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
        owned_buffer_probe owned_probe;
        char *owned_emit;

        memset(&owned_probe, 0, sizeof(owned_probe));
        owned_emit = (char *)malloc(256);
        fails += expect(owned_emit != NULL, "allocate owned emission buffer for deck child renderer test");
        if (owned_emit != NULL) {
            mdf_options owned_opts;
            mdf *owned_inst;

            mdf_options_init(&owned_opts);
            owned_opts.allocator.userdata = &owned_probe;
            owned_opts.allocator.alloc = owned_probe_alloc;
            owned_opts.allocator.realloc = owned_probe_realloc;
            owned_opts.allocator.free = owned_probe_free;
            owned_opts.emission_buffer.data = owned_emit;
            owned_opts.emission_buffer.cap = 256;
            owned_opts.emission_buffer.take_ownership = 1;
            owned_probe.owned = owned_emit;
            owned_inst = NULL;
            st = mdf_create(MDF_FORMAT_HTML_DECK, &owned_opts, &owned_inst);
            fails += expect(st == MDF_OK && owned_inst != NULL,
                            "deck renderer accepts caller-owned emission buffer");
            if (owned_inst != NULL) {
                st = owned_inst->render_cstr(owned_inst,
                                             "# A\n"
                                             "\n"
                                             "---\n"
                                             "\n"
                                             "# B\n",
                                             &out);
                fails += expect(st == MDF_OK && out != NULL && strstr(out, "# B") != NULL,
                                "deck render succeeds with caller-owned emission buffer");
                fails += expect(owned_probe.owned_frees == 0,
                                "deck child renderers do not free caller-owned parent emission buffer");
                owned_inst->string_free(owned_inst, out);
                out = NULL;
                owned_inst->destroy(owned_inst);
                fails += expect(owned_probe.owned_frees == 1,
                                "deck parent renderer frees caller-owned emission buffer on destroy");
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
            st = mdf_render(tracked_inst, &src, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts text");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "world";
    tok.len = 5;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts second text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts list item start");
    tok.type = MDF_TOKEN_TASK_CHECKED;
    tok.text = "X";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts checked task marker");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts following space token");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "done";
    tok.len = 4;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts task text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi task session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts heading start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Head";
    tok.len = 4;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts heading text");
    tok.type = MDF_TOKEN_HEADING_END;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts heading end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi heading session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts blockquote start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "quote";
    tok.len = 5;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts quote text");
    tok.type = MDF_TOKEN_BLOCKQUOTE_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts blockquote end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi blockquote session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts code block start");
    tok.type = MDF_TOKEN_CODE_TEXT;
    tok.text = "code();";
    tok.len = 7;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts code text");
    tok.type = MDF_TOKEN_CODE_BLOCK_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts code block end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi code block session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts paragraph text");
    tok.type = MDF_TOKEN_NEWLINE;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "b";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts second line text");
    tok.type = MDF_TOKEN_PARAGRAPH_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts paragraph end");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "c";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts following paragraph text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi paragraph session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts leading text");
    tok.type = MDF_TOKEN_THEMATIC_BREAK;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts thematic break");
    tok.type = MDF_TOKEN_NEWLINE;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts following newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "after";
    tok.len = 5;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts trailing text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi thematic session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token accepts ordered list item start");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts following space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "first";
    tok.len = 5;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts item text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi ordered list session accepts document end");
    st = mdf_finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "1. first") != NULL,
                    "ansi manual ordered list token stream renders ordered marker");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token starts a second manual session on the same handle");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi second manual session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi write_token can buffer text before a finish failure");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi manual session restarts cleanly after finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "ansi restarted manual session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
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
    st = mdf_render(inst, &src, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL, "render streams through source and sink");
    fails += expect(strstr(sink_data.buf, "- item") != NULL, "render sink output matches markdown content");
    grow_free(&sink_data);

    src.userdata = NULL;
    src.read = fail_read;
    st = mdf_render(inst, &src, &sink);
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
    st = mdf_render(inst, &src, &sink);
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
    st = mdf_render(inst, &src, &sink);
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
    st = mdf_render(inst, &src, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts plain text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts list item start");
    tok.type = MDF_TOKEN_TASK_UNCHECKED;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts unchecked task marker");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts following space token");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "todo";
    tok.len = 4;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts task text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html task session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts heading start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Head";
    tok.len = 4;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts heading text");
    tok.type = MDF_TOKEN_HEADING_END;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts heading end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html heading session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts blockquote start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "quote";
    tok.len = 5;
    tok.level = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts quote text");
    tok.type = MDF_TOKEN_BLOCKQUOTE_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts blockquote end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html blockquote session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts code block start");
    tok.type = MDF_TOKEN_CODE_TEXT;
    tok.text = "code();";
    tok.len = 7;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts code text");
    tok.type = MDF_TOKEN_CODE_BLOCK_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts code block end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html code block session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts paragraph text");
    tok.type = MDF_TOKEN_NEWLINE;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "b";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts second line text");
    tok.type = MDF_TOKEN_PARAGRAPH_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts paragraph end");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "c";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts following paragraph text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html paragraph session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts leading text");
    tok.type = MDF_TOKEN_THEMATIC_BREAK;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts thematic break");
    tok.type = MDF_TOKEN_NEWLINE;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts following newline");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "after";
    tok.len = 5;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts trailing text");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html thematic session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token accepts ordered list item start");
    tok.type = MDF_TOKEN_SPACE;
    tok.text = " ";
    tok.len = 1;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts following space");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "first";
    tok.len = 5;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts item text");
    tok.type = MDF_TOKEN_LIST_ITEM_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts list item end");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html ordered list session accepts document end");
    st = mdf_finish(inst, &sink);
    fails += expect(st == MDF_OK && sink_data.buf != NULL &&
                    strstr(sink_data.buf, "1.") != NULL &&
                    strstr(sink_data.buf, "first") != NULL &&
                    strstr(sink_data.buf, "</html>") != NULL,
                    "html manual ordered list token stream renders ordered marker inside document shell");
    grow_free(&sink_data);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = grow_write;
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token starts a second manual session on the same handle");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html second manual session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html manual session restarts cleanly after write_token failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html restarted manual session accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html write_token can buffer text before a finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html buffered session accepts document end before finish failure");
    armed_sink.armed = 1;
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html manual session restarts cleanly after finish failure");
    tok.type = MDF_TOKEN_DOCUMENT_END;
    tok.text = NULL;
    tok.len = 0;
    st = mdf_write_token(inst, &tok, &sink);
    fails += expect(st == MDF_OK, "html restarted session after finish failure accepts document end");
    st = mdf_finish(inst, &sink);
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
    st = mdf_write_token(inst, &tok, &sink);
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
    st = mdf_finish(inst, &sink);
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
                st = mdf_render(inst, &src, &sink);
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
                st = mdf_render(inst, &src, &sink);
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
                st = mdf_render(inst, &src, &sink);
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

    {
        static const char incremental_markdown[] =
            "# Split heading\n\n"
            "plain *emphasis* [link](https://example.com?a=1&b=2)\\&amp; `code`\n\n"
            "> quote\n\n"
            "- list item\n\n"
            "```text\nfenced code\n```\n\n"
            "| left | right |\n|---|---|\n| one | two |\n\n"
            "```mdf-bar-chart\nalpha, 1\nbeta, 2\n```\n";
        const char *ordinary;
        size_t split;
        size_t incremental_len;
        grow_sink expected_sink;
        grow_sink incremental_sink;
        mdf_sink incremental_output;

        ordinary = "ordinary text pauses before eof";
        incremental_len = strlen(incremental_markdown);
        memset(&expected_sink, 0, sizeof(expected_sink));
        sink.userdata = &expected_sink;
        sink.write = grow_write;
        mdf_options_init(&opts);
        opts.boring = 1;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental reference renderer creates");
        if (inst != NULL) {
            src_data.src = incremental_markdown;
            src_data.len = incremental_len;
            src_data.off = 0;
            src.userdata = &src_data;
            src.read = cstr_read;
            st = mdf_render(inst, &src, &sink);
            fails += expect(st == MDF_OK, "incremental reference render succeeds");
            inst->destroy(inst);
            inst = NULL;
        }

        memset(&incremental_sink, 0, sizeof(incremental_sink));
        incremental_output.userdata = &incremental_sink;
        incremental_output.write = grow_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental renderer creates");
        if (inst != NULL) {
            fails += expect(inst->feed != NULL && inst->flush != NULL &&
                            inst->finish_document != NULL && inst->begin_document != NULL,
                            "incremental lifecycle receiver methods are populated");
            st = mdf_feed(inst, ordinary, strlen(ordinary), &incremental_output);
            fails += expect(st == MDF_OK && incremental_sink.buf != NULL &&
                            strstr(incremental_sink.buf, "ordinary") != NULL,
                            "incremental feed emits decidable ordinary text before eof");
            st = mdf_flush(inst, &incremental_output);
            fails += expect(st == MDF_OK, "incremental flush is a non-eof boundary");
            st = inst->begin_document(inst);
            fails += expect(st == MDF_ERROR_INVALID, "incremental begin rejects an unfinished document");
            st = mdf_finish_document(inst, &incremental_output);
            fails += expect(st == MDF_OK, "incremental finish closes the first document");
            st = mdf_feed(inst, "later", 5, &incremental_output);
            fails += expect(st == MDF_ERROR_INVALID, "incremental feed rejects post-finish input");
            st = mdf_flush(inst, &incremental_output);
            fails += expect(st == MDF_ERROR_INVALID, "incremental flush rejects post-finish calls");
            st = mdf_finish_document(inst, &incremental_output);
            fails += expect(st == MDF_ERROR_INVALID, "incremental finish is not repeatable");
            st = inst->begin_document(inst);
            fails += expect(st == MDF_OK, "incremental begin starts a distinct next document");
            st = mdf_feed(inst, "second document", strlen("second document"), &incremental_output);
            fails += expect(st == MDF_OK, "incremental next document accepts input");
            st = mdf_finish_document(inst, &incremental_output);
            fails += expect(st == MDF_OK, "incremental next document finishes");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&incremental_sink);

        memset(&incremental_sink, 0, sizeof(incremental_sink));
        incremental_output.userdata = &incremental_sink;
        incremental_output.write = grow_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental separator renderer creates");
        if (inst != NULL) {
            st = mdf_feed(inst, "Hello ", strlen("Hello "), &incremental_output);
            fails += expect(st == MDF_OK && incremental_sink.buf != NULL &&
                            strcmp(incremental_sink.buf, "Hello") == 0 &&
                            incremental_sink.writes == 1,
                            "incremental feed emits the first word at its space boundary");
            st = mdf_feed(inst, "world ", strlen("world "), &incremental_output);
            fails += expect(st == MDF_OK && incremental_sink.buf != NULL &&
                            strcmp(incremental_sink.buf, "Hello world") == 0 &&
                            incremental_sink.writes == 3,
                            "incremental feed emits each later word once its boundary is decided");
            st = mdf_finish_document(inst, &incremental_output);
            fails += expect(st == MDF_OK, "incremental separator renderer finishes");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&incremental_sink);

        for (split = 1; split < incremental_len; split++) {
            memset(&incremental_sink, 0, sizeof(incremental_sink));
            incremental_output.userdata = &incremental_sink;
            incremental_output.write = grow_write;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
            if (st == MDF_OK) {
                st = mdf_feed(inst, incremental_markdown, split, &incremental_output);
            }
            if (st == MDF_OK) {
                st = mdf_flush(inst, &incremental_output);
            }
            if (st == MDF_OK) {
                st = mdf_feed(inst, incremental_markdown + split,
                                incremental_len - split, &incremental_output);
            }
            if (st == MDF_OK) {
                st = mdf_finish_document(inst, &incremental_output);
            }
            fails += expect(st == MDF_OK && incremental_sink.buf != NULL && expected_sink.buf != NULL &&
                            strcmp(incremental_sink.buf, expected_sink.buf) == 0,
                            "incremental byte split matches one-shot render");
            if (inst != NULL) {
                inst->destroy(inst);
                inst = NULL;
            }
            grow_free(&incremental_sink);
        }
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental empty fragment renderer creates");
        if (inst != NULL) {
            st = mdf_feed(inst, "", 0, &sink);
            fails += expect(st == MDF_ERROR_INVALID, "incremental feed rejects an empty fragment");
            inst->destroy(inst);
            inst = NULL;
        }
        {
            armed_failing_sink failed_incremental_sink;

            memset(&failed_incremental_sink, 0, sizeof(failed_incremental_sink));
            failed_incremental_sink.armed = 1;
            incremental_output.userdata = &failed_incremental_sink;
            incremental_output.write = armed_fail_write;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL, "incremental failing sink renderer creates");
            if (inst != NULL) {
                st = mdf_feed(inst, "sink failure\n", strlen("sink failure\n"), &incremental_output);
                fails += expect(st == MDF_ERROR_IO && strcmp(inst->error(inst), "sink write failed") == 0,
                                "incremental sink failure enters a failed state");
                st = mdf_finish_document(inst, &incremental_output);
                fails += expect(st == MDF_ERROR_INVALID, "incremental failed state does not replay output");
                inst->destroy(inst);
                inst = NULL;
            }
            grow_free(&failed_incremental_sink.capture);
        }
        {
            char long_word[201];
            char long_markdown[201];

            memset(long_word, 'x', sizeof(long_word) - 1);
            long_word[sizeof(long_word) - 1] = '\0';
            memcpy(long_markdown, long_word, sizeof(long_word));
            memset(&fail_allocs, 0, sizeof(fail_allocs));
            fail_allocs.fail_after = (size_t)-1;
            mdf_options_init(&opts);
            opts.boring = 1;
            opts.width = 1000;
            opts.emission_buffer.initial_cap = 8;
            opts.emission_buffer.max_cap = 1024;
            opts.allocator.userdata = &fail_allocs;
            opts.allocator.alloc = failing_alloc;
            opts.allocator.realloc = failing_realloc;
            opts.allocator.free = failing_free;
            memset(&incremental_sink, 0, sizeof(incremental_sink));
            incremental_output.userdata = &incremental_sink;
            incremental_output.write = grow_write;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL,
                            "incremental flush allocation-failure renderer creates");
            if (inst != NULL) {
                st = mdf_feed(inst, long_markdown, strlen(long_markdown), &incremental_output);
                fails += expect(st == MDF_OK && incremental_sink.len == 0,
                                "incremental long word remains pending before EOF");
                fail_allocs.alloc_calls = 0;
                fail_allocs.realloc_calls = 0;
                fail_allocs.fail_after = 0;
                if (st == MDF_OK) {
                    st = mdf_flush(inst, &incremental_output);
                }
                fails += expect(st == MDF_OK,
                                "incremental flush does not attempt an unresolved emission");
                fails += expect(incremental_sink.len == 0,
                                "incremental flush does not invoke the sink");
                fail_allocs.fail_after = (size_t)-1;
                if (st == MDF_OK) {
                    st = mdf_finish_document(inst, &incremental_output);
                }
                fails += expect(st == MDF_OK && incremental_sink.len > 0,
                                "incremental EOF emits the retained long-word decision");
                inst->destroy(inst);
                inst = NULL;
            }
            fail_allocs.fail_after = (size_t)-1;
            grow_free(&incremental_sink);
        }
        memset(&armed_sink, 0, sizeof(armed_sink));
        armed_sink.armed = 1;
        incremental_output.userdata = &armed_sink;
        incremental_output.write = armed_fail_write;
        st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental html renderer creates");
        if (inst != NULL) {
            st = mdf_set_html_title(inst, "Incremental HTML");
            if (st == MDF_OK) st = mdf_flush(inst, &incremental_output);
            fails += expect(st == MDF_OK && armed_sink.capture.len == 0,
                            "initial incremental html flush does not write to the sink");
            armed_sink.armed = 0;
            if (st == MDF_OK) st = mdf_feed(inst, "# Heading\n", strlen("# Heading\n"), &incremental_output);
            if (st == MDF_OK) st = mdf_finish_document(inst, &incremental_output);
            fails += expect(st == MDF_OK && armed_sink.capture.buf != NULL &&
                            strstr(armed_sink.capture.buf, "<title>Incremental HTML</title>") != NULL &&
                            strstr(armed_sink.capture.buf, "</html>") != NULL,
                            "incremental html emits the explicit title and document closure");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&armed_sink.capture);
        st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental deck renderer creates");
        if (inst != NULL) {
            st = mdf_feed(inst, "# deck\n", strlen("# deck\n"), &sink);
            fails += expect(st == MDF_ERROR_INVALID, "incremental lifecycle rejects whole-source deck rendering");
            inst->destroy(inst);
            inst = NULL;
        }
        st = mdf_create(MDF_FORMAT_HTML, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "incremental untitled html renderer creates");
        if (inst != NULL) {
            st = mdf_feed(inst, "# Heading\n", strlen("# Heading\n"), &sink);
            fails += expect(st == MDF_ERROR_INVALID &&
                            strcmp(inst->error(inst), "incremental HTML requires an explicit title") == 0,
                            "incremental html rejects ambiguous automatic title detection");
            inst->destroy(inst);
            inst = NULL;
        }
        {
            char *oversized;
            size_t oversized_len;

            oversized_len = 65537;
            oversized = (char *)malloc(oversized_len);
            fails += expect(oversized != NULL, "incremental oversized chart test allocates");
            if (oversized != NULL) {
                memset(oversized, 'x', oversized_len);
                st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
                fails += expect(st == MDF_OK && inst != NULL, "incremental bounded chart renderer creates");
                if (inst != NULL) {
                    st = mdf_feed(inst, "```mdf-bar-chart\n", strlen("```mdf-bar-chart\n"), &sink);
                    if (st == MDF_OK) {
                        st = mdf_feed(inst, oversized, oversized_len, &sink);
                    }
                    fails += expect(st == MDF_ERROR_PARSE &&
                                    strstr(inst->error(inst), "retention limit") != NULL,
                                    "incremental oversized unfinished chart fails at the retention limit");
                    inst->destroy(inst);
                    inst = NULL;
                }
                free(oversized);
            }
        }
        {
            char *eof_sized_chart;
            size_t eof_sized_chart_len;

            eof_sized_chart_len = 65536;
            eof_sized_chart = (char *)malloc(eof_sized_chart_len);
            fails += expect(eof_sized_chart != NULL, "incremental eof-sized chart test allocates");
            if (eof_sized_chart != NULL) {
                memset(eof_sized_chart, 'x', eof_sized_chart_len);
                mdf_options_init(&opts);
                opts.boring = 1;
                st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
                fails += expect(st == MDF_OK && inst != NULL, "incremental eof-sized chart renderer creates");
                if (inst != NULL) {
                    st = mdf_feed(inst, "```mdf-bar-chart\n", strlen("```mdf-bar-chart\n"), &sink);
                    if (st == MDF_OK) {
                        st = mdf_feed(inst, eof_sized_chart, eof_sized_chart_len, &sink);
                    }
                    fails += expect(st == MDF_OK,
                                    "incremental eof-sized chart body is accepted before eof completion");
                    if (st == MDF_OK) {
                        st = mdf_finish_document(inst, &sink);
                    }
                    fails += expect(st == MDF_ERROR_PARSE &&
                                    strstr(inst->error(inst), "retention limit") != NULL,
                                    "incremental eof-sized chart reports the retention limit as a parse error");
                    inst->destroy(inst);
                    inst = NULL;
                }
                free(eof_sized_chart);
            }
        }
        {
            static const char row_mode_prefix[] = "| h |\n|---|\n";
            static const char row_mode_row[] = "| \xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97 |  \n";
            static const size_t chunk_sizes[] = {4096, 8};
            char *row_mode_table;
            size_t row_mode_table_len;
            size_t row;
            size_t case_index;
            mdf_sink row_mode_sink;

            row_mode_table_len = strlen(row_mode_prefix) + 25000 * strlen(row_mode_row);
            row_mode_table = (char *)malloc(row_mode_table_len);
            fails += expect(row_mode_table != NULL, "incremental row-mode utf8 table test allocates");
            if (row_mode_table != NULL) {
                memcpy(row_mode_table, row_mode_prefix, strlen(row_mode_prefix));
                for (row = 0; row < 25000; row++) {
                    memcpy(row_mode_table + strlen(row_mode_prefix) + row * strlen(row_mode_row),
                           row_mode_row, strlen(row_mode_row));
                }
                row_mode_sink.userdata = NULL;
                row_mode_sink.write = discard_write;
                for (case_index = 0; case_index < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); case_index++) {
                    size_t off;

                    mdf_options_init(&opts);
                    opts.boring = 1;
                    opts.table_buffer_mode = MDF_TABLE_BUFFER_ROW;
                    st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
                    fails += expect(st == MDF_OK && inst != NULL,
                                    "incremental row-mode utf8 table renderer creates");
                    if (inst == NULL) {
                        continue;
                    }
                    off = 0;
                    while (st == MDF_OK && off < row_mode_table_len) {
                        size_t n;

                        n = row_mode_table_len - off;
                        if (n > chunk_sizes[case_index]) {
                            n = chunk_sizes[case_index];
                        }
                        st = mdf_feed(inst, row_mode_table + off, n, &row_mode_sink);
                        off += n;
                    }
                    if (st == MDF_OK) {
                        st = mdf_finish_document(inst, &row_mode_sink);
                    }
                    fails += expect(st == MDF_OK,
                                    "incremental row-mode utf8 table retention is independent of fragment size");
                    inst->destroy(inst);
                    inst = NULL;
                }
                free(row_mode_table);
            }
        }
        {
            size_t row;

            mdf_options_init(&opts);
            opts.boring = 1;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
            fails += expect(st == MDF_OK && inst != NULL, "incremental bounded table renderer creates");
            if (inst != NULL) {
                st = mdf_feed(inst, "| left | right |\n|---|---|\n",
                                strlen("| left | right |\n|---|---|\n"), &sink);
                for (row = 0; st == MDF_OK && row <= 1024; row++) {
                    st = mdf_feed(inst, "| one | two |\n", strlen("| one | two |\n"), &sink);
                }
                fails += expect(st == MDF_ERROR_PARSE &&
                                strstr(inst->error(inst), "retention limit") != NULL,
                                "incremental oversized unfinished table fails at the retention limit");
                inst->destroy(inst);
                inst = NULL;
            }
        }
        {
            static const char frontmatter_table_prefix[] = "---\na: b\n| ";
            static const char frontmatter_table_suffix[] = " |\n";
            char *frontmatter_table;
            size_t frontmatter_table_len;

            frontmatter_table_len = strlen(frontmatter_table_prefix) + 35000 + strlen(frontmatter_table_suffix);
            frontmatter_table = (char *)malloc(frontmatter_table_len);
            fails += expect(frontmatter_table != NULL, "incremental EOF frontmatter table test allocates");
            if (frontmatter_table != NULL) {
                memcpy(frontmatter_table, frontmatter_table_prefix, strlen(frontmatter_table_prefix));
                memset(frontmatter_table + strlen(frontmatter_table_prefix), 'x', 35000);
                memcpy(frontmatter_table + strlen(frontmatter_table_prefix) + 35000,
                       frontmatter_table_suffix, strlen(frontmatter_table_suffix));
                mdf_options_init(&opts);
                opts.boring = 1;
                st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
                fails += expect(st == MDF_OK && inst != NULL,
                                "incremental EOF frontmatter table renderer creates");
                if (inst != NULL) {
                    st = mdf_feed(inst, frontmatter_table, frontmatter_table_len, &sink);
                    fails += expect(st == MDF_OK,
                                    "incremental EOF frontmatter table is accepted before replay");
                    if (st == MDF_OK) {
                        st = mdf_finish_document(inst, &sink);
                    }
                    fails += expect(st == MDF_ERROR_PARSE &&
                                    strstr(inst->error(inst), "retention limit") != NULL,
                                    "incremental EOF frontmatter table reports retention as a parse error");
                    inst->destroy(inst);
                    inst = NULL;
                }
                free(frontmatter_table);
            }
        }
        {
            static const char allocation_markdown[] =
                "# Allocation\n\n"
                "| left | right |\n|---|---|\n| one | two |\n\n"
                "```mdf-bar-chart\nalpha, 1\nbeta, 2\n```\n";
            size_t fail_step;
            int saw_nomem;

            saw_nomem = 0;
            for (fail_step = 1; fail_step <= 32; fail_step++) {
                size_t allocation_start;

                memset(&fail_allocs, 0, sizeof(fail_allocs));
                fail_allocs.fail_after = (size_t)-1;
                mdf_options_init(&opts);
                opts.boring = 1;
                opts.allocator.userdata = &fail_allocs;
                opts.allocator.alloc = failing_alloc;
                opts.allocator.realloc = failing_realloc;
                opts.allocator.free = failing_free;
                st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
                fails += expect(st == MDF_OK && inst != NULL,
                                "incremental allocator-failure renderer creates before injection");
                if (inst == NULL) {
                    continue;
                }
                allocation_start = fail_allocs.alloc_calls + fail_allocs.realloc_calls;
                fail_allocs.fail_after = allocation_start + fail_step;
                st = mdf_feed(inst, allocation_markdown, strlen(allocation_markdown), &sink);
                if (st == MDF_ERROR_NOMEM) {
                    saw_nomem = 1;
                    fails += expect(strstr(inst->error(inst), "out of memory") != NULL,
                                    "incremental allocator failure preserves an actionable error");
                    fails += expect(mdf_finish_document(inst, &sink) == MDF_ERROR_INVALID,
                                    "incremental allocator failure enters a non-replayable failed state");
                } else {
                    fail_allocs.fail_after = (size_t)-1;
                    if (st == MDF_OK) {
                        st = mdf_finish_document(inst, &sink);
                    }
                    fails += expect(st == MDF_OK,
                                    "incremental allocator injection either fails cleanly or completes cleanly");
                }
                inst->destroy(inst);
                inst = NULL;
            }
            fails += expect(saw_nomem, "incremental allocator failure coverage reaches parser allocation paths");
        }
        grow_free(&expected_sink);
    }

    {
        static const char source_stream_markdown[] = "plain tail";
        chunked_cstr_source stream_src;
        source_offset_probe_sink stream_sink;

        mdf_options_init(&opts);
        opts.boring = 1;
        memset(&stream_src, 0, sizeof(stream_src));
        stream_src.src = source_stream_markdown;
        stream_src.len = strlen(source_stream_markdown);
        stream_src.max_chunk = strlen("plain ");
        memset(&stream_sink, 0, sizeof(stream_sink));
        stream_sink.source = &stream_src;
        stream_sink.needle = "plain";
        src.userdata = &stream_src;
        src.read = chunked_cstr_read;
        sink.userdata = &stream_sink;
        sink.write = source_offset_probe_write;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &inst);
        fails += expect(st == MDF_OK && inst != NULL, "blocking incremental source renderer creates");
        if (inst != NULL) {
            st = mdf_render(inst, &src, &sink);
            fails += expect(st == MDF_OK && stream_sink.saw_needle &&
                            stream_sink.needle_source_off <= strlen("plain "),
                            "blocking source emits a word closed by input before its next read");
            fails += expect(stream_sink.capture.buf != NULL &&
                            strstr(stream_sink.capture.buf, "tail") != NULL,
                            "blocking source emits a final plain-text tail at eof");
            inst->destroy(inst);
            inst = NULL;
        }
        grow_free(&stream_sink.capture);
    }

    fails += expect(allocs.allocs > 0 && allocs.frees > 0, "custom allocator observed allocations");
    fails += expect(allocs.allocs == allocs.frees, "custom allocator balanced after destroy");
    return fails == 0 ? 0 : 1;
}
