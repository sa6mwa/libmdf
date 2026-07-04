#include <libmdf/mdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal contract coverage: this suite intentionally exercises parser hooks
 * that are not part of the public header. */
#include "../src/mdf_internal.h"

typedef struct counting_allocator {
    size_t allocs;
    size_t frees;
    size_t fail_at_alloc;
} counting_allocator;

static void *test_alloc(void *userdata, size_t size)
{
    counting_allocator *c;

    c = (counting_allocator *)userdata;
    if (c->fail_at_alloc != 0 && c->allocs + 1 == c->fail_at_alloc) {
        return NULL;
    }
    c->allocs++;
    return malloc(size);
}

static void *test_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    counting_allocator *c;

    (void)old_size;
    c = (counting_allocator *)userdata;
    if (ptr == NULL) {
        if (c->fail_at_alloc != 0 && c->allocs + 1 == c->fail_at_alloc) {
            return NULL;
        }
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

typedef struct mem_sink {
    char buf[256];
    size_t len;
} mem_sink;

typedef struct chunk_source {
    const char *src;
    size_t len;
    size_t off;
    size_t chunk;
} chunk_source;

typedef struct grow_sink {
    char *buf;
    size_t len;
    size_t cap;
} grow_sink;

typedef struct token_capture {
    mdf_renderer base;
    grow_sink sink;
    size_t tokens;
} token_capture;

typedef struct stream_probe_sink {
    const char *needle;
    size_t needle_len;
    size_t input_index;
    size_t first_needle_input_index;
    size_t writes;
} stream_probe_sink;

static const unsigned char test_ttf_font[] = {4, 5, 6, 7};

static size_t test_ttf_read(void *userdata, size_t offset, unsigned char *dst, size_t cap, int *err)
{
    size_t remain;
    size_t n;

    (void)userdata;
    *err = 0;
    if (offset >= sizeof(test_ttf_font)) {
        return 0;
    }
    remain = sizeof(test_ttf_font) - offset;
    n = remain < cap ? remain : cap;
    if (n > 2) {
        n = 2;
    }
    memcpy(dst, test_ttf_font + offset, n);
    return n;
}

typedef struct parser_stream_probe_sink {
    chunk_source *source;
    const char *needle;
    size_t needle_len;
    size_t first_needle_source_off;
    size_t writes;
} parser_stream_probe_sink;

typedef struct chunk_record {
    size_t off;
    size_t len;
} chunk_record;

typedef struct chunk_log {
    char bytes[8192];
    size_t len;
    chunk_record chunks[256];
    size_t count;
    int overflow;
} chunk_log;

typedef struct emission_probe {
    chunk_log sink;
    chunk_log trace;
} emission_probe;

static int mem_write(void *userdata, const char *src, size_t len)
{
    mem_sink *m;

    m = (mem_sink *)userdata;
    if (m->len + len >= sizeof(m->buf)) {
        return -1;
    }
    memcpy(m->buf + m->len, src, len);
    m->len += len;
    m->buf[m->len] = '\0';
    return 0;
}

static size_t chunk_read(void *userdata, char *dst, size_t cap, int *err)
{
    chunk_source *src;
    size_t remain;
    size_t n;

    src = (chunk_source *)userdata;
    (void)err;
    if (src->off >= src->len) {
        return 0;
    }
    remain = src->len - src->off;
    n = remain;
    if (src->chunk > 0 && n > src->chunk) {
        n = src->chunk;
    }
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, src->src + src->off, n);
    src->off += n;
    return n;
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

static int bytes_contain(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len)
{
    size_t i;

    if (needle_len == 0) {
        return 1;
    }
    if (haystack_len < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int stream_probe_write(void *userdata, const char *src, size_t len)
{
    stream_probe_sink *probe;

    probe = (stream_probe_sink *)userdata;
    probe->writes++;
    if (probe->first_needle_input_index == 0 &&
        bytes_contain(src, len, probe->needle, probe->needle_len)) {
        probe->first_needle_input_index = probe->input_index;
    }
    return 0;
}

static int parser_stream_probe_write(void *userdata, const char *src, size_t len)
{
    parser_stream_probe_sink *probe;

    probe = (parser_stream_probe_sink *)userdata;
    probe->writes++;
    if (probe->first_needle_source_off == 0 &&
        bytes_contain(src, len, probe->needle, probe->needle_len)) {
        probe->first_needle_source_off = probe->source->off;
    }
    return 0;
}

static int chunk_log_append(chunk_log *log, const char *src, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (log->count >= sizeof(log->chunks) / sizeof(log->chunks[0]) ||
        log->len + len > sizeof(log->bytes)) {
        log->overflow = 1;
        return -1;
    }
    log->chunks[log->count].off = log->len;
    log->chunks[log->count].len = len;
    memcpy(log->bytes + log->len, src, len);
    log->len += len;
    log->count++;
    return 0;
}

static int emission_probe_sink_write(void *userdata, const char *src, size_t len)
{
    emission_probe *probe;

    probe = (emission_probe *)userdata;
    return chunk_log_append(&probe->sink, src, len);
}

static int emission_probe_trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    emission_probe *probe;

    (void)format;
    probe = (emission_probe *)userdata;
    return chunk_log_append(&probe->trace, src, len);
}

static int chunk_logs_equal(const chunk_log *a, const chunk_log *b)
{
    size_t i;

    if (a->overflow || b->overflow || a->count != b->count || a->len != b->len) {
        return 0;
    }
    for (i = 0; i < a->count; i++) {
        if (a->chunks[i].len != b->chunks[i].len) {
            return 0;
        }
        if (memcmp(a->bytes + a->chunks[i].off,
                   b->bytes + b->chunks[i].off,
                   a->chunks[i].len) != 0) {
            return 0;
        }
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static void grow_sink_free(grow_sink *sink)
{
    free(sink->buf);
    sink->buf = NULL;
    sink->len = 0;
    sink->cap = 0;
}

static mdf_status token_capture_write_token(mdf_renderer *self, const mdf_token *token, mdf_sink *sink)
{
    token_capture *capture;
    char line[64];
    int n;

    (void)sink;
    capture = (token_capture *)self;
    n = sprintf(line, "%d:%lu\n", (int)token->type, (unsigned long)token->len);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return MDF_ERROR_IO;
    }
    if (grow_write(&capture->sink, line, (size_t)n) != 0) {
        return MDF_ERROR_IO;
    }
    capture->tokens++;
    return MDF_OK;
}

static mdf_status token_capture_finish(mdf_renderer *self, mdf_sink *sink)
{
    (void)self;
    (void)sink;
    return MDF_OK;
}

static char *read_text_file(const char *primary, const char *fallback, size_t *out_len)
{
    FILE *fp;
    long size;
    char *buf;
    size_t n;
    const char *path;

    path = primary;
    fp = fopen(path, "rb");
    if (fp == NULL && fallback != NULL) {
        path = fallback;
        fp = fopen(path, "rb");
    }
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (n != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

static int margin_output_lines_are_shaped(const char *out, int left_margin)
{
    const char *line;

    line = out;
    while (*line != '\0') {
        const char *end;
        size_t len;
        int i;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        len = (size_t)(end - line);
        if (len > 0) {
            if (len < (size_t)left_margin) {
                return 0;
            }
            for (i = 0; i < left_margin; i++) {
                if (line[i] != ' ') {
                    return 0;
                }
            }
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    return 1;
}

static int run_margin_corpus_case(const char *path, int width, int left_margin, int right_margin)
{
    mdf_options opts;
    mdf *renderer;
    mdf_status st;
    char *src;
    char *out;
    char fallback[512];

    sprintf(fallback, "../../%s", path);
    src = read_text_file(path, fallback, NULL);
    if (src == NULL) {
        return 0;
    }
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = width;
    opts.margin_left = left_margin;
    opts.margin_right = right_margin;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK || renderer == NULL) {
        free(src);
        return 0;
    }
    out = NULL;
    st = renderer->render_cstr(renderer, src, &out);
    free(src);
    if (st != MDF_OK || out == NULL) {
        renderer->destroy(renderer);
        return 0;
    }
    st = margin_output_lines_are_shaped(out, left_margin) ? MDF_OK : MDF_ERROR_PARSE;
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return st == MDF_OK;
}

static int run_margin_corpus_cases(void)
{
    static const char *paths[] = {
        "testdata/mdtest/TEST.md",
        "testdata/OBAF.md",
        "testdata/agents.md",
        "testdata/table-corpus/wrapping.md",
        "testdata/table-corpus/container-contexts.md"
    };
    static const struct {
        int width;
        int left;
        int right;
    } cases[] = {
        {20, 0, 0},
        {50, 2, 0},
        {80, 0, 3},
        {100, 2, 3},
        {80, 6, 4}
    };
    size_t p;
    size_t c;

    for (p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            if (!run_margin_corpus_case(paths[p], cases[c].width, cases[c].left, cases[c].right)) {
                return 0;
            }
        }
    }
    return 1;
}

static void strip_ansi_inplace(char *s)
{
    size_t i;
    size_t out;

    out = 0;
    for (i = 0; s[i] != '\0'; ) {
        if (s[i] == '\033' && s[i + 1] == '[') {
            i += 2;
            while (s[i] != '\0' && ((s[i] < '@') || (s[i] > '~'))) {
                i++;
            }
            if (s[i] != '\0') {
                i++;
            }
            continue;
        }
        if (s[i] == '\033' && s[i + 1] == ']') {
            i += 2;
            while (s[i] != '\0') {
                if (s[i] == '\007') {
                    i++;
                    break;
                }
                if (s[i] == '\033' && s[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                i++;
            }
            continue;
        }
        s[out++] = s[i++];
    }
    s[out] = '\0';
}

static int ascii_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int visible_contains_phrase(const char *haystack, const char *needle)
{
    char *flat_haystack;
    char *flat_needle;
    size_t h;
    size_t n;
    int ok;

    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    flat_haystack = (char *)malloc(strlen(haystack) + 1);
    flat_needle = (char *)malloc(strlen(needle) + 1);
    if (flat_haystack == NULL || flat_needle == NULL) {
        free(flat_haystack);
        free(flat_needle);
        return 0;
    }
    h = 0;
    for (n = 0; haystack[n] != '\0'; n++) {
        if (!ascii_space_char(haystack[n])) {
            flat_haystack[h++] = haystack[n];
        }
    }
    flat_haystack[h] = '\0';
    h = 0;
    for (n = 0; needle[n] != '\0'; n++) {
        if (!ascii_space_char(needle[n])) {
            flat_needle[h++] = needle[n];
        }
    }
    flat_needle[h] = '\0';
    ok = strstr(flat_haystack, flat_needle) != NULL;
    free(flat_haystack);
    free(flat_needle);
    return ok;
}

static size_t count_substr(const char *s, const char *needle)
{
    size_t count;
    size_t needle_len;
    const char *p;

    count = 0;
    needle_len = strlen(needle);
    p = s;
    if (needle_len == 0) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static int ansi_style_before(const char *text, const char *pos, const char **style, size_t *style_len)
{
    const char *p;

    if (text == NULL || pos == NULL || pos <= text || style == NULL || style_len == NULL) {
        return 0;
    }
    if (pos[-1] != 'm') {
        return 0;
    }
    p = pos - 1;
    while (p > text && *p != '\033' && *p != '\n') {
        p--;
    }
    if (*p != '\033' || p[1] != '[') {
        return 0;
    }
    *style = p;
    *style_len = (size_t)(pos - p);
    return *style_len > 0;
}

static int ansi_chart_label_matches_bar_style(const char *out, const char *label)
{
    const char *label_pos;
    const char *bar_pos;
    const char *label_style;
    const char *bar_style;
    size_t label_style_len;
    size_t bar_style_len;

    label_pos = strstr(out, label);
    if (label_pos == NULL) {
        return 0;
    }
    bar_pos = strstr(label_pos + strlen(label), "\342\226\210");
    if (bar_pos == NULL) {
        return 0;
    }
    if (!ansi_style_before(out, label_pos, &label_style, &label_style_len) ||
        !ansi_style_before(out, bar_pos, &bar_style, &bar_style_len)) {
        return 0;
    }
    return label_style_len == bar_style_len &&
           memcmp(label_style, bar_style, label_style_len) == 0;
}

static int ansi_chart_texts_match_style(const char *out, const char *first, const char *second)
{
    const char *first_pos;
    const char *second_pos;
    const char *first_style;
    const char *second_style;
    size_t first_style_len;
    size_t second_style_len;

    first_pos = strstr(out, first);
    second_pos = first_pos == NULL ? NULL : strstr(first_pos + strlen(first), second);
    if (first_pos == NULL || second_pos == NULL) {
        return 0;
    }
    if (!ansi_style_before(out, first_pos, &first_style, &first_style_len) ||
        !ansi_style_before(out, second_pos, &second_style, &second_style_len)) {
        return 0;
    }
    return first_style_len == second_style_len &&
           memcmp(first_style, second_style, first_style_len) == 0;
}

static int html_span_style_for_text(const char *out, const char *text, const char **style, size_t *style_len)
{
    const char *pos;
    const char *span;
    const char *style_start;
    const char *style_end;

    pos = strstr(out, text);
    if (pos == NULL) {
        return 0;
    }
    span = pos;
    while (span > out && strncmp(span, "<span style=\"", 13) != 0) {
        span--;
    }
    if (strncmp(span, "<span style=\"", 13) != 0) {
        return 0;
    }
    style_start = span + 13;
    style_end = strchr(style_start, '"');
    if (style_end == NULL || style == NULL || style_len == NULL) {
        return 0;
    }
    *style = style_start;
    *style_len = (size_t)(style_end - style_start);
    return 1;
}

static int html_chart_texts_match_style(const char *out, const char *first, const char *second)
{
    const char *first_style;
    const char *second_style;
    size_t first_style_len;
    size_t second_style_len;

    if (!html_span_style_for_text(out, first, &first_style, &first_style_len) ||
        !html_span_style_for_text(out, second, &second_style, &second_style_len)) {
        return 0;
    }
    return first_style_len == second_style_len &&
           memcmp(first_style, second_style, first_style_len) == 0;
}

static int html_chart_label_matches_bar_style(const char *out, const char *label)
{
    const char *label_pos;
    const char *bar_pos;
    const char *label_span;
    const char *bar_span;
    const char *label_style_start;
    const char *label_style_end;
    const char *bar_style_start;
    const char *bar_style_end;
    const char *label_color;
    const char *bar_bg;
    size_t label_style_len;
    size_t bar_style_len;
    size_t label_color_len;
    size_t bar_bg_len;

    label_pos = strstr(out, label);
    bar_pos = label_pos == NULL ? NULL : strstr(label_pos + strlen(label), "\342\226\210");
    if (label_pos == NULL || bar_pos == NULL) {
        return 0;
    }
    label_span = label_pos;
    while (label_span > out && strncmp(label_span, "<span style=\"", 13) != 0) {
        label_span--;
    }
    bar_span = bar_pos;
    while (bar_span > out && strncmp(bar_span, "<span style=\"", 13) != 0) {
        bar_span--;
    }
    if (strncmp(label_span, "<span style=\"", 13) != 0 ||
        strncmp(bar_span, "<span style=\"", 13) != 0) {
        return 0;
    }
    label_style_start = label_span + 13;
    bar_style_start = bar_span + 13;
    label_style_end = strchr(label_style_start, '"');
    bar_style_end = strchr(bar_style_start, '"');
    if (label_style_end == NULL || bar_style_end == NULL) {
        return 0;
    }
    label_style_len = (size_t)(label_style_end - label_style_start);
    bar_style_len = (size_t)(bar_style_end - bar_style_start);
    label_color = strstr(label_style_start, "color:rgb(");
    bar_bg = strstr(bar_style_start, "background-color:rgb(");
    if (label_color == NULL || label_color >= label_style_end ||
        bar_bg == NULL || bar_bg >= bar_style_end) {
        return label_style_len == bar_style_len &&
               memcmp(label_style_start, bar_style_start, label_style_len) == 0;
    }
    label_color += strlen("color:rgb(");
    bar_bg += strlen("background-color:rgb(");
    if (strchr(label_color, ')') == NULL || strchr(bar_bg, ')') == NULL) {
        return 0;
    }
    label_color_len = (size_t)(strchr(label_color, ')') - label_color);
    bar_bg_len = (size_t)(strchr(bar_bg, ')') - bar_bg);
    return label_color_len == bar_bg_len &&
           memcmp(label_color, bar_bg, label_color_len) == 0;
}

static char *html_visible_text(const char *src)
{
    char *out;
    size_t i;
    size_t j;
    int in_tag;
    int in_style;

    out = (char *)malloc(strlen(src) + 1);
    if (out == NULL) {
        return NULL;
    }
    i = 0;
    j = 0;
    in_tag = 0;
    in_style = 0;
    while (src[i] != '\0') {
        if (in_style) {
            if (strncmp(src + i, "</style>", 8) == 0) {
                i += 8;
                in_style = 0;
            } else {
                i++;
            }
            continue;
        }
        if (!in_tag && strncmp(src + i, "<style", 6) == 0) {
            in_tag = 1;
            in_style = 1;
            while (src[i] != '\0' && src[i] != '>') {
                i++;
            }
            if (src[i] == '>') {
                i++;
            }
            in_tag = 0;
            continue;
        }
        if (src[i] == '<') {
            in_tag = 1;
            i++;
            continue;
        }
        if (in_tag) {
            if (src[i] == '>') {
                in_tag = 0;
            }
            i++;
            continue;
        }
        out[j++] = src[i++];
    }
    out[j] = '\0';
    return out;
}

static int html_vertical_chart_axis_columns_aligned(const char *out)
{
    char *visible;
    const char *line;
    size_t expected_col;
    int have_expected;
    int ok;

    visible = html_visible_text(out);
    if (visible == NULL) {
        return 0;
    }
    line = visible;
    expected_col = 0;
    have_expected = 0;
    ok = 1;
    while (*line != '\0') {
        const char *end;
        const char *axis;
        const char *a;
        const char *b;
        const char *c;
        size_t col;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        a = strstr(line, "\342\224\244");
        b = strstr(line, "\342\224\202");
        c = strstr(line, "\342\224\224");
        axis = NULL;
        if (a != NULL && a < end) axis = a;
        if (b != NULL && b < end && (axis == NULL || b < axis)) axis = b;
        if (c != NULL && c < end && (axis == NULL || c < axis)) axis = c;
        if (axis != NULL) {
            col = (size_t)(axis - line);
            if (!have_expected) {
                expected_col = col;
                have_expected = 1;
            } else if (col != expected_col) {
                ok = 0;
                break;
            }
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    free(visible);
    return ok && have_expected;
}

static size_t max_visible_line_cols(const char *s)
{
    size_t max_cols;
    size_t cols;
    size_t i;

    max_cols = 0;
    cols = 0;
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c;

        c = (unsigned char)s[i];
        if (s[i] == '\n') {
            if (cols > max_cols) {
                max_cols = cols;
            }
            cols = 0;
            continue;
        }
        if ((c & 0xc0) == 0x80) {
            continue;
        }
        cols++;
    }
    if (cols > max_cols) {
        max_cols = cols;
    }
    return max_cols;
}

static size_t max_ansi_visible_line_cols(const char *s)
{
    char *copy;
    size_t cols;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    cols = max_visible_line_cols(copy);
    free(copy);
    return cols;
}

static int ansi_visible_contains(const char *s, const char *needle)
{
    char *copy;
    int ok;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    ok = strstr(copy, needle) != NULL;
    free(copy);
    return ok;
}

static int ansi_visible_has_exact_trimmed_line(const char *s, const char *needle)
{
    char *copy;
    const char *line;
    int ok;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    ok = 0;
    line = copy;
    while (*line != '\0') {
        const char *end;
        const char *start;
        size_t needle_len;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        start = line;
        while (start < end && *start == ' ') {
            start++;
        }
        needle_len = strlen(needle);
        if ((size_t)(end - start) == needle_len && memcmp(start, needle, needle_len) == 0) {
            ok = 1;
            break;
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    free(copy);
    return ok;
}

static size_t visible_col_for_ptr(const char *line, const char *pos);

static int first_ansi_visible_line_has_leading_spaces(const char *s, size_t min_spaces)
{
    char *copy;
    const char *line;
    int ok;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    ok = 0;
    line = copy;
    while (*line != '\0') {
        const char *end;
        size_t len;
        size_t spaces;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        len = (size_t)(end - line);
        if (len > 0) {
            spaces = 0;
            while (spaces < len && line[spaces] == ' ') {
                spaces++;
            }
            ok = spaces >= min_spaces;
            break;
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    free(copy);
    return ok;
}

static int visible_line_contains(const char *line, const char *end, const char *needle)
{
    size_t needle_len;
    const char *p;

    needle_len = strlen(needle);
    if (needle_len == 0) {
        return 1;
    }
    p = line;
    while (p + needle_len <= end) {
        if (memcmp(p, needle, needle_len) == 0) {
            return 1;
        }
        p++;
    }
    return 0;
}

static int first_ansi_visible_label_col(const char *s, const char *label, size_t *out_col)
{
    char *copy;
    const char *line;
    int ok;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    ok = 0;
    line = copy;
    while (*line != '\0') {
        const char *end;
        const char *pos;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        pos = strstr(line, label);
        if (pos != NULL && pos < end) {
            *out_col = visible_col_for_ptr(line, pos);
            ok = 1;
            break;
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    free(copy);
    return ok;
}

static int ansi_visible_labels_share_col(const char *s, const char **labels, size_t label_count)
{
    size_t expected;
    size_t col;
    size_t i;

    if (label_count == 0) {
        return 1;
    }
    if (!first_ansi_visible_label_col(s, labels[0], &expected)) {
        return 0;
    }
    for (i = 1; i < label_count; i++) {
        if (!first_ansi_visible_label_col(s, labels[i], &col) || col != expected) {
            return 0;
        }
    }
    return 1;
}

static mdf_status render_chunked_cstr(mdf *renderer, const char *src, size_t chunk, char **out)
{
    chunk_source input;
    grow_sink output;
    mdf_source source;
    mdf_sink sink;
    mdf_status st;

    memset(&input, 0, sizeof(input));
    memset(&output, 0, sizeof(output));
    input.src = src;
    input.len = strlen(src);
    input.chunk = chunk;
    source.userdata = &input;
    source.read = chunk_read;
    sink.userdata = &output;
    sink.write = grow_write;
    st = renderer->render(renderer, &source, &sink);
    if (st != MDF_OK) {
        grow_sink_free(&output);
        *out = NULL;
        return st;
    }
    if (grow_write(&output, "", 1) != 0) {
        grow_sink_free(&output);
        *out = NULL;
        return MDF_ERROR_NOMEM;
    }
    output.len--;
    *out = output.buf;
    return MDF_OK;
}

static int ansi_visible_chart_lines_start_with(const char *s, const char *prefix)
{
    char *copy;
    const char *line;
    size_t prefix_len;
    int saw_chart_line;
    int ok;

    copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, s);
    strip_ansi_inplace(copy);
    prefix_len = strlen(prefix);
    saw_chart_line = 0;
    ok = 1;
    line = copy;
    while (*line != '\0') {
        const char *end;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        if (visible_line_contains(line, end, "\342\224\202") ||
            visible_line_contains(line, end, "\342\226\210") ||
            visible_line_contains(line, end, "chart: no data")) {
            saw_chart_line = 1;
            if ((size_t)(end - line) < prefix_len || memcmp(line, prefix, prefix_len) != 0) {
                ok = 0;
                break;
            }
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    free(copy);
    return saw_chart_line && ok;
}

static int html_tile_chart_first_rows_are_adjacent(const char *s)
{
    const char *needle;
    const char *first;
    const char *second;

    needle = "<span class=\"mdf-line mdf-chart-line\" style=\"display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"";
    first = strstr(s, needle);
    if (first == NULL) {
        return 0;
    }
    second = strstr(first + strlen(needle), needle);
    if (second == NULL) {
        return 0;
    }
    return memchr(first, '\n', (size_t)(second - first)) == NULL;
}

static int html_quoted_chart_prefix_is_not_chart_line(const char *s)
{
    return strstr(s, "\">&gt;</span>\n<div class=\"mdf-chart-block\"") == NULL;
}

static int visible_line_has_marker_at_col(const char *line, const char *line_end, size_t target_col, const char *marker)
{
    size_t col;
    size_t marker_len;
    const char *p;

    col = 0;
    marker_len = strlen(marker);
    p = line;
    while (p < line_end && *p != '\0') {
        unsigned char c;

        c = (unsigned char)*p;
        if ((c & 0xc0) == 0x80) {
            p++;
            continue;
        }
        if (col == target_col &&
            (size_t)(line_end - p) >= marker_len &&
            memcmp(p, marker, marker_len) == 0) {
            return 1;
        }
        col++;
        p++;
    }
    return 0;
}

static size_t visible_col_for_ptr(const char *line, const char *pos)
{
    const char *p;
    size_t col;

    col = 0;
    p = line;
    while (p < pos && *p != '\0') {
        unsigned char c;

        c = (unsigned char)*p;
        if ((c & 0xc0) != 0x80) {
            col++;
        }
        p++;
    }
    return col;
}

static int chart_tick_label_has_marker_above(const char *out, char label, const char *marker)
{
    char *copy;
    const char *line;
    const char *label_line;
    const char *label_pos;
    size_t label_col;
    int ok;

    copy = (char *)malloc(strlen(out) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, out);
    strip_ansi_inplace(copy);
    label_line = NULL;
    label_pos = NULL;
    line = copy;
    while (*line != '\0') {
        const char *line_end;
        const char *p;

        line_end = strchr(line, '\n');
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        p = line;
        while (p < line_end) {
            if (*p == label) {
                label_line = line;
                label_pos = p;
            }
            p++;
        }
        line = *line_end == '\n' ? line_end + 1 : line_end;
    }
    if (label_line == NULL || label_pos == NULL) {
        free(copy);
        return 0;
    }
    label_col = visible_col_for_ptr(label_line, label_pos);
    ok = 0;
    line = copy;
    while (line < label_line && *line != '\0') {
        const char *line_end;

        line_end = strchr(line, '\n');
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        if (visible_line_has_marker_at_col(line, line_end, label_col, marker)) {
            ok = 1;
            break;
        }
        line = *line_end == '\n' ? line_end + 1 : line_end;
    }
    free(copy);
    return ok;
}

static int raw_ansi_lines_start_with_margin_outside_styles(const char *out, int left_margin)
{
    const char *line;

    line = out;
    while (*line != '\0') {
        const char *end;
        size_t len;
        int i;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        len = (size_t)(end - line);
        if (len > 0) {
            if (left_margin > 0 && line[0] == '\033') {
                return 0;
            }
            if (len < (size_t)left_margin) {
                return 0;
            }
            for (i = 0; i < left_margin; i++) {
                if (line[i] != ' ') {
                    return 0;
                }
            }
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    return 1;
}

static int raw_ansi_lines_have_margin_before_styles(const char *out, int left_margin)
{
    const char *line;

    line = out;
    while (*line != '\0') {
        const char *end;
        size_t len;
        int i;

        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        len = (size_t)(end - line);
        if (len > 0) {
            if (left_margin > 0 && line[0] == '\033') {
                return 0;
            }
            if (len < (size_t)left_margin) {
                return 0;
            }
            for (i = 0; i < left_margin; i++) {
                if (line[i] != ' ') {
                    return 0;
                }
            }
        }
        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
    return 1;
}

static int raw_osc8_sequences_are_balanced(const char *out)
{
    return count_substr(out, "\033]8;;https://agilemanifesto.org/\033\\") ==
           count_substr(out, "\033]8;;\033\\");
}

static int raw_osc8_closed_at_linebreaks(const char *out)
{
    size_t i;
    int osc8_active;

    osc8_active = 0;
    for (i = 0; out[i] != '\0'; i++) {
        if (out[i] == '\033' && out[i + 1] == ']') {
            size_t start;
            size_t end;

            start = i + 2;
            end = start;
            while (out[end] != '\0') {
                if (out[end] == '\007') {
                    break;
                }
                if (out[end] == '\033' && out[end + 1] == '\\') {
                    break;
                }
                end++;
            }
            if (end >= start + 3 && out[start] == '8' && out[start + 1] == ';' && out[start + 2] == ';') {
                osc8_active = end > start + 3;
            }
            if (out[end] == '\007') {
                i = end;
            } else if (out[end] == '\033' && out[end + 1] == '\\') {
                i = end + 1;
            }
            continue;
        }
        if (out[i] == '\n' && osc8_active) {
            return 0;
        }
    }
    return !osc8_active;
}

static int sgr_sequence_is_reset(const char *seq, size_t len)
{
    size_t i;
    int value;
    int have_digit;

    if (len == 0) {
        return 1;
    }
    value = 0;
    have_digit = 0;
    for (i = 0; i <= len; i++) {
        if (i < len && seq[i] >= '0' && seq[i] <= '9') {
            have_digit = 1;
            value = (value * 10) + (seq[i] - '0');
            continue;
        }
        if (i == len || seq[i] == ';') {
            if (!have_digit || value == 0) {
                return 1;
            }
            value = 0;
            have_digit = 0;
        }
    }
    return 0;
}

static int raw_ansi_styles_closed_at_linebreaks(const char *out)
{
    size_t i;
    int sgr_active;

    sgr_active = 0;
    for (i = 0; out[i] != '\0'; i++) {
        if (out[i] == '\033' && out[i + 1] == '[') {
            size_t start;
            size_t end;

            start = i + 2;
            end = start;
            while (out[end] != '\0' && out[end] != 'm' && ((out[end] < '@') || (out[end] > '~'))) {
                end++;
            }
            if (out[end] == 'm') {
                sgr_active = !sgr_sequence_is_reset(out + start, end - start);
                i = end;
                continue;
            }
        }
        if (out[i] == '\n' && sgr_active) {
            return 0;
        }
    }
    return 1;
}

static int run_ansi_link_wrap_regression_case(const char *name, const char *markdown,
                                              int width, int left_margin, int right_margin,
                                              int osc8, const char *required_a,
                                              const char *required_b, const char *required_c)
{
    mdf_options opts;
    mdf *renderer;
    mdf_status st;
    char *out;
    char *visible;
    int ok;
    size_t len;

    mdf_options_init(&opts);
    opts.width = width;
    opts.margin_left = left_margin;
    opts.margin_right = right_margin;
    opts.osc8 = osc8;
    opts.boring = 0;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK || renderer == NULL) {
        fprintf(stderr, "FAIL: %s renderer create width=%d left=%d right=%d osc8=%d\n",
                name, width, left_margin, right_margin, osc8);
        return 0;
    }
    out = NULL;
    st = renderer->render_cstr(renderer, markdown, &out);
    if (st != MDF_OK || out == NULL) {
        fprintf(stderr, "FAIL: %s render width=%d left=%d right=%d osc8=%d\n",
                name, width, left_margin, right_margin, osc8);
        renderer->destroy(renderer);
        return 0;
    }
    ok = 1;
    if (!margin_output_lines_are_shaped(out, left_margin)) {
        fprintf(stderr, "FAIL: %s margin shape width=%d left=%d right=%d osc8=%d\n",
                name, width, left_margin, right_margin, osc8);
        ok = 0;
    }
    if (!raw_ansi_lines_start_with_margin_outside_styles(out, left_margin)) {
        fprintf(stderr, "FAIL: %s raw ANSI margin/style boundary width=%d left=%d right=%d osc8=%d\n",
                name, width, left_margin, right_margin, osc8);
        ok = 0;
    }
    if (!raw_ansi_styles_closed_at_linebreaks(out)) {
        fprintf(stderr, "FAIL: %s ANSI style crosses linebreak width=%d left=%d right=%d osc8=%d\n",
                name, width, left_margin, right_margin, osc8);
        ok = 0;
    }
    if (osc8 && !raw_osc8_sequences_are_balanced(out)) {
        fprintf(stderr, "FAIL: %s unbalanced OSC8 sequences width=%d left=%d right=%d\n",
                name, width, left_margin, right_margin);
        ok = 0;
    }
    if (osc8 && !raw_osc8_closed_at_linebreaks(out)) {
        fprintf(stderr, "FAIL: %s OSC8 crosses linebreak width=%d left=%d right=%d\n",
                name, width, left_margin, right_margin);
        ok = 0;
    }
    if (osc8 && strstr(out, "\033]8;;https://agilemanifesto.org/\033\\\n") != NULL) {
        fprintf(stderr, "FAIL: %s stranded osc8 opener width=%d left=%d right=%d\n",
                name, width, left_margin, right_margin);
        ok = 0;
    }
    if (left_margin == 2 && strstr(out, "\n   and guided") != NULL) {
        fprintf(stderr, "FAIL: %s leaked leading space before continuation width=%d right=%d osc8=%d\n",
                name, width, right_margin, osc8);
        ok = 0;
    }
    len = strlen(out);
    visible = (char *)malloc(len + 1);
    if (visible == NULL) {
        ok = 0;
    } else {
        memcpy(visible, out, len + 1);
        strip_ansi_inplace(visible);
        if (required_a != NULL && !visible_contains_phrase(visible, required_a)) {
            fprintf(stderr, "FAIL: %s missing visible text '%s' width=%d left=%d right=%d osc8=%d\n",
                    name, required_a, width, left_margin, right_margin, osc8);
            ok = 0;
        }
        if (required_b != NULL && !visible_contains_phrase(visible, required_b)) {
            fprintf(stderr, "FAIL: %s missing visible text '%s' width=%d left=%d right=%d osc8=%d\n",
                    name, required_b, width, left_margin, right_margin, osc8);
            ok = 0;
        }
        if (required_c != NULL && !visible_contains_phrase(visible, required_c)) {
            fprintf(stderr, "FAIL: %s missing visible text '%s' width=%d left=%d right=%d osc8=%d\n",
                    name, required_c, width, left_margin, right_margin, osc8);
            ok = 0;
        }
        free(visible);
    }
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return ok;
}

static int run_ansi_link_wrap_regression_cases(void)
{
    static const char *obaf =
        "change through adaptive learning and value delivery. Inspired by the original "
        "[Agile Manifesto](https://agilemanifesto.org/), this framework defines a model "
        "where teams are driven by **outcomes**, **not requirements**, and guided by "
        "evidence, not assumption.\n";
    static const char *compact =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
        "[Agile Manifesto](https://agilemanifesto.org/), tail with **not requirements**, "
        "and guided text.\n";
    static const char *autolink =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
        "<https://agilemanifesto.org/> tail with **outcomes**, **not requirements**, "
        "and guided text.\n";
    static const char *styled_start =
        "**not requirements**, and guided by evidence after a styled phrase that begins the line.\n";
    static const char *link_start =
        "[Agile Manifesto](https://agilemanifesto.org/), then **not requirements**, "
        "and guided by evidence at the beginning of the paragraph.\n";
    static const char *blank_lines =
        "\n\n[Agile Manifesto](https://agilemanifesto.org/), **outcomes**, "
        "**not requirements**, and guided text after blank lines.\n\n";
    static const char *punctuated =
        "prefix **outcomes**, **not requirements**; [Agile Manifesto](https://agilemanifesto.org/) "
        "keeps punctuation, styles, and links around wraps.\n";
    static const char *long_link =
        "[A longer link label that wraps across lines in narrow viewports](https://agilemanifesto.org/) "
        "keeps the continuation margin outside the link.\n";
    static const int widths[] = {20, 25, 30, 35, 40, 50, 60, 65, 70, 75, 80, 85, 90};
    static const int margins[][2] = {
        {0, 0},
        {2, 0},
        {0, 3},
        {2, 3},
        {6, 4}
    };
    static const int osc8s[] = {0, 1};
    size_t w;
    size_t m;
    size_t o;

    for (w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (m = 0; m < sizeof(margins) / sizeof(margins[0]); m++) {
            for (o = 0; o < sizeof(osc8s) / sizeof(osc8s[0]); o++) {
                if (!run_ansi_link_wrap_regression_case("obaf", obaf, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "Agile Manifesto", "outcomes", "not requirements")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("compact", compact, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "Agile Manifesto", "not requirements", "and guided")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("autolink", autolink, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "tail with", "outcomes", "not requirements")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("styled-start", styled_start, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "not requirements", "and guided", "styled phrase")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("link-start", link_start, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "Agile Manifesto", "not requirements", "and guided")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("blank-lines", blank_lines, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "Agile Manifesto", "outcomes", "not requirements")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("punctuated", punctuated, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "outcomes", "not requirements", "Agile Manifesto")) {
                    return 0;
                }
                if (!run_ansi_link_wrap_regression_case("long-link", long_link, widths[w],
                        margins[m][0], margins[m][1], osc8s[o],
                        "A longer link label", "narrow viewports", "continuation margin")) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int run_ansi_margin_table_regression_case(const char *name, const char *markdown,
                                                 int width, int left_margin, int right_margin,
                                                 mdf_table_buffer_mode buffer_mode,
                                                 mdf_table_wire_mode wire_mode,
                                                 const char *required_a,
                                                 const char *required_b,
                                                 const char *required_c)
{
    mdf_options opts;
    mdf *renderer;
    mdf_status st;
    char *out;
    char *visible;
    int ok;
    size_t len;

    mdf_options_init(&opts);
    opts.width = width;
    opts.margin_left = left_margin;
    opts.margin_right = right_margin;
    opts.osc8 = 0;
    opts.boring = 0;
    opts.table_buffer_mode = buffer_mode;
    opts.table_wire_mode = wire_mode;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK || renderer == NULL) {
        fprintf(stderr, "FAIL: %s table renderer create width=%d left=%d right=%d buffer=%d wire=%d\n",
                name, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
        return 0;
    }
    out = NULL;
    st = renderer->render_cstr(renderer, markdown, &out);
    if (st != MDF_OK || out == NULL) {
        fprintf(stderr, "FAIL: %s table render width=%d left=%d right=%d buffer=%d wire=%d\n",
                name, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
        renderer->destroy(renderer);
        return 0;
    }
    ok = 1;
    if (!margin_output_lines_are_shaped(out, left_margin)) {
        fprintf(stderr, "FAIL: %s table margin shape width=%d left=%d right=%d buffer=%d wire=%d\n",
                name, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
        ok = 0;
    }
    if (!raw_ansi_lines_have_margin_before_styles(out, left_margin)) {
        fprintf(stderr, "FAIL: %s table raw ANSI margin/style boundary width=%d left=%d right=%d buffer=%d wire=%d\n",
                name, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
        ok = 0;
    }
    if (!raw_ansi_styles_closed_at_linebreaks(out)) {
        fprintf(stderr, "FAIL: %s table ANSI style crosses linebreak width=%d left=%d right=%d buffer=%d wire=%d\n",
                name, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
        ok = 0;
    }
    len = strlen(out);
    visible = (char *)malloc(len + 1);
    if (visible == NULL) {
        ok = 0;
    } else {
        memcpy(visible, out, len + 1);
        strip_ansi_inplace(visible);
        if (required_a != NULL && !visible_contains_phrase(visible, required_a)) {
            fprintf(stderr, "FAIL: %s table missing visible text '%s' width=%d left=%d right=%d buffer=%d wire=%d\n",
                    name, required_a, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
            ok = 0;
        }
        if (required_b != NULL && !visible_contains_phrase(visible, required_b)) {
            fprintf(stderr, "FAIL: %s table missing visible text '%s' width=%d left=%d right=%d buffer=%d wire=%d\n",
                    name, required_b, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
            ok = 0;
        }
        if (required_c != NULL && !visible_contains_phrase(visible, required_c)) {
            fprintf(stderr, "FAIL: %s table missing visible text '%s' width=%d left=%d right=%d buffer=%d wire=%d\n",
                    name, required_c, width, left_margin, right_margin, (int)buffer_mode, (int)wire_mode);
            ok = 0;
        }
        free(visible);
    }
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return ok;
}

static int run_ansi_margin_table_regression_cases(void)
{
    static const char *inline_table =
        "| Topic | Link | Styled |\n"
        "| --- | --- | --- |\n"
        "| governance | [Agile Manifesto](https://agilemanifesto.org/) | **not requirements**, and guided by evidence |\n"
        "| outcomes | <https://agilemanifesto.org/> | *adaptive learning* with **outcomes** preserved |\n";
    static const char *narrow_table =
        "| Name | Description |\n"
        "| --- | --- |\n"
        "| mixed long | plain text before *italic phrase* then **bold phrase** then ***combined phrase*** with enough words to wrap |\n"
        "| linked long | [Agile Manifesto](https://agilemanifesto.org/) then **not requirements**, and guided by evidence |\n";
    static const struct {
        int width;
        int left;
        int right;
        mdf_table_buffer_mode buffer;
        mdf_table_wire_mode wire;
    } cases[] = {
        {30, 0, 0, MDF_TABLE_BUFFER_FULL, MDF_TABLE_WIRE_LINE},
        {40, 2, 0, MDF_TABLE_BUFFER_ROW, MDF_TABLE_WIRE_ASCII},
        {60, 0, 4, MDF_TABLE_BUFFER_FULL, MDF_TABLE_WIRE_SPACE},
        {80, 2, 4, MDF_TABLE_BUFFER_ROW, MDF_TABLE_WIRE_LINE},
        {100, 6, 4, MDF_TABLE_BUFFER_FULL, MDF_TABLE_WIRE_ASCII},
        {30, 6, 4, MDF_TABLE_BUFFER_ROW, MDF_TABLE_WIRE_SPACE}
    };
    size_t c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (!run_ansi_margin_table_regression_case("inline-table", inline_table,
                cases[c].width, cases[c].left, cases[c].right, cases[c].buffer, cases[c].wire,
                "Ag", "req", "ada")) {
            return 0;
        }
        if (!run_ansi_margin_table_regression_case("narrow-table", narrow_table,
                cases[c].width, cases[c].left, cases[c].right, cases[c].buffer, cases[c].wire,
                "italic", "bold", "req")) {
            return 0;
        }
    }
    return 1;
}

static int run_ansi_table_cell_margin_regression_case(void)
{
    static const char *markdown =
        "| Capability | Desktop | Mobile | Notes |\n"
        "| --- | --- | --- | --- |\n"
        "| Keyboard navigation | yes | yes | external keyboards should work |\n"
        "| Touch navigation | yes | yes | horizontal and vertical swipes are supported |\n";
    mdf_options opts;
    mdf *renderer;
    mdf_status st;
    char *out;
    char *visible;
    int ok;
    size_t len;

    mdf_options_init(&opts);
    opts.width = 100;
    opts.margin_left = 10;
    opts.margin_right = 10;
    opts.theme_name = "everforest";
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK || renderer == NULL) {
        return 0;
    }
    out = NULL;
    st = renderer->render_cstr(renderer, markdown, &out);
    if (st != MDF_OK || out == NULL) {
        renderer->destroy(renderer);
        return 0;
    }
    len = strlen(out);
    visible = (char *)malloc(len + 1);
    if (visible == NULL) {
        renderer->string_free(renderer, out);
        renderer->destroy(renderer);
        return 0;
    }
    memcpy(visible, out, len + 1);
    strip_ansi_inplace(visible);
    ok = 1;
    if (strstr(visible, "│           Keyboard") != NULL ||
        strstr(visible, "│           Touch") != NULL ||
        strstr(visible, "│           external") != NULL) {
        fprintf(stderr, "FAIL: table cell renderer leaked left margin into cell text\n");
        ok = 0;
    }
    if (strstr(visible, "│ Keyboard") == NULL ||
        strstr(visible, "│ Touch") == NULL ||
        strstr(visible, "│ external") == NULL) {
        fprintf(stderr, "FAIL: table cell text is not left-aligned after margin stripping\n");
        ok = 0;
    }
    free(visible);
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return ok;
}

static int run_ansi_table_osc8_regression_case(void)
{
    static const char *markdown =
        "| Name | Description |\n"
        "| --- | --- |\n"
        "| linked long | [Agile Manifesto](https://agilemanifesto.org/) then **not requirements**, and guided by evidence |\n";
    mdf_options opts;
    mdf *renderer;
    mdf_status st;
    char *out;
    int ok;

    mdf_options_init(&opts);
    opts.width = 30;
    opts.margin_left = 6;
    opts.margin_right = 4;
    opts.osc8 = 1;
    opts.boring = 0;
    opts.table_buffer_mode = MDF_TABLE_BUFFER_ROW;
    opts.table_wire_mode = MDF_TABLE_WIRE_SPACE;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK || renderer == NULL) {
        return 0;
    }
    out = NULL;
    st = renderer->render_cstr(renderer, markdown, &out);
    if (st != MDF_OK || out == NULL) {
        renderer->destroy(renderer);
        return 0;
    }
    ok = 1;
    if (!margin_output_lines_are_shaped(out, opts.margin_left)) {
        fprintf(stderr, "FAIL: table osc8 margin shape\n");
        ok = 0;
    }
    if (!raw_ansi_lines_have_margin_before_styles(out, opts.margin_left)) {
        fprintf(stderr, "FAIL: table osc8 margin/style boundary\n");
        ok = 0;
    }
    if (!raw_ansi_styles_closed_at_linebreaks(out)) {
        fprintf(stderr, "FAIL: table osc8 ANSI style crosses linebreak\n");
        ok = 0;
    }
    if (!raw_osc8_sequences_are_balanced(out)) {
        fprintf(stderr, "FAIL: table osc8 unbalanced sequences\n");
        ok = 0;
    }
    if (!raw_osc8_closed_at_linebreaks(out)) {
        fprintf(stderr, "FAIL: table osc8 crosses linebreak\n");
        ok = 0;
    }
    if (strstr(out, "\033]8;;https://agilemanifesto.org/\033\\\033[0m") != NULL) {
        fprintf(stderr, "FAIL: table osc8 opener immediately reset\n");
        ok = 0;
    }
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return ok;
}

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    mdf_parser *parser;
    mdf_sink sink;
    mdf_source framed_source;
    mem_sink sink_data;
    mdf_token tok;
    char *out;
    mdf_status st;
    counting_allocator allocs;
    int fails;
    static const unsigned char test_font[] = {1, 2, 3};

    fails = 0;
    memset(&allocs, 0, sizeof(allocs));
    mdf_options_init(&opts);
    opts.boring = 1;
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "create rejects null out");
    st = mdf_create((mdf_format)99, &opts, &renderer);
    fails += expect(st == MDF_ERROR_INVALID && renderer == NULL, "create rejects invalid format");
    renderer = NULL;
    parser = NULL;
    out = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "renderer create");
    fails += expect(renderer != NULL &&
                    renderer->write_token != NULL &&
                    renderer->finish != NULL &&
                    renderer->render != NULL &&
                    renderer->render_cstr != NULL &&
                    renderer->error != NULL &&
                    renderer->destroy != NULL &&
                    renderer->string_free != NULL,
                    "single-handle api populates receiver methods");
    fails += expect(strcmp(renderer->error(NULL), "invalid instance") == 0,
                    "error reports invalid instance for null handle");
    st = renderer->render(NULL, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null self");
    st = renderer->render(renderer, NULL, NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render rejects null source and sink");
    st = renderer->render_cstr(renderer, NULL, &out);
    fails += expect(st == MDF_ERROR_INVALID && out == NULL, "render_cstr rejects null markdown");
    st = renderer->render_cstr(renderer, "# hi\n", NULL);
    fails += expect(st == MDF_ERROR_INVALID, "render_cstr rejects null out");
    renderer->string_free(renderer, NULL);
    st = mdf_parser_create(&opts, &parser);
    fails += expect(st == MDF_OK && parser != NULL, "parser create");
    {
        token_capture token_out;
        chunk_source src;

        memset(&token_out, 0, sizeof(token_out));
        token_out.base.write_token = token_capture_write_token;
        token_out.base.finish = token_capture_finish;
        src.src = "# T\n";
        src.len = strlen(src.src);
        src.off = 0;
        src.chunk = 1;
        framed_source.userdata = &src;
        framed_source.read = chunk_read;
        memset(&sink_data, 0, sizeof(sink_data));
        sink.userdata = &sink_data;
        sink.write = mem_write;
        st = parser->parse(parser, &framed_source, &token_out.base, &sink);
        fails += expect(st == MDF_OK && token_out.tokens >= 4, "parser emits through custom renderer receiver methods");
        fails += expect(token_out.sink.buf != NULL &&
                        strstr(token_out.sink.buf, "4:0\n") != NULL &&
                        strstr(token_out.sink.buf, "0:1\n") != NULL &&
                        strstr(token_out.sink.buf, "5:0\n") != NULL &&
                        strstr(token_out.sink.buf, "16:0\n") != NULL,
                        "custom renderer receives heading and document-end tokens");
        grow_sink_free(&token_out.sink);
    }
    renderer->destroy(renderer);
    renderer = NULL;

    mdf_options_init(&opts);
    st = mdf_create(MDF_FORMAT_HTML_DECK, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "deck renderer create before parser token-stream rejection");
    if (renderer != NULL) {
        chunk_source src;

        src.src = "# Deck token stream\n";
        src.len = strlen(src.src);
        src.off = 0;
        src.chunk = 1;
        framed_source.userdata = &src;
        framed_source.read = chunk_read;
        memset(&sink_data, 0, sizeof(sink_data));
        sink.userdata = &sink_data;
        sink.write = mem_write;
        st = parser->parse(parser, &framed_source, renderer, &sink);
        fails += expect(st == MDF_ERROR_INVALID && sink_data.len == 0,
                        "parser-driven token streaming rejects deck renderers");
        fails += expect(strcmp(renderer->error(renderer), "deck renderers do not support token streaming") == 0,
                        "deck renderer records unsupported token streaming error");
        renderer->destroy(renderer);
        renderer = NULL;
    }

    mdf_options_init(&opts);
    opts.allocator.userdata = &allocs;
    opts.allocator.alloc = test_alloc;
    opts.allocator.realloc = test_realloc;
    opts.allocator.free = test_free;
    opts.boring = 0;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "streaming invariant renderer create");
    if (renderer != NULL) {
        static const char streamed_emph[] = "**\"Governance exists to support autonomy\"** does not imply\n";
        const char *close;
        const char *space_after_first_word;
        stream_probe_sink probe;
        mdf_sink probe_sink;

        memset(&probe, 0, sizeof(probe));
        probe.needle = "Governance";
        probe.needle_len = strlen(probe.needle);
        probe_sink.userdata = &probe;
        probe_sink.write = stream_probe_write;
        close = strstr(streamed_emph + 2, "**");
        space_after_first_word = strchr(streamed_emph, ' ');
        for (probe.input_index = 0; probe.input_index < strlen(streamed_emph); probe.input_index++) {
            tok.type = MDF_TOKEN_TEXT;
            tok.text = streamed_emph + probe.input_index;
            tok.len = 1;
            tok.level = 0;
            st = renderer->write_token(renderer, &tok, &probe_sink);
            if (st != MDF_OK || (close != NULL && probe.input_index >= (size_t)(close - streamed_emph))) {
                break;
            }
        }
        fails += expect(st == MDF_OK, "streaming invariant accepts byte tokens through emphasis");
        fails += expect(probe.first_needle_input_index > 0, "streaming invariant emits first emphasis word before closing delimiter");
        fails += expect(close != NULL &&
                        probe.first_needle_input_index < (size_t)(close - streamed_emph),
                        "streaming invariant does not buffer emphasis until close");
        fails += expect(space_after_first_word != NULL &&
                        probe.first_needle_input_index <= (size_t)(space_after_first_word - streamed_emph + 1),
                        "streaming invariant emits emphasis word at first word boundary");
        tok.type = MDF_TOKEN_DOCUMENT_END;
        tok.text = NULL;
        tok.len = 0;
        tok.level = 0;
        st = renderer->write_token(renderer, &tok, &probe_sink);
        fails += expect(st == MDF_OK, "streaming invariant renderer finishes");
    }
    renderer->destroy(renderer);
    renderer = NULL;
    {
        emission_probe probe;
        chunk_source src;
        mdf_source source;
        mdf_sink probe_sink;
        parser_stream_probe_sink stream_probe;
        size_t newline_off;

        memset(&probe, 0, sizeof(probe));
        memset(&src, 0, sizeof(src));
        opts.write_trace.userdata = &probe;
        opts.write_trace.emit = emission_probe_trace_emit;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
        fails += expect(st == MDF_OK && renderer != NULL, "emission identity renderer create");
        if (renderer != NULL) {
            src.src = "# The Outcome-Based Agile Framework\n";
            src.len = strlen(src.src);
            src.chunk = 1;
            source.userdata = &src;
            source.read = chunk_read;
            probe_sink.userdata = &probe;
            probe_sink.write = emission_probe_sink_write;
            st = renderer->render(renderer, &source, &probe_sink);
            fails += expect(st == MDF_OK, "emission identity render succeeds");
            fails += expect(chunk_logs_equal(&probe.sink, &probe.trace),
                            "decision emissions and sink writes are one-to-one");
            fails += expect(probe.trace.count >= 12,
                            "heading emission identity keeps word-sized decision chunks");
            renderer->destroy(renderer);
            renderer = NULL;
        }
        memset(&probe, 0, sizeof(probe));
        memset(&src, 0, sizeof(src));
        opts.boring = 0;
        opts.width = 18;
        opts.write_trace.userdata = &probe;
        opts.write_trace.emit = emission_probe_trace_emit;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
        fails += expect(st == MDF_OK && renderer != NULL,
                        "styled heading emphasis renderer create");
        if (renderer != NULL) {
            src.src = "# **bold** trailing heading words that wrap\n";
            src.len = strlen(src.src);
            src.chunk = 1;
            source.userdata = &src;
            source.read = chunk_read;
            probe_sink.userdata = &probe;
            probe_sink.write = emission_probe_sink_write;
            st = renderer->render(renderer, &source, &probe_sink);
            fails += expect(st == MDF_OK, "styled heading emphasis render succeeds under ubsan");
            fails += expect(chunk_logs_equal(&probe.sink, &probe.trace),
                            "styled heading emphasis keeps emission identity");
            renderer->destroy(renderer);
            renderer = NULL;
        }
        opts.boring = 1;
        opts.width = 80;
        opts.write_trace.userdata = NULL;
        opts.write_trace.emit = NULL;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
        fails += expect(st == MDF_OK && renderer != NULL, "parser stream invariant renderer create");
        if (renderer != NULL) {
            memset(&src, 0, sizeof(src));
            memset(&stream_probe, 0, sizeof(stream_probe));
            src.src = "# The Outcome-Based Agile Framework\n";
            src.len = strlen(src.src);
            src.chunk = 1;
            source.userdata = &src;
            source.read = chunk_read;
            stream_probe.source = &src;
            stream_probe.needle = "The";
            stream_probe.needle_len = strlen(stream_probe.needle);
            probe_sink.userdata = &stream_probe;
            probe_sink.write = parser_stream_probe_write;
            st = renderer->render(renderer, &source, &probe_sink);
            newline_off = (size_t)(strchr(src.src, '\n') - src.src) + 1;
            fails += expect(st == MDF_OK, "parser stream invariant render succeeds");
            fails += expect(stream_probe.first_needle_source_off > 0,
                            "parser stream invariant emits heading text");
            fails += expect(stream_probe.first_needle_source_off < newline_off,
                            "parser stream invariant emits heading before newline read");
            renderer->destroy(renderer);
            renderer = NULL;
        }
    }
    opts.boring = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "boring renderer restore after streaming invariant");
    st = renderer->render_cstr(renderer, "# Hello\n\nWorld\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "render cstr");
    fails += expect(strstr(out, "Hello") != NULL && strstr(out, "World") != NULL, "render output");
    fails += expect(strstr(out, "# Hello") != NULL, "heading marker visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1. First\n2. Second\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ordered list render");
    fails += expect(strstr(out, "1. First") != NULL && strstr(out, "2. Second") != NULL, "ordered list markers visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "+ plus\n* star\n- dash\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "unordered list render");
    fails += expect(strstr(out, "- plus") != NULL && strstr(out, "- star") != NULL && strstr(out, "- dash") != NULL, "unordered markers normalized");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- one\n  - two\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "nested unordered list render");
    fails += expect(strstr(out, "\n  - two") != NULL, "nested unordered indent preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1) **North-star intent is upstream and non-prescriptive.**\n   - If the org uses an outcome framework, treat its north-star artifacts as upstream intent.\n   - Your unit maps to it.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ordered nested unordered render");
    fails += expect(strstr(out, "1) North-star intent is upstream and non-prescriptive.\n\n   - If the org uses") != NULL && strstr(out, "\n  - If the org uses") == NULL, "ordered nested unordered indent preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* one\n  * two\n    continuation text\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "nested star list continuation render");
    fails += expect(strstr(out, "\n  - two continuation text") != NULL, "nested star list continuation joins");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- [x] lower\n- [X] upper\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "checked task marker render");
    fails += expect(strstr(out, "[x] lower") != NULL && strstr(out, "[X] upper") != NULL, "checked task marker case preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- [ ] Do we define success in terms of user behavior, business value, or system\n      performance\342\200\224not just features delivered?\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "task continuation render");
    fails += expect(strstr(out, "- [ ] Do we define success") != NULL && strstr(out, "\n      performance") != NULL && strstr(out, "\n  performance") == NULL, "task continuation aligns after checkbox");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- [ ] Is the outcome flexible in *how* it\342\200\231s achieved, but firm in\n      *why* it matters?\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "task continuation emphasis render");
    fails += expect(strstr(out, "firm in why it matters?") != NULL && strstr(out, "*why*") == NULL, "task continuation emphasis parses inline");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "See [website](https://example.com) now.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "link fallback render");
    fails += expect(strstr(out, "website (https://example.com)") != NULL, "link fallback visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "**See [website](https://example.com) now**\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "emphasis link fallback render");
    fails += expect(strstr(out, "website (https://example.com)") != NULL && strstr(out, "[website]") == NULL, "emphasis link parsed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Read [*talks down the machine*](https://pkt.systems/tdtm.pdf).\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "link label emphasis render");
    fails += expect(strstr(out, "talks down the machine (https://pkt.systems/tdtm.pdf)") != NULL && strstr(out, "*talks") == NULL, "link label emphasis parsed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* *Michel Blomgren <sa6mwa@gmail.com> (2025-04-26)*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "emphasis email autolink render");
    fails += expect(strstr(out, "Michel Blomgren sa6mwa@gmail.com (2025-04-26)") != NULL && strstr(out, "<sa6mwa") == NULL, "emphasis email autolink parsed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "The study [How AI Impacts Skill Formation](https://www.anthropic.com/research/AI-assistance-coding-skills) finds more.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "long link fallback render");
    fails += expect(strstr(out, "How AI Impacts Skill Formation") != NULL && strstr(out, "(https://www.anthropic.com/research/AI-assistance-coding-skills)") != NULL, "long link fallback visible");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 8;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "styled punctuation wrap renderer create");
    st = renderer->render_cstr(renderer, "aaa **bbb**, ccc\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled punctuation wrap render");
    fails += expect(strstr(out, "aaa bbb,\nccc") != NULL, "styled punctuation wraps with word");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore");
    st = renderer->render_cstr(renderer, "<https://example.com>\n<user@example.com>\n<x>\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "autolink render");
    fails += expect(strstr(out, "<https://example.com>") == NULL && strstr(out, "https://example.com") != NULL, "url autolink visible");
    fails += expect(strstr(out, "<user@example.com>") == NULL && strstr(out, "user@example.com") != NULL, "email autolink visible");
    fails += expect(strstr(out, "<x>") != NULL, "invalid autolink preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Use `code` now.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "inline code render");
    fails += expect(strstr(out, "`code`") == NULL && strstr(out, "code") != NULL, "inline code removes backticks");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow inline code renderer create");
    st = renderer->render_cstr(renderer, "* All git commit messages **must** follow the Conventional Commits specification\n  (`type(scope): summary`).\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow inline code render");
    fails += expect(strstr(out, "  (type(scope):\n   summary).") != NULL && strstr(out, "`type") == NULL, "narrow inline code wraps with opener");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore after narrow inline code");
    st = renderer->render_cstr(renderer, "Backtick code: `` ` ``.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "double backtick code render");
    fails += expect(strstr(out, "Backtick code: `.") != NULL && strstr(out, "`` ` ``") == NULL, "double backtick code parsed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```\ncode\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "fenced code render");
    fails += expect(strstr(out, "```") == NULL && strstr(out, "code") != NULL && strstr(out, "\n\nnext") != NULL, "fenced code output");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nkey,value\nA,10\nB,20\nC,5\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart fence render");
    fails += expect(strstr(out, "```") == NULL &&
                    strstr(out, "A ") != NULL &&
                    strstr(out, "\342\224\202") != NULL &&
                    strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "57.1%") != NULL &&
                    strstr(out, "\n\nnext") != NULL,
                    "horizontal chart renders bars, percentages, and following paragraph");
    fails += expect(max_ansi_visible_line_cols(out) <= 64,
                    "horizontal chart uses centered proportional content width by default");
    fails += expect(first_ansi_visible_line_has_leading_spaces(out, 10),
                    "ANSI horizontal chart preserves centered leading pad");
    {
        size_t top_col;
        size_t quoted_col;
        size_t list_col;

        fails += expect(first_ansi_visible_label_col(out, "A ", &top_col),
                        "top-level chart exposes first label column");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "> ```mdf-bar-chart\n> A,10\n> B,20\n> ```\n\nnext\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "quoted chart fence render");
        fails += expect(out != NULL &&
                        strstr(out, "```") == NULL &&
                        strstr(out, "A ") != NULL &&
                        strstr(out, "B ") != NULL &&
                        strstr(out, "\n\nnext") != NULL,
                        "quoted chart strips container prefixes from chart data and closing fence");
        fails += expect(out != NULL && ansi_visible_chart_lines_start_with(out, "> "),
                        "quoted chart emits quote prefix on each chart line");
        fails += expect(out != NULL &&
                        first_ansi_visible_label_col(out, "A ", &quoted_col) &&
                        quoted_col == top_col,
                        "quoted chart subtracts quote prefix from centered padding");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "- item\n\n  ```mdf-bar-chart\n  A,10\n  B,20\n  ```\n\nnext\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "listed chart fence render");
        fails += expect(out != NULL &&
                        strstr(out, "```") == NULL &&
                        strstr(out, "A ") != NULL &&
                        strstr(out, "B ") != NULL &&
                        strstr(out, "\n\nnext") != NULL,
                        "listed chart strips container indentation from chart body and closing fence");
        fails += expect(out != NULL &&
                        first_ansi_visible_label_col(out, "A ", &list_col) &&
                        list_col == top_col,
                        "listed chart subtracts list indentation from centered padding");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "> before\n> ```mdf-bar-chart\n> A,1\n> B,2\n> ```\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "quoted chart after text render");
        fails += expect(out != NULL &&
                        ansi_visible_contains(out, "> before\n> ") &&
                        !ansi_visible_contains(out, "> before\n\n>") &&
                        !ansi_visible_contains(out, "> before        A"),
                        "quoted chart after text starts on next quote line without blank line");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "> before\n>\n> ```mdf-bar-chart\n> A,1\n> B,2\n> ```\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "quoted chart after blank quote line render");
        fails += expect(out != NULL &&
                        ansi_visible_contains(out, "> before\n>\n> ") &&
                        !ansi_visible_contains(out, "> before\n>\n>\n> ") &&
                        !ansi_visible_contains(out, "> before\n\n>"),
                        "quoted chart after blank quote line does not add another quote blank");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "- before\n  ```mdf-bar-chart\n  A,1\n  B,2\n  ```\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "listed chart after text render");
        fails += expect(out != NULL &&
                        ansi_visible_contains(out, "- before\n ") &&
                        !ansi_visible_contains(out, "- before\n\n") &&
                        !ansi_visible_contains(out, "- before        A"),
                        "listed chart after text starts on next list line without blank line");
        renderer->string_free(renderer, out);
        out = NULL;
        st = renderer->render_cstr(renderer, "- before\n\n  ```mdf-bar-chart\n  A,1\n  B,2\n  ```\n", &out);
        fails += expect(st == MDF_OK && out != NULL, "listed chart after blank list line render");
        fails += expect(out != NULL &&
                        ansi_visible_contains(out, "- before\n\n ") &&
                        !ansi_visible_contains(out, "- before\n\n\n"),
                        "listed chart after blank list line does not add another blank line");
        renderer->string_free(renderer, out);
        out = NULL;
    }
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "renderer recreate before chart allocation failure");
    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = mem_write;
    tok.type = MDF_TOKEN_CHART_BLOCK;
    tok.text = "A,10\n";
    tok.len = 5;
    tok.level = MDF_CHART_KIND_HORIZONTAL_BAR;
    allocs.fail_at_alloc = allocs.allocs + 1;
    st = renderer->write_token(renderer, &tok, &sink);
    fails += expect(st == MDF_ERROR_NOMEM, "chart value allocation failure reports NOMEM");
    fails += expect(renderer->error(renderer) != NULL &&
                    strstr(renderer->error(renderer), "out of memory") != NULL,
                    "chart value allocation failure preserves out-of-memory diagnostic");
    allocs.fail_at_alloc = 0;
    if (out != NULL) {
        renderer->string_free(renderer, out);
        out = NULL;
    }
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "renderer recreate after chart allocation failure");
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\n\nA,10\nB,20\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart preserves blank lines inside fence");
    fails += expect(strstr(out, "chart: no data") == NULL &&
                    strstr(out, "A ") != NULL &&
                    strstr(out, "B ") != NULL &&
                    strstr(out, "\n\nnext") != NULL,
                    "blank chart line does not terminate chart fence");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "foo\n```mdf-bar-chart\nA,1\n```\nbar\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "paragraph-adjacent chart fence render");
    fails += expect(strstr(out, "foo ") == NULL &&
                    strstr(out, "\nbar") != NULL &&
                    strstr(out, "\n bar") == NULL &&
                    strstr(out, "A ") != NULL,
                    "paragraph-adjacent chart does not share text line or leak following space");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\n    ```,2\nA,1\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart preserves indented backtick labels");
    fails += expect(strstr(out, "```") != NULL &&
                    strstr(out, "A ") != NULL &&
                    strstr(out, "next") != NULL,
                    "four-space indented chart rows do not close the fence");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\n```Label,10\nA,1\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart preserves unindented backtick labels");
    fails += expect(strstr(out, "Label") != NULL &&
                    strstr(out, "A ") != NULL &&
                    strstr(out, "next") != NULL,
                    "chart row beginning with backticks does not close the fence");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1\n````\nAfter\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart accepts longer closing fence");
    fails += expect(strstr(out, "A ") != NULL &&
                    strstr(out, "\nAfter") != NULL &&
                    strstr(out, "````") == NULL,
                    "longer chart closing fence closes chart and preserves following text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1\nOops,no\nB,2\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart skips malformed rows");
    fails += expect(strstr(out, "A ") != NULL &&
                    strstr(out, "B ") != NULL &&
                    strstr(out, "Oops") == NULL &&
                    strstr(out, "66.7%") != NULL,
                    "malformed chart row does not truncate subsequent valid rows");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,10\nB,20\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart emits at EOF without closing fence");
    fails += expect(strstr(out, "A ") != NULL &&
                    strstr(out, "B ") != NULL &&
                    strstr(out, "66.7%") != NULL,
                    "unclosed chart fence renders collected rows");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nNanRow,nan\nInfRow,inf\nFinite,10\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart rejects non-finite values");
    fails += expect(strstr(out, "Finite") != NULL &&
                    strstr(out, "100.0%") != NULL &&
                    strstr(out, "NanRow") == NULL &&
                    strstr(out, "InfRow") == NULL &&
                    strstr(out, "nan") == NULL &&
                    strstr(out, "inf") == NULL,
                    "non-finite chart rows are skipped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nHuge,1e100\nSmall,1\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart renders huge finite values");
    fails += expect(strstr(out, "Huge") != NULL &&
                    strstr(out, "1e+100") != NULL &&
                    strstr(out, "100.0%") != NULL,
                    "huge finite chart value avoids integer formatting overflow");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1e308\nB,1e308\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "horizontal chart renders huge finite totals");
    fails += expect(strstr(out, "50.0%") != NULL &&
                    strstr(out, "nan") == NULL &&
                    strstr(out, "-nan") == NULL,
                    "huge finite chart totals keep finite percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 40;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "width 40 huge horizontal chart renderer create");
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nHuge,1.7976931348623157e308\nSmall,1\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "width 40 huge horizontal chart render");
    fails += expect(strstr(out, "1.79769e+308") != NULL &&
                    max_ansi_visible_line_cols(out) <= 40,
                    "horizontal chart reserves actual huge value width");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default chart renderer restore after width 40 huge chart");
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,sort=ascending\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "sorted ascending chart render");
    fails += expect(strstr(out, "C ") != NULL && strstr(out, "A ") != NULL && strstr(out, "B ") != NULL &&
                    strstr(out, "C ") < strstr(out, "A ") && strstr(out, "A ") < strstr(out, "B "),
                    "sorted ascending chart orders by value");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,colored-bars=off\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "mono chart option render");
    fails += expect(strstr(out, "\342\226\210") != NULL && strstr(out, "57.1%") != NULL,
                    "mono chart option keeps bar output and percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,disable_percentage\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "disable percentage chart option render");
    fails += expect(strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "A ") != NULL &&
                    strstr(out, "10") != NULL &&
                    strstr(out, "%") == NULL,
                    "disable_percentage hides horizontal chart percentages but keeps values");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,disable_percent\nA,10\nB,20\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "disable percent alias chart option render");
    fails += expect(strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "%") == NULL,
                    "disable_percent alias hides horizontal chart percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,disable_percentage=false\nA,10\nB,20\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "disable percentage false chart option render");
    fails += expect(strstr(out, "66.7%") != NULL,
                    "disable_percentage=false preserves horizontal chart percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    {
        mdf_options styled_opts;
        mdf *styled_renderer;
        char *styled_out;

        styled_opts = opts;
        styled_opts.boring = 0;
        styled_renderer = NULL;
        styled_out = NULL;
        st = mdf_create(MDF_FORMAT_ANSI, &styled_opts, &styled_renderer);
        fails += expect(st == MDF_OK && styled_renderer != NULL, "styled chart color renderer create");
        if (styled_renderer != NULL) {
            st = styled_renderer->render_cstr(styled_renderer, "```mdf-bar-chart\nA,10\nB,20\nC,5\n```\n", &styled_out);
            fails += expect(st == MDF_OK && styled_out != NULL, "styled chart color render");
            fails += expect(styled_out != NULL && ansi_chart_label_matches_bar_style(styled_out, "A"),
                            "horizontal chart labels match row bar color");
            fails += expect(styled_out != NULL && ansi_chart_texts_match_style(styled_out, "A", "10") &&
                            ansi_chart_texts_match_style(styled_out, "A", " 28.6%"),
                            "horizontal chart values and percentages match row bar color");
            styled_renderer->string_free(styled_renderer, styled_out);
            styled_out = NULL;
            st = styled_renderer->render_cstr(styled_renderer, "```mdf-bar-chart,colored-bars=off\nA,10\nB,20\nC,5\n```\n", &styled_out);
            fails += expect(st == MDF_OK && styled_out != NULL, "styled mono chart color render");
            fails += expect(styled_out != NULL && ansi_chart_label_matches_bar_style(styled_out, "A"),
                            "mono chart option keeps labels matched to mono bar color");
            styled_renderer->string_free(styled_renderer, styled_out);
            styled_out = NULL;
            st = styled_renderer->render_cstr(styled_renderer, "```mdf-tile-chart\nBuild,40\nTest,25\nShip,10\n```\n", &styled_out);
            fails += expect(st == MDF_OK && styled_out != NULL, "styled tile chart render");
            fails += expect(styled_out != NULL &&
                            strstr(styled_out, "\033[30;") != NULL &&
                            strstr(styled_out, "Build") != NULL &&
                            strstr(styled_out, "53.3%") != NULL &&
                            strstr(styled_out, "■") != NULL,
                            "styled tile chart renders separated in-bar text and legend");
            fails += expect(styled_out != NULL && ansi_chart_texts_match_style(styled_out, "■", "40"),
                            "styled tile chart legend values match segment color");
            styled_renderer->string_free(styled_renderer, styled_out);
            styled_out = NULL;
            st = styled_renderer->render_cstr(styled_renderer,
                "```mdf-tile-chart\nS01,4\nS02,5\nS03,6\nS04,7\nS05,8\nS06,9\nS07,10\nS08,11\n```\n",
                &styled_out);
            fails += expect(st == MDF_OK && styled_out != NULL, "styled dense tile chart render");
            fails += expect(styled_out != NULL && strstr(styled_out, "\033[4m") == NULL,
                            "styled dense tile chart color cycle does not underline legend entries");
            styled_renderer->string_free(styled_renderer, styled_out);
            styled_renderer->destroy(styled_renderer);
        }
    }
    st = renderer->render_cstr(renderer, "```mdf-vertical-bar-chart\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "vertical chart fence render");
    fails += expect(strstr(out, "\342\224\224") != NULL &&
                    strstr(out, "\342\224\200") != NULL &&
                    strstr(out, "\342\226\210") != NULL,
                    "vertical chart renders axis and bars");
    fails += expect(ansi_visible_contains(out, "20 \342\224\244") &&
                    ansi_visible_contains(out, "10 \342\224\244") &&
                    ansi_visible_contains(out, " 0 \342\224\224"),
                    "vertical chart renders y-axis values and ticks");
    fails += expect(max_ansi_visible_line_cols(out) >= 48,
                    "vertical chart expands small datasets toward content width");
    fails += expect(chart_tick_label_has_marker_above(out, 'B', "\342\226\210"),
                    "vertical chart centers tick labels under bars");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-line-chart\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "line chart fence falls back to code render");
    fails += expect(strstr(out, "mdf-line-chart") == NULL &&
                    strstr(out, "A,10") != NULL &&
                    strstr(out, "\342\227\217") == NULL,
                    "removed line chart syntax is ordinary fenced code");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-tile-chart\nBuild,40\nTest,25\nShip,10\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "tile chart fence render");
    fails += expect(strstr(out, "Build") != NULL &&
                    strstr(out, "53.3%") != NULL &&
                    strstr(out, "Test") != NULL &&
                    strstr(out, "33.3%") != NULL &&
                    strstr(out, "Build 40") != NULL &&
                    strstr(out, "Test 25") != NULL,
                    "tile chart renders separated labels, percentages, and raw-value legend");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-tile-chart\nA,0\nB,0\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "zero-total tile chart render");
    fails += expect(strstr(out, "A 0") != NULL &&
                    strstr(out, "B 0") != NULL &&
                    strstr(out, "0.0%") == NULL,
                    "zero-total tile chart keeps legend without fake in-bar percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow chart renderer create");
    st = renderer->render_cstr(renderer,
        "```mdf-horizontal-bar-chart\nLong label one,1000\nLong label two,500\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow horizontal chart render");
    fails += expect(max_visible_line_cols(out) <= 20, "narrow horizontal chart scales to configured width");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 8;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "very narrow utf8 chart renderer create");
    st = renderer->render_cstr(renderer,
        "```mdf-horizontal-bar-chart\n\303\205ngstr\303\266m,10\nBeta,5\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "very narrow utf8 chart render");
    fails += expect(strstr(out, "\303\205") != NULL &&
                    strstr(out, "\303\033") == NULL &&
                    strstr(out, "\303 ") == NULL,
                    "narrow chart labels truncate on utf8 boundaries");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow chart renderer restore after utf8 chart");
    st = renderer->render_cstr(renderer,
        "```mdf-tile-chart\nA,1\nB,2\nC,3\nD,4\nE,5\nF,6\nG,7\nH,8\nI,9\nJ,10\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow tile chart render");
    fails += expect(max_visible_line_cols(out) <= 20, "narrow tile chart scales to configured width");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 60;
    opts.margin_left = 8;
    opts.margin_right = 6;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "margin chart renderer create");
    st = renderer->render_cstr(renderer,
        "```mdf-bar-chart,sort\nBuild,40\nTest,25\nShip,10\n```\n"
        "```mdf-vertical-bar-chart\nA,1\nB,3\nC,2\n```\n"
        "```mdf-tile-chart\nBuild,40\nTest,25\nShip,10\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "margin chart render");
    fails += expect(out != NULL && raw_ansi_lines_have_margin_before_styles(out, opts.margin_left),
                    "charts honor ANSI left margin before styles");
    renderer->string_free(renderer, out);
    out = NULL;
    {
        static const struct {
            int width;
            int left;
            int right;
        } chart_layout_cases[] = {
            {60, 0, 0},
            {80, 0, 0},
            {60, 20, 20},
            {72, 4, 8},
            {96, 20, 20}
        };
        static const char *quote_labels[] = {"x", "y", "z"};
        static const char *list_labels[] = {"@", "~", "?"};
        static const char *quoted_list_labels[] = {"{", "}", "="};
        const char *container_chart_src =
            "Quoted chart.\n\n"
            "> ```mdf-bar-chart\n"
            "> x,40\n"
            "> y,25\n"
            "> z,10\n"
            "> ```\n\n"
            "Listed chart.\n\n"
            "- item:\n\n"
            "  ```mdf-bar-chart\n"
            "  @,40\n"
            "  ~,25\n"
            "  ?,10\n"
            "  ```\n\n"
            "Quoted list chart.\n\n"
            "> - item:\n"
            ">\n"
            ">   ```mdf-bar-chart\n"
            ">   {,40\n"
            ">   },25\n"
            ">   =,10\n"
            ">   ```\n";
        size_t ci;

        for (ci = 0; ci < sizeof(chart_layout_cases) / sizeof(chart_layout_cases[0]); ci++) {
            opts.width = chart_layout_cases[ci].width;
            opts.margin_left = chart_layout_cases[ci].left;
            opts.margin_right = chart_layout_cases[ci].right;
            opts.boring = 0;
            renderer->destroy(renderer);
            renderer = NULL;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
            fails += expect(st == MDF_OK && renderer != NULL, "chunked container chart renderer create");
            if (renderer != NULL) {
                st = render_chunked_cstr(renderer, container_chart_src, 3, &out);
                fails += expect(st == MDF_OK && out != NULL, "chunked container chart render");
                fails += expect(out != NULL &&
                                strstr(out, "```") == NULL &&
                                strstr(out, "> >") == NULL &&
                                strstr(out, "key,value") == NULL,
                                "chunked container chart does not leak fences or nested quote prefixes");
                fails += expect(out != NULL && raw_ansi_lines_have_margin_before_styles(out, opts.margin_left),
                                "chunked container charts honor margin before styles");
                fails += expect(out != NULL && max_ansi_visible_line_cols(out) <= (size_t)opts.width,
                                "chunked container charts stay within configured width");
                fails += expect(out != NULL &&
                                ansi_visible_labels_share_col(out, quote_labels, sizeof(quote_labels) / sizeof(quote_labels[0])),
                                "quoted chunked chart labels share a visible column");
                fails += expect(out != NULL &&
                                ansi_visible_labels_share_col(out, list_labels, sizeof(list_labels) / sizeof(list_labels[0])),
                                "listed chunked chart labels share a visible column");
                fails += expect(out != NULL &&
                                ansi_visible_labels_share_col(out, quoted_list_labels, sizeof(quoted_list_labels) / sizeof(quoted_list_labels[0])),
                                "quoted-list chunked chart labels share a visible column");
                fails += expect(out != NULL &&
                                ansi_visible_has_exact_trimmed_line(out, ">"),
                                "quoted-list chunked chart preserves blank quote line before chart");
                free(out);
                out = NULL;
            }
        }
    }
    opts.width = 0;
    opts.margin_left = 0;
    opts.margin_right = 0;
    opts.boring = 1;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore after chart tests");
    {
        emission_probe probe;

        memset(&probe, 0, sizeof(probe));
        renderer->destroy(renderer);
        renderer = NULL;
        opts.write_trace.userdata = &probe;
        opts.write_trace.emit = emission_probe_trace_emit;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
        fails += expect(st == MDF_OK && renderer != NULL, "chart trace renderer create");
        if (st == MDF_OK && renderer != NULL) {
            mdf_sink trace_sink;
            chunk_source trace_src;
            mdf_source trace_source;

            trace_src.src = "```mdf-bar-chart\nA,10\nB,20\n```\n";
            trace_src.len = strlen(trace_src.src);
            trace_src.off = 0;
            trace_src.chunk = 5;
            trace_source.userdata = &trace_src;
            trace_source.read = chunk_read;
            trace_sink.userdata = &probe;
            trace_sink.write = emission_probe_sink_write;
            st = renderer->render(renderer, &trace_source, &trace_sink);
            fails += expect(st == MDF_OK, "chart trace render succeeds");
            fails += expect(chunk_logs_equal(&probe.sink, &probe.trace),
                            "chart trace events match sink writes one-to-one");
        }
        opts.write_trace.userdata = NULL;
        opts.write_trace.emit = NULL;
        renderer->destroy(renderer);
        renderer = NULL;
        st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
        fails += expect(st == MDF_OK && renderer != NULL, "default renderer restore after chart trace");
    }
    st = renderer->render_cstr(renderer, "* If `.golangci.yml` exists:\n\n```yaml\nversion: \"2\"\nlinters:\n  disable:\n    - errcheck\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list fenced code render");
    fails += expect(strstr(out, "yamlversion") == NULL && strstr(out, "  version: \"2\"\n  linters:\n    disable:\n      - errcheck") != NULL, "list fenced code strips info and indents");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "    code\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "indented code render");
    fails += expect(strstr(out, "    code") == NULL && strstr(out, "code") != NULL && strstr(out, "\n\nnext") != NULL, "indented code output");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* [Overview](#overview)\n    *   [Philosophy](#philosophy)\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "indented nested list render");
    fails += expect(strstr(out, "  - ") != NULL && strstr(out, "Philosophy") != NULL &&
                    strstr(out, "*   [Philosophy]") == NULL,
                    "indented nested list parsed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* A\n\nB\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blank boundary render");
    fails += expect(strstr(out, "- A\n\nB") != NULL, "list blank boundary preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Micro-reviews are **blameless** by design:\n- no naming/shaming\n- no interruptions\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "paragraph list boundary render");
    fails += expect(strstr(out, "by design:\n\n- no naming/shaming\n- no interruptions") != NULL, "paragraph list boundary closes paragraph");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- **Outcomes**  \nBecause it matters.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list hard break render");
    fails += expect(strstr(out, "- Outcomes\n  Because it matters.") != NULL, "list hard break indents continuation");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* **Hypothesis CoA** - Capture the planned course of action in a\n  narrative format, ideally structured as a three-phase progression:\n  *Initially, thereafter, and finally*  \n  This storytelling form improves clarity, alignment, and memory\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list hard break source-indented render");
    fails += expect(strstr(out, "finally\n  This storytelling form") != NULL &&
                    strstr(out, "finally\n   This storytelling") == NULL,
                    "list hard break does not duplicate source indent");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Quality: what \"good\" means under real operating conditions  \n\n**Course of Action (CoA)** *(short planning narrative; not a detailed plan)*:\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "hard break blank render");
    fails += expect(strstr(out, "conditions\n\nCourse of Action") != NULL && strstr(out, "conditions\n\n\nCourse of Action") == NULL, "hard break blank collapses paragraph spacing");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.boring = 0;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow table renderer create");
    st = renderer->render_cstr(renderer, "| Col A | Col B |\n| --- | --- |\n| Longer cell that should wrap at narrower widths | B3 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow table render");
    fails += expect(strstr(out, "| --- | --- |") == NULL && strstr(out, "\342\224\214") != NULL && strstr(out, "wrap at") != NULL && strstr(out, "narrower") != NULL, "narrow table consumes markdown and wraps cells");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 100;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "wide styled table renderer create");
    st = renderer->render_cstr(renderer, "| Name | Description |\n| --- | --- |\n| mixed long | plain text before *italic phrase* then **bold phrase** then ***combined phrase*** with enough words to wrap |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "wide styled table render");
    fails += expect(strstr(out, "with\nenough") == NULL &&
                    strstr(out, "enough") != NULL &&
                    strstr(out, "\342\224\202") != NULL,
                    "wide styled table cells are wrapped only by table renderer");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow table renderer restore");
    st = renderer->render_cstr(renderer, "> | A | B |\n> | --- | --- |\n> | 1 | 2 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted table render");
    fails += expect(strstr(out, "\033[90m>\033[0m \033[90m\342\224\214") != NULL && strstr(out, "> | --- | --- |") == NULL, "quoted table renders with quote prefix");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- item\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list followed by table render");
    fails += expect(strstr(out, "item\n\n\033[90m\342\224\214") != NULL, "list followed by table keeps blank boundary");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| A | B |\n| --- | --- |\n| 1 | 2 |\nafter table\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "table followed by paragraph render");
    fails += expect(strstr(out, "after table") != NULL &&
                    strstr(out, "\n\nafter table") == NULL,
                    "table followed by paragraph replays terminating line without extra blank");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> | A | B |\n> | --- | --- |\n> | 1 | 2 |\n> after table\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted table followed by paragraph render");
    fails += expect(strstr(out, "after table") != NULL &&
                    strstr(out, "\n\nafter table") == NULL &&
                    strstr(out, "\n\n\033[90m>\033[0m after table") == NULL,
                    "quoted table followed by paragraph replays terminating line without extra blank");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| A | B |\n| 1 | 2 |\n| 3 | 4 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "headerless table render");
    fails += expect(strstr(out, "\342\224\214\342\224\200\342\224\200\342\224\200") != NULL && strstr(out, "| 1 | 2 |") == NULL, "headerless table consumes pipe rows");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "| x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | x | K | Z |\n"
        "| y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | y | L | Q |\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "wide headerless table render");
    fails += expect(strstr(out, "K") != NULL &&
                    strstr(out, "L") != NULL &&
                    strstr(out, "Z") == NULL &&
                    strstr(out, "Q") == NULL,
                    "wide headerless table drops cells past the supported column cap");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| City | Description |\n| --- | --- |\n| \346\235\261\344\272\254 | \346\227\245\346\234\254 \343\201\256 \351\246\226\351\203\275 |\n| \345\244\247\351\230\252 | \351\226\242\350\245\277 \343\201\256 \351\203\275\345\270\202 |\n| \344\272\254\351\203\275 | \346\255\264\345\217\262 \347\232\204 \343\201\252 \351\203\275\345\270\202 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "unicode table render");
    fails += expect(strstr(out, "\346\235\261\344\272\254\033[90m \342\224\202 \033[0m") != NULL &&
                    strstr(out, "\345\244\247\351\230\252\033[90m \342\224\202 \033[0m") != NULL &&
                    strstr(out, "\344\272\254\351\203\275\033[90m \342\224\202 \033[0m") != NULL,
                    "unicode table uses display width");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| Entity | Example | Long Example |\n| --- | --- | --- |\n| non-breaking | 10\302\240000 | values like 10\302\240000 and 20\302\240000 should remain coherent |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "entity narrow table render");
    fails += expect(strstr(out, "\342\224\214\342\224\200\342\224\200\342\224\200\342\224\254\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\254\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\220") != NULL &&
                    strstr(out, "\033[90m│ \033[0mn\033[90m │ \033[0m10\302\240000\033[90m │ \033[0mvalues\033[90m │\033[0m") != NULL,
                    "entity narrow table preserves non-breaking words when constrained");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| Left | Middle | Right |\n| --- | --- | --- |\n| a <x|y> | z | right |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "literal angle table render");
    fails += expect(strstr(out, "<x>") != NULL && strstr(out, "y>") != NULL && strstr(out, "a <x|y>") == NULL, "literal angle table cell closes dangling autolink text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "A | B | C\n1 | 2 | 3\nlong left value | long middle value that wraps in narrow output | long right value\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "no-edge headerless rows render");
    fails += expect(strstr(out, "A | B | C 1 | 2 | 3") != NULL && strstr(out, "\342\224\214") == NULL,
                    "no-edge headerless rows stay plain text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "A | B\n--- | ---\n1 | 2\nalpha | beta\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "no-edge header rows render");
    fails += expect(strstr(out, "A | B --- | --- 1 |") != NULL && strstr(out, "\342\224\234") == NULL,
                    "no-edge header rows stay plain text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "A | B |\n--- | --- |\n1 | 2 |\nalpha | beta |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "trailing-edge rows render");
    fails += expect(strstr(out, "A | B | --- | --- |") != NULL && strstr(out, "\342\224\234") == NULL,
                    "trailing-edge rows stay plain text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Autolinks:\n<https://example.com/path?x=1>\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow autolink render");
    fails += expect(strstr(out, "example.com/path?x=1") != NULL && strstr(out, "https://example.com/path?x=1") == NULL,
                    "narrow autolink strips scheme when it is the only way to fit");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "[ref1]: https://example.com \"Example\"\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow reflink-style url render");
    fails += expect(strstr(out, "[ref1]: https:\n//example.com\n\"Example\"") != NULL,
                    "narrow reflink-style url prefers scheme boundary split");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.boring = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "styled narrow renderer create");
    st = renderer->render_cstr(renderer, "This is [an example](http://example.com) inline link.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow wrapped link render");
    fails += expect(strstr(out, "an example\033[0m\n(\033[90mhttp://example.com\033[0m)\ninline link.") != NULL,
                    "styled wrapped fallback url keeps gray when word fits on wrapped line");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Inspired by the original [Agile Manifesto](https://agilemanifesto.org/), this framework defines a model.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow mid-line link label wrap render");
    fails += expect(strstr(out, "original \033[4m\033[1;34mAgile\033[0m\n\033[4m\033[1;34mManifesto\033[0m") != NULL,
                    "styled mid-line link label wraps at spaces instead of moving the full label");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "This is [label](https://e.co/xxxxx).\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow sentence-final wrapped link render");
    fails += expect(strstr(out, "label\033[0m\n(https://e.co/xxxxx)\n.") != NULL &&
                    strstr(out, "(\033[90mhttps://e.co/xxxxx\033[0m)") == NULL,
                    "sentence-final exact-fit fallback link stays plain before trailing punctuation");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
                               "**raw source of [TEST.md](https://raw.githubusercontent.com/mxstbr/markdown-test-file/master/TEST.md) for the deets!** ([this is the test file rendered](./TEST.md))\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow outer-paren link render");
    fails += expect(strstr(out, "rendered\033[0m\n(\033[90m./TEST.md\033[0m))") != NULL,
                    "wrapped outer-parenthesized fallback link keeps both closing parens on the wrapped line");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "A | [site](https://example.com)\nnot a table\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow mixed url word render");
    fails += expect(strstr(out, "site\033[0m\n(https://example.com\n) not a table") != NULL &&
                    strstr(out, "(\033[90mhttps://example.com\033[0m") == NULL,
                    "overlong mixed text and fallback url word follows Go split styling");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "[https://pkt.systems/centaur.md](https://pkt.systems/centaur.md)\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow url label render");
    fails += expect(strstr(out, "\033[4m\033[1;34mhttps://pkt.systems/centau\033[0m\n\033[4m\033[1;34mr.md\033[0m") != NULL,
                    "url-shaped link label keeps prefix chunk on the first wrapped line");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Inline HTML: <span class=\"note\">note</span> and <strong>strong</strong>.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow html render");
    fails += expect(strstr(out, "Inline HTML: <span\nclass=\"note\">note</s\npan> and\n<strong>strong</stro\nng>.") != NULL,
                    "html tags with spaces stream as plain wrapped text");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html shell renderer create");
    st = renderer->render_cstr(renderer, "# Title\n\nBody.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html shell render");
    fails += expect(out != NULL && strstr(out, "<!doctype html>") != NULL &&
                    strstr(out, "<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->") != NULL &&
                    strstr(out, "<html lang=\"en\">") != NULL &&
                    strstr(out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">") != NULL &&
                    strstr(out, "data:font/woff2") == NULL &&
                    strstr(out, "JetBrains Mono") == NULL &&
                    strstr(out, "font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,\"Liberation Mono\",\"Courier New\",monospace") != NULL &&
                    strstr(out, "--mdf-content-max-width:96ch;") != NULL &&
                    strstr(out, "<main class=\"mdf-document\">") != NULL &&
                    strstr(out, "<span class=\"mdf-heading\" style=\"color:rgb(0,205,0);font-weight:700;font-size:32pt;font-weight:700;--mdf-heading-indent:2ch;\"># Title</span>") != NULL &&
                    strstr(out, "<span style=\"color:rgb(220,220,220);\">Body.</span>") != NULL &&
                    strstr(out, "</main>\n</body>\n</html>\n") != NULL,
                    "library html shell does not embed or hardcode the cmdf font");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html title renderer create");
    st = mdf_set_html_title(renderer, "Manual & <Title>");
    fails += expect(st == MDF_OK, "html title setter succeeds before rendering");
    st = mdf_set_html_title(renderer, NULL);
    fails += expect(st == MDF_OK, "html title setter clears title before rendering");
    st = mdf_set_html_title(renderer, "Manual & <Title>");
    fails += expect(st == MDF_OK, "html title setter resets title before rendering");
    st = renderer->render_cstr(renderer, "Body.\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<title>Manual &amp; &lt;Title&gt;</title>") != NULL,
                    "library html title setter is escaped into the document shell");
    renderer->string_free(renderer, out);
    out = NULL;
    st = mdf_set_html_title(renderer, "Second Title");
    fails += expect(st == MDF_OK, "html title setter retitles reusable renderer after render");
    st = renderer->render_cstr(renderer, "Second body.\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<title>Second Title</title>") != NULL,
                    "library html reusable renderer uses retitled title");
    renderer->string_free(renderer, out);
    out = NULL;
    st = mdf_set_html_title(renderer, NULL);
    fails += expect(st == MDF_OK, "html title setter clears reusable renderer after render");
    st = renderer->render_cstr(renderer, "Default body.\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "<title>mdf</title>") != NULL,
                    "library html reusable renderer uses default title after clear");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    opts.html_font.family = "Example Mono";
    opts.html_font.regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
    opts.html_font.regular.data = test_font;
    opts.html_font.regular.data_len = sizeof(test_font);
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html user font renderer create");
    st = renderer->render_cstr(renderer, "Body.\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "@font-face{font-family:\"Example Mono\";src:url(data:font/woff2;base64,AQID)") != NULL &&
                    strstr(out, "font-family:\"Example Mono\",monospace") != NULL,
                    "library html uses caller-supplied woff2 font family and data");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    memset(&opts.html_font, 0, sizeof(opts.html_font));
    opts.html_font.family = "Callback Mono";
    opts.html_font.regular.format = MDF_HTML_FONT_FORMAT_TTF;
    opts.html_font.regular.read = test_ttf_read;
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html callback font renderer create");
    st = renderer->render_cstr(renderer, "Body.\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "@font-face{font-family:\"Callback Mono\";src:url(data:font/ttf;base64,BAUGBw==)") != NULL &&
                    strstr(out, "format('truetype')") != NULL,
                    "library html supports callback-supplied ttf font data");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    memset(&opts.html_font, 0, sizeof(opts.html_font));
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html shell renderer recreate after user font");
    st = renderer->render_cstr(renderer, "### Example: *lockd*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html heading emphasis render");
    fails += expect(strstr(out, "<span class=\"mdf-heading\" style=\"color:rgb(205,0,205);font-weight:700;font-size:15pt;font-weight:700;--mdf-heading-indent:4ch;\">### Example: lockd</span>") != NULL,
                    "html headings flatten inline emphasis styling like parityjudge");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "<https://example.com>\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html autolink render");
    fails += expect(strstr(out, "<a href=\"https://example.com\"><span style=\"color:rgb(59,156,255);font-weight:700;text-decoration:underline;\">https://example.com</span></a>") != NULL,
                    "html autolink emits anchor wrapper");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "[safe](https://example.com) [mail](mailto:user@example.com) [rel](/docs) [frag](#top) <schem://host/path>\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "html safe link schemes render");
    fails += expect(strstr(out, "href=\"https://example.com\"") != NULL &&
                    strstr(out, "href=\"mailto:user@example.com\"") != NULL &&
                    strstr(out, "href=\"/docs\"") != NULL &&
                    strstr(out, "href=\"#top\"") != NULL &&
                    strstr(out, "href=\"schem://host/path\"") != NULL,
                    "html safe link schemes remain active anchors");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Literal \342\226\210 block.\n\n```\n\342\226\210\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html literal block glyph render");
    fails += expect(strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "display:inline-block;width:1ch;height:1lh;line-height:1lh;vertical-align:top;background-color:rgb(") == NULL,
                    "html literal block glyphs are not rewritten outside chart output");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "[js](javascript:alert(1)) [case](JaVaScRiPt:alert(1)) [data](data:text/html,boom) [ctrl](java\tscript:alert(1)) [jsurl](javascript://example.com/%0aalert(1))\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "html unsafe link schemes render");
    fails += expect(strstr(out, "href=\"javascript:") == NULL &&
                    strstr(out, "href=\"JaVaScRiPt:") == NULL &&
                    strstr(out, "href=\"data:") == NULL &&
                    strstr(out, "href=\"java") == NULL &&
                    strstr(out, ">js</span>") != NULL &&
                    strstr(out, ">case</span>") != NULL &&
                    strstr(out, ">data</span>") != NULL &&
                    strstr(out, ">ctrl</span>") != NULL,
                    "html unsafe link schemes are not active anchors");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- [ ] task\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html task list render");
    fails += expect(strstr(out, "<span class=\"mdf-line\"><span class=\"mdf-prefix\"><span style=\"color:rgb(0,205,205);font-size:12pt;font-weight:700;\">-</span><span style=\"color:rgb(220,220,220);\"> [ ] </span></span><span class=\"mdf-content\"><span style=\"color:rgb(220,220,220);\">task</span>\n</main>") != NULL,
                    "html task list keeps parityjudge final wrapper state");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- first\n\n  second\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html indented continuation render");
    fails += expect(strstr(out, "<span class=\"mdf-line\"><span class=\"mdf-prefix\"><span style=\"color:rgb(220,220,220);\">  </span></span><span class=\"mdf-content\"><span style=\"color:rgb(220,220,220);\">second</span>\n</main>") != NULL,
                    "html keeps leading-space continuation lines in prefix wrappers");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
                               "350&nbsp;000 should not wrap.\n"
                               "120&#160;000 should not wrap.\n"
                               "42&#xA0;000 should not wrap.\n"
                               "Mixing inline text: A&nbsp;B and C&#160;D and E&#xA0;F.\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "html nbsp render");
    fails += expect(strstr(out, "350&amp;nbsp;000 should not wrap.") != NULL &&
                    strstr(out, "120&amp;#160;000 should not wrap.") != NULL &&
                    strstr(out, "42&amp;#xA0;000 should not wrap.") != NULL &&
                    strstr(out, "A B and C D and E F") != NULL,
                    "html prose escapes no-break number entities and decodes inline entities like parityjudge");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "AT&T and AT&amp;T\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html raw ampersand render");
    fails += expect(strstr(out, "AT&amp;T and AT&amp;T") != NULL,
                    "html escapes raw ampersands after inline entity decoding");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| A | B |\n| --- | --- |\n| 1 | 2 |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html table render");
    fails += expect(strstr(out, "<table class=\"mdf-table mdf-table-bordered\"><thead><tr><th style=\"text-align:left;\">A</th><th style=\"text-align:left;\">B</th></tr></thead><tbody><tr><td style=\"text-align:left;\">1</td><td style=\"text-align:left;\">2</td></tr></tbody></table>") != NULL,
                    "html table renders bordered table structure");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart render");
    fails += expect(strstr(out, "<div class=\"mdf-chart-block\" style=\"width:100%;text-align:center;\"><span class=\"mdf-chart-box\" style=\"display:inline-block;text-align:left;\">") != NULL &&
                    strstr(out, "<div class=\"mdf-chart-block\" style=\"text-align:center;\">") == NULL &&
                    strstr(out, "<div class=\"mdf-chart-block\" style=\"display:table;margin-left:auto;margin-right:auto;text-align:left;\">") == NULL &&
                    strstr(out, "<span class=\"mdf-line mdf-chart-line\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\"") != NULL &&
                    strstr(out, "<span class=\"mdf-content\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\">") != NULL &&
                    strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "background-color:rgb(") != NULL &&
                    strstr(out, "57.1%") != NULL &&
                    strstr(out, "color:rgb(") != NULL,
                    "html chart preserves themed styled chart output without prose wrapping");
    fails += expect(strstr(out, ">\342\226\210\342\226\210") != NULL &&
                    strstr(out, "\342\226\210</span>") != NULL,
                    "html chart paints full block background while preserving glyph text");
    fails += expect(strstr(out, "display:inline-block;height:1lh;line-height:1lh;vertical-align:top;white-space:pre;color:transparent;background-color:rgb(") != NULL &&
                    strstr(out, "display:inline-block;width:1ch;height:1lh;line-height:1lh;vertical-align:top;color:inherit;-webkit-text-fill-color:transparent;") != NULL &&
                    strstr(out, "ch;height:1lh;line-height:1lh;vertical-align:top;white-space:pre;overflow:hidden;color:transparent;background-color:rgb(") == NULL,
                    "html chart block cells fill row height while axis glyphs remain one-cell spans");
    fails += expect(html_chart_label_matches_bar_style(out, ">A</span>"),
                    "html chart labels match row bar color");
    fails += expect(html_chart_texts_match_style(out, ">A</span>", "10 28.6%"),
                    "html chart values and percentages match row bar color");
    fails += expect(strstr(out, "<span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\"><span style=\"color:rgb(") != NULL,
                    "html horizontal chart trims ANSI centering prefix");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart,disable_percentage\nA,10\nB,20\nC,5\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html disable percentage chart option render");
    fails += expect(strstr(out, "<div class=\"mdf-chart-block\"") != NULL &&
                    strstr(out, "\342\226\210") != NULL &&
                    strstr(out, ">10</span>") != NULL &&
                    strstr(out, "28.6%") == NULL &&
                    strstr(out, "57.1%") == NULL &&
                    strstr(out, "14.3%") == NULL,
                    "html disable_percentage hides horizontal chart percentages but keeps values");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
                               "```mdf-vertical-bar-chart\n"
                               "Quarter,value\n"
                               "Q1,12\n"
                               "Q2,19\n"
                               "Q3,14\n"
                               "Q4,27\n"
                               "```\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "html vertical chart axis regression render");
    fails += expect(out != NULL && html_vertical_chart_axis_columns_aligned(out),
                    "html vertical chart keeps wide numeric tick rows aligned");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
                               "> ```mdf-vertical-bar-chart\n"
                               "> Quote Build,4\n"
                               "> Quote Test,7\n"
                               "> Quote Ship,3\n"
                               "> ```\n"
                               "\n"
                               "> ```mdf-tile-chart\n"
                               "> Quote Build,40\n"
                               "> Quote Test,25\n"
                               "> Quote Ship,10\n"
                               "> ```\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "html quoted chart geometry render");
    fails += expect(html_vertical_chart_axis_columns_aligned(out),
                    "html quoted vertical chart keeps y-axis rows aligned after quote prefix");
    fails += expect(strstr(out, "display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"display:inline-block;white-space:pre;overflow-wrap:normal;text-align:left;\"><span style=\"color:rgb(0,205,0);background-color:rgb(0,205,0);display:inline-block;height:1lh;line-height:1lh;vertical-align:top;font-weight:700;\">") != NULL &&
                    strstr(out, "display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"display:inline-block;white-space:pre;overflow-wrap:normal;text-align:left;\"><span style=\"color:rgb(220,220,220);\"> </span><span style=\"color:rgb(0,205,0);background-color:rgb(0,205,0);") == NULL,
                    "html quoted tile chart trims unstyled leading tile padding");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
                               "- Charts in a list:\n"
                               "\n"
                               "  ```mdf-vertical-bar-chart\n"
                               "  Quote Build,4\n"
                               "  Quote Test,7\n"
                               "  Quote Ship,3\n"
                               "  ```\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "html list vertical chart geometry render");
    fails += expect(html_vertical_chart_axis_columns_aligned(out),
                    "html list vertical chart keeps y-axis rows aligned");
    renderer->string_free(renderer, out);
    out = NULL;
    {
        mdf_options boring_html_opts;
        mdf *boring_html_renderer;
        char *boring_html_out;

        boring_html_opts = opts;
        boring_html_opts.boring = 1;
        boring_html_renderer = NULL;
        boring_html_out = NULL;
        st = mdf_create(MDF_FORMAT_HTML, &boring_html_opts, &boring_html_renderer);
        fails += expect(st == MDF_OK && boring_html_renderer != NULL, "boring html chart renderer create");
        if (boring_html_renderer != NULL) {
            st = boring_html_renderer->render_cstr(boring_html_renderer, "```mdf-bar-chart\nA,10\nB,20\n```\n", &boring_html_out);
            fails += expect(st == MDF_OK && boring_html_out != NULL, "boring html chart render");
            fails += expect(boring_html_out != NULL &&
                            strstr(boring_html_out, "\342\226\210") != NULL &&
                            strstr(boring_html_out, "color:rgb(205,0,205)") == NULL &&
                            strstr(boring_html_out, "color:rgb(59,156,255)") == NULL &&
                            strstr(boring_html_out, "background-color:rgb(205,0,205)") == NULL &&
                            strstr(boring_html_out, "background-color:rgb(59,156,255)") == NULL &&
                            strstr(boring_html_out, "background-color:rgb(0,0,0)") != NULL,
                            "boring html chart uses black boring output instead of themed ANSI colors");
            boring_html_renderer->string_free(boring_html_renderer, boring_html_out);
            boring_html_renderer->destroy(boring_html_renderer);
        }
    }
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\n\nA,10\nB,20\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart preserves blank lines inside fence");
    fails += expect(strstr(out, "chart: no data") == NULL &&
                    strstr(out, ">A</span>") != NULL &&
                    strstr(out, ">B</span>") != NULL &&
                    strstr(out, "next") != NULL,
                    "html blank chart line does not terminate chart fence");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,10\nB,20\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart emits at EOF without closing fence");
    fails += expect(strstr(out, ">A</span>") != NULL &&
                    strstr(out, ">B</span>") != NULL &&
                    strstr(out, "66.7%") != NULL,
                    "html unclosed chart fence renders collected rows");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\n```Label,10\nA,1\n```\n\nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart preserves unindented backtick labels");
    fails += expect(strstr(out, "Label") != NULL &&
                    strstr(out, ">A</span>") != NULL &&
                    strstr(out, "next") != NULL,
                    "html chart row beginning with backticks does not close the fence");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1\n````\nAfter\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart accepts longer closing fence");
    fails += expect(strstr(out, ">A</span>") != NULL &&
                    strstr(out, "After") != NULL &&
                    strstr(out, "````") == NULL,
                    "html longer chart closing fence closes chart and preserves following text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1\nOops,no\nB,2\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart skips malformed rows");
    fails += expect(strstr(out, ">A</span>") != NULL &&
                    strstr(out, ">B</span>") != NULL &&
                    strstr(out, "Oops") == NULL &&
                    strstr(out, "66.7%") != NULL,
                    "html malformed chart row does not truncate subsequent valid rows");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nNanRow,nan\nInfRow,inf\nFinite,10\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart rejects non-finite values");
    fails += expect(strstr(out, "Finite") != NULL &&
                    strstr(out, "100.0%") != NULL &&
                    strstr(out, "NanRow") == NULL &&
                    strstr(out, "InfRow") == NULL &&
                    strstr(out, "nan") == NULL &&
                    strstr(out, "inf") == NULL,
                    "html non-finite chart rows are skipped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nHuge,1e100\nSmall,1\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart renders huge finite values");
    fails += expect(strstr(out, "Huge") != NULL &&
                    strstr(out, "1e+100") != NULL &&
                    strstr(out, "100.0%") != NULL,
                    "html huge finite chart value avoids integer formatting overflow");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-bar-chart\nA,1e308\nB,1e308\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html chart renders huge finite totals");
    fails += expect(strstr(out, "50.0%") != NULL &&
                    strstr(out, "nan") == NULL &&
                    strstr(out, "-nan") == NULL,
                    "html huge finite chart totals keep finite percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Before chart.\n\n```mdf-bar-chart\nA,10\nB,20\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html prose before chart render");
    if (out != NULL) {
        const char *before = strstr(out, "Before chart.");
        const char *chart_block = strstr(out, "<div class=\"mdf-chart-block\"");
        const char *before_close = before != NULL ? strstr(before, "</span>") : NULL;

        fails += expect(before != NULL &&
                        chart_block != NULL &&
                        before_close != NULL &&
                        before_close < chart_block,
                        "html chart block opens after preceding prose line closes");
    } else {
        fails += expect(0, "html chart block opens after preceding prose line closes");
    }
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-tile-chart\nBuild,40\nTest,25\nShip,10\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html tile chart render");
    fails += expect(strstr(out, "background-color:rgb(") != NULL &&
                    strstr(out, "Build") != NULL &&
                    strstr(out, "53.3%") != NULL &&
                    strstr(out, "\342\226\210") != NULL &&
                    strstr(out, "■") != NULL &&
                    strstr(out, "40") != NULL,
                    "html tile chart preserves printable block fills, separated in-bar text, and legend");
    fails += expect(strstr(out, "<span class=\"mdf-line mdf-chart-line\" style=\"display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"") != NULL &&
                    strstr(out, "<span class=\"mdf-content\" style=\"display:inline-block;white-space:pre;overflow-wrap:normal;text-align:left;\">") != NULL,
                    "html tile chart centers variable-width rows inside chart box");
    fails += expect(out != NULL && html_tile_chart_first_rows_are_adjacent(out),
                    "html tile chart keeps label and percentage rows contiguous");
    fails += expect(strstr(out, "background-color:rgb(") != NULL &&
                    strstr(out, "display:inline-block;height:1lh;line-height:1lh;vertical-align:top;") != NULL,
                    "html tile chart background spans fill row height");
    fails += expect(html_chart_texts_match_style(out, "■", ">40</span>"),
                    "html tile chart legend values match segment color");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> ```mdf-tile-chart\n> Build,40\n> Test,25\n> Ship,10\n> ```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html quoted tile chart render");
    fails += expect(out != NULL && html_quoted_chart_prefix_is_not_chart_line(out),
                    "html quoted tile chart does not center quote marker as a chart row");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "```mdf-tile-chart\nA,0\nB,0\n```\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html zero-total tile chart render");
    fails += expect(strstr(out, ">A</span><span style=\"color:rgb(") != NULL &&
                    strstr(out, ">B</span><span style=\"color:rgb(") != NULL &&
                    strstr(out, ">0</span>") != NULL &&
                    strstr(out, "0.0%") == NULL,
                    "html zero-total tile chart keeps legend without fake in-bar percentages");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "```mdf-tile-chart\nS01,4\nS02,5\nS03,6\nS04,7\nS05,8\nS06,9\nS07,10\nS08,11\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "html dense tile chart render");
    fails += expect(out != NULL && strstr(out, "<main") != NULL &&
                    strstr(strstr(out, "<main"), "text-decoration:underline;") == NULL,
                    "html dense tile chart color cycle does not underline legend entries");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "```mdf-tile-chart\nBuild,40\nTest,25\nShip,10\n```\n\n"
        "```mdf-bar-chart\nA,10\nB,20\n```\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "html mixed chart kind render");
    fails += expect(out != NULL &&
                    strstr(out, "<span class=\"mdf-line mdf-chart-line\" style=\"display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"") != NULL &&
                    strstr(out, "<span class=\"mdf-line mdf-chart-line\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\"") != NULL,
                    "html tile row centering does not leak into following bar chart");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| Name | Description |\n| --- | --- |\n| mixed long | plain text before *italic phrase* then **bold phrase** then ***combined phrase*** with enough words to wrap |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html wide styled table render");
    fails += expect(strstr(out, "with\nenough") == NULL &&
                    strstr(out, "<td style=\"text-align:left;\">plain text before") != NULL &&
                    strstr(out, "combined phrase") != NULL,
                    "html styled table cell does not contain inner renderer newline");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "| A | B |\n| --- | --- |\n| 1 | 2 |\nafter html table\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html table followed by paragraph render");
    fails += expect(strstr(out, "after html table") != NULL &&
                    strstr(out, "</table>\n\n<span") == NULL,
                    "html table followed by paragraph replays terminating line without extra blank");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> | A | B |\n> | --- | --- |\n> | 1 | 2 |\n> after html table\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html quoted table followed by paragraph render");
    fails += expect(strstr(out, "</div><span class=\"mdf-line\"") != NULL &&
                    strstr(out, "after html table") != NULL,
                    "html quoted table followed by paragraph replays terminating line without separator newline");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> | [site](https://example.com) |\n> | --- |\n> | x |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html quoted table render");
    fails += expect(strstr(out, "<div class=\"mdf-table-block\"><span class=\"mdf-prefix\"><span style=\"color:rgb(127,127,127);\">&gt;</span><span style=\"color:rgb(220,220,220);\"> </span></span><table class=\"mdf-table mdf-table-bordered\"><thead><tr><th style=\"text-align:left;\"><a href=\"https://example.com\"><span style=\"color:rgb(0,205,205);font-weight:700;text-decoration:underline;\">site</span></a></th></tr></thead><tbody><tr><td style=\"text-align:left;\">x</td></tr></tbody></table>") != NULL,
                    "html quoted table keeps prefix wrapper and header link styling");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> - item\n>\n>   | A | B |\n>   | --- | --- |\n>   | x | y |\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "html quoted list table gap render");
    fails += expect(strstr(out, "<span style=\"color:rgb(127,127,127);\">&gt;</span>\n<div class=\"mdf-table-block\"><span class=\"mdf-prefix\"><span style=\"color:rgb(127,127,127);\">&gt;</span><span style=\"color:rgb(220,220,220);\"> </span><span style=\"color:rgb(220,220,220);\">  </span></span><table class=\"mdf-table mdf-table-bordered\">") != NULL,
                    "html flushes blank quote lines before quoted tables");
    renderer->string_free(renderer, out);
    out = NULL;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "styled narrow ansi recreate after html shell");
    st = renderer->render_cstr(renderer, "* `testdata/mdtest/TEST.md`.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow inline code render");
    fails += expect(strstr(out, "\033[36m-\033[0m \033[35mtestdata/mdtest/\033[0m\n  \033[35mTEST.md\033[0m.") != NULL,
                    "wrapped inline code keeps code style across list continuation");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 75;
    opts.margin_left = 20;
    opts.margin_right = 20;
    opts.theme_name = "everforest";
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "styled wide-margin inline code punctuation renderer create");
    st = renderer->render_cstr(renderer,
                               "Escaped punctuation should remain readable as literal text: "
                               "`*not emphasis*`, `[not a link]`, and `# not a heading`.\n",
                               &out);
    fails += expect(st == MDF_OK && out != NULL, "styled wide-margin inline code punctuation render");
    fails += expect(strstr(out, "\n                    .") == NULL &&
                    strstr(out, "\n                    \033[38;5;109m# not a heading") != NULL &&
                    strstr(out, "# not a heading\033[0m.") != NULL,
                    "wide-margin inline code keeps trailing punctuation attached inside margin");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    opts.margin_left = 0;
    opts.margin_right = 0;
    opts.theme_name = "default";
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "styled narrow ansi recreate after wide-margin inline code");
    st = renderer->render_cstr(renderer, "* `golangci-lint run ./...`\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow inline code bullet-only render");
    fails += expect(strstr(out, "\033[36m-\033[0m \n  \033[35mgolangci-lint run .\033[0m\n  \033[35m/...\033[0m") != NULL,
                    "wrapped inline code keeps the bullet marker line space before continuation");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* github.com/yuin/goldmark +\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow plain path render");
    fails += expect(strstr(out, "\033[36m-\033[0m github.com/yuin/gold\n  mark +") != NULL,
                    "path-like list words wrap at full width from the bullet prefix line");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* github.com/yuin/goldmark + extension.GFM (parity reference only; streaming is the long-term path)\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow dotted suffix path render");
    fails += expect(strstr(out, "\033[36m-\033[0m github.com/yuin/gold\n  mark +\n  extension.GFM\n  (parity reference\n  only; streaming is\n  the long-term\n  path)") != NULL,
                    "dotted words without a slash wrap before the word instead of splitting like a URL path");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* return a response/result struct as the first value.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow plain slash word render");
    fails += expect(strstr(out, "\033[36m-\033[0m return a\n  response/result\n  struct as the\n  first value.") != NULL,
                    "generic slash words wrap before the word instead of splitting after the pending space");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "(`type(scope): summary`).\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled narrow bracketed inline code render");
    fails += expect(strstr(out, "(type(scope):\n summary).") != NULL &&
                    strstr(out, "\033[35mtype(scope):") == NULL,
                    "overlong bracketed inline code follows Go mixed-word plain wrapping");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "### Example 2: *lingon* -- speed\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled heading emphasis continuation render");
    fails += expect(strstr(out, "\033[1;35m### \033[0m\033[1;35mExample 2:\033[0m\n    \033[1;35m\033[3m\033[34mlingon\033[0m\033[1;35m -- speed\033[0m") != NULL,
                    "wrapped heading emphasis avoids stray reset on continuation line");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "## Scope and relationship to organization-level frameworks\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled heading prefix overflow render");
    fails += expect(strstr(out, "\033[1;34morganization-level\033[0m\n   \033[1;34mframeworks\033[0m") != NULL,
                    "first heading word after prefix may overflow the prefix line before wrapping");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "**non-prescriptive—avoiding** early commitments\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled utf8 overlong word render");
    fails += expect(strstr(out, "\033[1m\033[1;37mnon-prescriptive") != NULL &&
                    strstr(out, "avo\033[0m\n\033[1m\033[1;37miding\033[0m early") != NULL,
                    "styled utf8 overlong words split by rune width instead of wrapping by available columns");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "**non-prescriptive**—avoiding early commitments\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled attached em dash word render");
    fails += expect(strstr(out, "\033[1m\033[1;37mnon-prescriptive\342\200\224avo\033[0m\n\033[1m\033[1;37miding\033[0m early") != NULL,
                    "single-word emphasis keeps an attached em-dash suffix in the styled wrapped word");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Outputs are **things we deliver**—features, services,\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled multiword em dash reserve render");
    fails += expect(strstr(out, "Outputs are \033[1m\033[1;37mthings\033[0m\n\033[1m\033[1;37mwe deliver\033[0m\342\200\224features,") != NULL,
                    "multiword emphasis does not reserve an attached em-dash suffix against its first wrapped word");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    opts.boring = 1;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "unicode chunk regression renderer create");
    {
        chunk_source src;
        mdf_source source;
        grow_sink grow;
        mdf_sink dyn_sink;
        const char *unicode_chunk_md;

        memset(&src, 0, sizeof(src));
        unicode_chunk_md =
            "| City | Description |\n"
            "| --- | --- |\n"
            "| 東京 | 日本 の 首都 |\n"
            "| 大阪 | 関西 の 都市 |\n";
        src.src = unicode_chunk_md;
        src.len = strlen(unicode_chunk_md);
        src.chunk = 3;
        source.userdata = &src;
        source.read = chunk_read;
        memset(&grow, 0, sizeof(grow));
        dyn_sink.userdata = &grow;
        dyn_sink.write = grow_write;
        st = parser->parse(parser, &source, renderer, &dyn_sink);
        fails += expect(st == MDF_OK && grow.buf != NULL, "unicode chunk regression parse");
        if (grow.buf != NULL) {
            strip_ansi_inplace(grow.buf);
            fails += expect(strstr(grow.buf, "│ 大   │ 関 の     │") != NULL,
                            "chunk-3 unicode table matches parityjudge v0.7.0 row loss");
        }
        grow_sink_free(&grow);
    }
    opts.boring = 1;
    opts.width = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore after table");
    st = renderer->render_cstr(renderer, "1. Bird\n1. McHale\n1. Parish\n\n3. Bird\n1. McHale\n8. Parish\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ordered list numbering render");
    fails += expect(strstr(out, "1. Bird\n2. McHale\n3. Parish\n\n4. Bird\n5. McHale\n6. Parish") != NULL, "ordered list numbering increments");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1. **Outcome:** More users set up their accounts through self-service  \n   **Signals of change:** Increased percentage of accounts created\n   without needing human assistance  \n   **Related outputs:** Redesigned onboarding experience and automated\n   support tools  \n\n2. **Outcome:** Fewer users require support for password issues  \n   **Signals of change:** Lower volume of support tickets and positive\n   user feedback  \n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ordered hard break blank render");
    fails += expect(strstr(out, "automated support tools\n\n2. Outcome") != NULL &&
                    strstr(out, "automated support tools\n\n\n2. Outcome") == NULL,
                    "ordered hard break blanks collapse once");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1. **Outcome:** Increased usage  \n   **Signals of change:** Higher adoption  \n   **Related outputs:** Launch guidance  \n\n\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "terminal list blank render");
    fails += expect(strstr(out, "Related outputs: Launch guidance\n\n") == NULL, "terminal list blanks are dropped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1. Bird\n1. McHale\n\ntext\n\n3. Bird\n1. McHale\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "ordered list numbering reset render");
    fails += expect(strstr(out, "1. Bird\n2. McHale\n\ntext\n\n3. Bird\n4. McHale") != NULL, "ordered list numbering resets after text");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "1. This is a list item with two paragraphs.\n\n    Vestibulum enim wisi.\nvitae, risus.\n\n2. Suspendisse id sem.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list paragraph continuation render");
    fails += expect(strstr(out, "1. This is a list item with two paragraphs.\n\n   Vestibulum enim wisi. vitae, risus.\n\n2. Suspendisse") != NULL, "list paragraph continuation preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* A list item with a blockquote:\n\n    > This is a blockquote\n    > inside a list item.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blockquote render");
    fails += expect(strstr(out, "  > This is a blockquote inside a list item.") != NULL && strstr(out, "> This is a blockquote > inside") == NULL, "list blockquote joins quoted lines");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- item\n\n    > Example:\n    > Intent text\n    >\n    > Tip text\n\n- next\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blockquote blank render");
    fails += expect(strstr(out, "  > Example: Intent text\n  >\n  > Tip text") != NULL && strstr(out, "\n> Tip text") == NULL, "list blockquote indent preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "> - Quoted list item:\n"
        ">\n"
        ">   | Quoted List | Value |\n"
        ">   | --- | --- |\n"
        ">   | short | value |\n"
        "\n"
        "## After Quoted List Table\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted list table boundary render");
    fails += expect(strstr(out, "\n>   ") != NULL &&
                    strstr(out, "\342\224\214") != NULL &&
                    strstr(out, "\n> \342\224\214") == NULL,
                    "quoted list table keeps nested indent");
    fails += expect(strstr(out, "After Quoted List Table") != NULL &&
                    strstr(out, "\n\033[90m>\033[0m \033[1;34m## \033[0m\033[1;34mAfter Quoted List Table") == NULL &&
                    strstr(out, "\n> ## After Quoted List Table") == NULL,
                    "quoted list table does not leak quote prefix");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "- [ ] Task item with a table:\n"
        "\n"
        "      | Task Column | Value |\n"
        "      | --- | --- |\n"
        "      | unchecked | task docs |\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "task list table indent render");
    fails += expect(strstr(out, "\n      ") != NULL &&
                    strstr(out, "\342\224\214") != NULL &&
                    strstr(out, "\n  \033[90m\342\224\214") == NULL,
                    "task list table keeps checkbox continuation indent");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* item\n\n\t> quote\n\t> more\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "tabbed list blockquote render");
    fails += expect(strstr(out, "- item\n\n  > quote more") != NULL, "tabbed list blockquote uses list indent");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 80;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "list blockquote wrap renderer create");
    st = renderer->render_cstr(renderer, "* item\n\n\t> *Intent: \"Prevent the fire from reaching the gas station\n\t> (end-state), without risking firefighter safety (constraint), in\n\t> order to maintain critical infrastructure and avoid civilian\n\t> casualties (purpose).\"*\n\n\t> Tip: Use the full intent statement as the title of the Outcome Card.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blockquote wrapped emphasis continuation render");
    fails += expect(strstr(out, "\n  > risking firefighter safety") != NULL && strstr(out, "\n  >   risking firefighter safety") == NULL, "list blockquote wrapped continuation uses quote indent");
    fails += expect(strstr(out, "\n    >\n  > Tip: Use") != NULL, "list blockquote blank marker before next quote");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* item\n\n\t> *Intent: \"Prevent the fire from reaching the gas station\n\t> (end-state), without risking firefighter safety (constraint), in\n\t> order to maintain critical infrastructure and avoid civilian\n\t> casualties (purpose).\"*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blockquote wrapped emphasis eof render");
    fails += expect(strstr(out, "\n    >\n") == NULL, "list blockquote eof does not synthesize blank marker");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.margin_left = 2;
    opts.boring = 1;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "margin list blockquote wrap renderer create");
    st = renderer->render_cstr(renderer,
        "  * **Purpose (IOT)** - The strategic or operational rationale behind\n"
        "    the intent; typically phrased as *\"in order to...\"*\n"
        "\n"
        "\t> **Example:**  \n"
        "\t> *Intent: \"Prevent the fire from reaching the gas station\n"
        "\t> (end-state), without risking firefighter safety (constraint), in\n"
        "\t> order to maintain critical infrastructure and avoid civilian\n"
        "\t> casualties (purpose).\"*\n"
        "\n"
        "\t> *Tip: Use the full intent statement as the title of the Outcome\n"
        "\t> Card. This helps keep constraints visible and prevents them from\n"
        "\t> silently transforming into assumptions or rigid requirements.*\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "margin nested list blockquote render");
    fails += expect(strstr(out, "\n      > Example:\n      > Intent:") != NULL &&
                    strstr(out, "\n        > Intent:") == NULL,
                    "margin nested list blockquote source lines keep quote marker column");
    fails += expect(strstr(out, "\n        >\n      > Tip:") != NULL &&
                    strstr(out, "\n        > Tip:") == NULL,
                    "margin nested list resumed blockquote keeps quote marker column");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 0;
    opts.margin_left = 0;
    opts.boring = 1;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore after list blockquote wrap");
    st = renderer->render_cstr(renderer, "> * one\n>   continued\n> * two\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted unordered continuation render");
    fails += expect(strstr(out, "> - one continued\n> - two") != NULL, "quoted unordered continuation joins before next item");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> Well, that is it folks.\n> - This is a list inside\n>   a blockquote.\n> - This is another item\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted paragraph list boundary render");
    fails += expect(strstr(out, "> Well, that is it folks.\n>\n> - This is a list inside a blockquote.\n> - This is another item") != NULL, "quoted paragraph list boundary keeps blank marker");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> - first\n> - second\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted list start render");
    fails += expect(strstr(out, "> - first\n> - second") != NULL && strstr(out, ">\n> - first") == NULL, "quoted list start has no leading blank marker");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 20;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow quoted paragraph list renderer create");
    st = renderer->render_cstr(renderer, "> Well, that is it folks.\n> - This is a list inside\n>   a blockquote.\n> - This is another item\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow quoted paragraph list render");
    fails += expect(strstr(out, "> Well, that is it\n> folks.\n>\n> - This is a list\n>   inside a\n>   blockquote.\n> - This is another\n>   item") != NULL && strstr(out, "\n  item") == NULL, "narrow quoted list continuations keep quote prefix");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 0;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer restore after quoted paragraph list");
    st = renderer->render_cstr(renderer, "> * one\n> \nnext\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted unordered blank exit render");
    fails += expect(strstr(out, "> - one\n\nnext") != NULL && strstr(out, "\n>\n>\nnext") == NULL, "quoted unordered blank exits quote");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> * one\n> \n> next\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted unordered blank continue render");
    fails += expect(strstr(out, "> - one\n>\n> next") != NULL && strstr(out, "> - one\n\n>") == NULL, "quoted unordered blank stays quoted");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> * one\n\n## h\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted unordered blank heading render");
    fails += expect(strstr(out, "> - one\n\n## h") != NULL && strstr(out, "> - one\n\n\n## h") == NULL, "quoted unordered blank before heading has one blank");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> 1.   one\n> 2.   two\n> \n> next\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quoted ordered blank render");
    fails += expect(strstr(out, "> 1. one\n> 2. two\n>\n> next") != NULL && strstr(out, "> 2. two\n\n>") == NULL, "quoted ordered blank stays quoted");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* A list item with a code block:\n\n        <code goes here>\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list code block render");
    fails += expect(strstr(out, "\n\n    <code goes here>") != NULL, "list code block indentation preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 64;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "quote code reset renderer create");
    st = renderer->render_cstr(renderer, ">     code_value\n\nplain words continue across a wrap boundary without quote prefix\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "quote code reset render");
    fails += expect(strstr(out, "\n> plain") == NULL && strstr(out, "without quote prefix") != NULL, "quote wrap reset after quoted code");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 80;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer recreate after quote code");
    st = renderer->render_cstr(renderer, "A **north-star Outcome Card** should be **broad in definition** and\n**non-prescriptive**\342\200\224avoiding early commitments to specific\ntechnologies, solutions, or metrics. Its role is to align diverse\nefforts without constraining how they\342\200\231re executed.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled suffix wrap render");
    fails += expect(strstr(out, "broad in definition and\nnon-prescriptive\342\200\224avoiding early commitments") != NULL && strstr(out, "non-prescriptive\n\342\200\224avoiding") == NULL, "styled suffix wraps as attached word");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* **Falsifiability Check** - Confirm that the hypothesis is **testable\n  and falsifiable**, ensuring that failure to achieve the expected\n  outcome can be clearly recognized and learned from.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "styled comma suffix wrap render");
    fails += expect(strstr(out, "testable and\n  falsifiable, ensuring") != NULL && strstr(out, "falsifiable\n  ,") == NULL, "styled comma suffix wraps with word");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* Lorem ipsum dolor sit amet, consectetuer adipiscing elit.\nAliquam hendrerit mi posuere lectus.\n* Donec sit amet nisl.\n Suspendisse id sem consectetuer libero luctus adipiscing.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "lazy list continuation render");
    fails += expect(strstr(out, "- Lorem ipsum dolor sit amet, consectetuer adipiscing elit. Aliquam hendrerit") != NULL && strstr(out, "lectus.\n- Donec") != NULL, "lazy list continuation joins item");
    fails += expect(strstr(out, "nisl. Suspendisse id sem") != NULL, "indented lazy list continuation joins item");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "This is *em* and **strong** and ***both***.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "emphasis render");
    fails += expect(strstr(out, "*em*") == NULL && strstr(out, "**strong**") == NULL && strstr(out, "***both***") == NULL, "emphasis removes delimiters");
    fails += expect(strstr(out, "em") != NULL && strstr(out, "strong") != NULL && strstr(out, "both") != NULL, "emphasis text visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "**_both_**\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "nested emphasis render");
    fails += expect(strstr(out, "**_both_**") == NULL && strstr(out, "both") != NULL, "nested emphasis removes delimiters");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Big numbers like 10_000_000 should not wrap.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "intraword underscore render");
    fails += expect(strstr(out, "10 000 000") != NULL && strstr(out, "10_000_000") == NULL, "intraword underscores become spaces");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "What\342\200\231s the smallest experiment that falsifies the wrong path?\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "utf8 width render");
    fails += expect(strstr(out, "What\342\200\231s") != NULL && strstr(out, "wrong path") != NULL, "utf8 width text visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Constraints exist to define safe boundaries\342\200\224not to prescribe\nsolutions. Over time, teams often inherit outdated specifications or\ninterpret vague standards as fixed requirements. This slow expansion\nof what\342\200\231s considered \342\200\234non-negotiable\342\200\235 can quietly kill\ninnovation. Revisit constraints regularly to ensure they\342\200\231re still\nvalid, contextual, and evidence-based.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "utf8 wrap boundary render");
    fails += expect(strstr(out, "non-negotiable\342\200\235\ncan quietly") != NULL && strstr(out, "\342\n\200\235") == NULL, "utf8 wrap keeps continuation bytes together");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "x\n\n---\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "thematic break render");
    fails += expect(strstr(out, "----") == NULL, "thematic break suppressed");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "Title\n=====\n\nSub\n---\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "thematic after soft break render");
    fails += expect(strstr(out, "Sub ") == NULL, "thematic does not force pending soft space");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "a&nbsp;b &amp; c &#160; &#xA0;\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "entity render");
    fails += expect(strstr(out, "&nbsp;") == NULL && strstr(out, "&amp;") == NULL, "entities decoded");
    fails += expect(strstr(out, "a b & c") != NULL, "decoded entity text visible");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> quote\n> more\n\nplain\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "blockquote render");
    fails += expect(strstr(out, "> quote more") != NULL, "blockquote lines join");
    fails += expect(strstr(out, "\n\nplain") != NULL, "blockquote paragraph break");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> *Tip: one\n> two*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "blockquote multiline emphasis render");
    fails += expect(strstr(out, "> Tip: one two") != NULL && strstr(out, "*Tip") == NULL, "blockquote multiline emphasis joins");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> **Example:**  \n> *Intent text\n> more text*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "blockquote hard break emphasis render");
    fails += expect(strstr(out, "> Example:\n> Intent text more text") != NULL, "blockquote hard break before emphasis preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "* item\n\n\t> **Example:**  \n\t> *Intent text\n\t> more text*\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "list blockquote hard break emphasis render");
    fails += expect(strstr(out, "- item\n\n  > Example:\n  > Intent text more text") != NULL, "list blockquote hard break keeps indent");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> a\n> \n> b\n\n>     code_value\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "blockquote blank and code render");
    fails += expect(strstr(out, "> a\n") != NULL && strstr(out, "> b") != NULL, "blockquote blank line preserved");
    fails += expect(strstr(out, "code_value") != NULL && strstr(out, "code value") == NULL, "quoted indented code preserved");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 24;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "narrow quote renderer create");
    st = renderer->render_cstr(renderer, "> quoted words continue\n> across source lines\n\nplain\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow blockquote render");
    fails += expect(strstr(out, "> quoted words continue") != NULL && strstr(out, "\n> across source lines") != NULL, "blockquote visual wrap prefix");
    fails += expect(strstr(out, "\n\nplain") != NULL, "blockquote wrap state reset");
    renderer->string_free(renderer, out);
    out = NULL;
    opts.width = 80;
    renderer->destroy(renderer);
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "default width renderer recreate");
    st = renderer->render_cstr(renderer, "---\ntitle: Skip\n---\n\n# Kept\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "yaml frontmatter render");
    fails += expect(strstr(out, "Skip") == NULL && strstr(out, "Kept") != NULL, "yaml frontmatter stripped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "+++\ntitle = \"Skip\"\n+++\n\n# Kept\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "toml frontmatter render");
    fails += expect(strstr(out, "Skip") == NULL && strstr(out, "Kept") != NULL, "toml frontmatter stripped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, ";;;\n{\"title\":\"Skip\"}\n;;;\n\n# Kept\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "json frontmatter render");
    fails += expect(strstr(out, "Skip") == NULL && strstr(out, "Kept") != NULL, "json frontmatter stripped");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "---\n# Keep delimiter\n---\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "non-frontmatter delimiter render");
    fails += expect(strstr(out, "Keep delimiter") != NULL, "non-frontmatter delimiter kept");
    renderer->string_free(renderer, out);

    memset(&sink_data, 0, sizeof(sink_data));
    sink.userdata = &sink_data;
    sink.write = mem_write;
    tok.type = MDF_TOKEN_HEADING_START;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 2;
    fails += expect(renderer->write_token(renderer, &tok, &sink) == MDF_OK, "token heading start");
    tok.type = MDF_TOKEN_TEXT;
    tok.text = "Token";
    tok.len = 5;
    tok.level = 0;
    fails += expect(renderer->write_token(renderer, &tok, &sink) == MDF_OK, "token text");
    tok.type = MDF_TOKEN_HEADING_END;
    tok.text = NULL;
    tok.len = 0;
    tok.level = 2;
    fails += expect(renderer->write_token(renderer, &tok, &sink) == MDF_OK, "token heading end");
    fails += expect(strstr(sink_data.buf, "Token") != NULL, "token output");
    fails += expect(mdf_theme_count() > 1, "theme count");
    fails += expect(mdf_theme_exists("default"), "default theme exists");
    fails += expect(mdf_theme_exists("TOKYO-NIGHT"), "theme lookup is case-insensitive");
    renderer->destroy(renderer);
    renderer = NULL;
    opts.theme_name = "tokyo-night";
    opts.boring = 0;
    opts.osc8 = 0;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "ansi selected theme create");
    out = NULL;
    st = renderer->render_cstr(renderer, "# Head\n\n> quote\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "\033[1;38;5;218m# ") != NULL &&
                    strstr(out, "\033[1;38;5;218mHead") != NULL &&
                    strstr(out, "\033[38;5;239m>") != NULL &&
                    strstr(out, "\033[38;5;153mquote\033[0m") != NULL,
                    "ansi selected theme applies heading and quote text styles");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> plain *em* **strong** `code` [link](https://example.com)\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "\033[38;5;153mplain \033[0m\033[3m\033[38;5;110mem\033[0m") != NULL &&
                    strstr(out, "\033[38;5;153m \033[0m\033[1m\033[1;38;5;111mstrong\033[0m") != NULL &&
                    strstr(out, "\033[38;5;153m \033[0m\033[38;5;67mcode\033[0m") != NULL &&
                    strstr(out, "\033[38;5;153m \033[0m\033[4m\033[1;38;5;110mlink\033[0m") != NULL,
                    "ansi selected theme quote text does not leak into inline styles");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "> | H | S |\n> | --- | --- |\n> | plain | **strong** [link](https://example.com) |\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "\033[38;5;153mplain") != NULL &&
                    strstr(out, "\033[1m\033[1;38;5;111mstrong") != NULL &&
                    strstr(out, "\033[4m\033[1;38;5;110mlink") != NULL,
                    "ansi selected theme quote text applies to quoted table plain cells");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    renderer = NULL;
    opts.theme_name = "default";
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "ansi default theme quote text create");
    out = NULL;
    st = renderer->render_cstr(renderer, "> quote\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "\033[90m>\033[0m quote\n") != NULL &&
                    strstr(out, "\033[90m>\033[0m \033[") == NULL,
                    "ansi default theme leaves quote text plain");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    renderer = NULL;
    opts.theme_name = "tokyo-night";
    st = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "html selected theme create");
    out = NULL;
    st = renderer->render_cstr(renderer, "# Head\n\n* item\n", &out);
    fails += expect(st == MDF_OK && out != NULL &&
                    strstr(out, "color:rgb(255,175,215)") != NULL &&
                    strstr(out, "color:rgb(95,175,215)") != NULL,
                    "html selected theme applies heading and list styles");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    renderer = NULL;
    fails += expect(mdf_terminal_width(-1, 77) == 77, "terminal width fallback");
    fails += expect(run_margin_corpus_cases(), "ansi margin corpus line shape invariants");
    fails += expect(run_ansi_link_wrap_regression_cases(), "ansi link wrap matrix preserves visible text and margins");
    fails += expect(run_ansi_margin_table_regression_cases(), "ansi table margin matrix preserves visible text and ANSI boundaries");
    fails += expect(run_ansi_table_cell_margin_regression_case(), "ansi table cells do not inherit parent margins");
    fails += expect(run_ansi_table_osc8_regression_case(), "ansi table osc8 closes wrapped cell links per line");

    parser->destroy(parser);
    opts.width = 20;
    opts.theme_name = "default";
    opts.boring = 1;
    opts.osc8 = 0;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "wrap renderer create");
    out = NULL;
    st = renderer->render_cstr(renderer, "123456789 123456789 123456789\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "word wrap render");
    fails += expect(strcmp(out, "123456789 123456789\n123456789\n") == 0, "word wrap output");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "aaaaaaaaaaaaaaaaaaaaaaaaa\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "long word wrap render");
    fails += expect(strcmp(out, "aaaaaaaaaaaaaaaaaaaa\naaaaa\n") == 0, "long word wrap output");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "### Example 1: *lockd* - verification as a first-class artifact\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow heading emphasis render");
    fails += expect(strstr(out, "### Example 1: lockd\n    - verification") != NULL, "narrow heading wraps with continuation indent");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "correctness and concurrency\342\200\224**just over 30% of all lines are tests**, spread across more than 200 test cases.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "dash-adjacent emphasis render");
    fails += expect(strstr(out, "concurrency\342\200\224just\n") != NULL && strstr(out, "tests,\nspread") != NULL, "dash-adjacent emphasis preserves order and suffix");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "- [ ] Do we define success in terms of user behavior, business value, or system\n      performance\342\200\224not just features delivered?\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "narrow task utf8 word render");
    fails += expect(strstr(out, "performance\342\200\224not\n") != NULL && strstr(out, "performance\342\200\224no\n") == NULL, "narrow task utf8 word stays atomic");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer, "  plain\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "leading indent paragraph render");
    fails += expect(strcmp(out, "plain\n") == 0, "leading indent paragraph ignores non-code indentation");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    opts.width = 80;
    opts.boring = 0;
    opts.osc8 = 1;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "osc8 renderer create");
    out = NULL;
    st = renderer->render_cstr(renderer, "See [website](https://example.com) now.\n", &out);
    fails += expect(st == MDF_OK && out != NULL, "osc8 link render");
    fails += expect(strstr(out, "\033]8;;https://example.com\033\\") != NULL, "osc8 start visible");
    fails += expect(strstr(out, "\033]8;;\033\\") != NULL, "osc8 end visible");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    opts.width = 65;
    opts.margin_left = 2;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "osc8 wrapped margin renderer create");
    out = NULL;
    st = renderer->render_cstr(renderer,
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda [Agile Manifesto](https://agilemanifesto.org/), tail\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "osc8 wrapped margin link render");
    fails += expect(strstr(out, "\033]8;;https://agilemanifesto.org/\033\\\n") == NULL,
                    "osc8 opener is not stranded before wrapped label");
    fails += expect(strstr(out, "lambda \n") == NULL,
                    "osc8 wrapped label does not leave trailing separator space");
    fails += expect(strstr(out, "\n  \033]8;;https://agilemanifesto.org/\033\\\033[4m\033[1;34mAgile Manifesto") != NULL,
                    "osc8 opener moves with wrapped label after margin");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda <https://agilemanifesto.org/> tail\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "osc8 wrapped margin autolink render");
    fails += expect(strstr(out, "\033]8;;https://agilemanifesto.org/\033\\\n") == NULL,
                    "osc8 autolink opener is not stranded before wrapped text");
    fails += expect(strstr(out, "lambda \n") == NULL,
                    "osc8 wrapped autolink does not leave trailing separator space");
    fails += expect(strstr(out, "\n  \033]8;;https://agilemanifesto.org/\033\\\033[4m\033[1;34mhttps://agilemanifesto.org/") != NULL,
                    "osc8 autolink opener moves with wrapped text after margin");
    renderer->string_free(renderer, out);
    out = NULL;
    st = renderer->render_cstr(renderer,
        "change through adaptive learning and value delivery. Inspired by the original [Agile Manifesto](https://agilemanifesto.org/), this framework defines a model where teams are driven by **outcomes**, **not requirements**, and guided by evidence, not assumption.\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "osc8 obaf-shaped margin link render");
    fails += expect(strstr(out, "Agile\033[0m\n  \033[4m\033[1;34mManifesto") == NULL,
                    "osc8 obaf link label stays atomic when it fits after wrap");
    fails += expect(strstr(out, "outcomes") != NULL,
                    "osc8 obaf wrapped emphasis does not drop outcomes");
    renderer->string_free(renderer, out);
    opts.width = 60;
    renderer->destroy(renderer);
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    fails += expect(st == MDF_OK && renderer != NULL, "osc8 narrow obaf margin renderer create");
    out = NULL;
    st = renderer->render_cstr(renderer,
        "change through adaptive learning and value delivery. Inspired by the original [Agile Manifesto](https://agilemanifesto.org/), this framework defines a model where teams are driven by **outcomes**, **not requirements**, and guided by evidence, not assumption.\n",
        &out);
    fails += expect(st == MDF_OK && out != NULL, "osc8 narrow obaf margin link render");
    fails += expect(strstr(out, "outcomes") != NULL,
                    "osc8 narrow obaf wrapped emphasis does not drop outcomes");
    fails += expect(strstr(out, "\n   and guided") == NULL,
                    "osc8 narrow obaf continuation does not leak an extra leading space after margin");
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    opts.margin_left = 0;
    opts.osc8 = 0;
    fails += expect(allocs.allocs > 0 && allocs.frees > 0, "custom allocator used");
    fails += expect(allocs.allocs == allocs.frees, "custom allocator balances allocations after destroy");
    return fails == 0 ? 0 : 1;
}
