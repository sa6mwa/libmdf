#include <libmdf/mdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RECORDS 1024

typedef struct chunk_source {
    const char *src;
    size_t len;
    size_t off;
    size_t chunk;
} chunk_source;

typedef struct record {
    char *data;
    size_t len;
} record;

typedef struct capture {
    record writes[MAX_RECORDS];
    record traces[MAX_RECORDS];
    size_t write_count;
    size_t trace_count;
    char *out;
    size_t out_len;
    size_t out_cap;
    int failed;
} capture;

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

static char *dup_bytes(const char *src, size_t len)
{
    char *dst;

    dst = (char *)malloc(len + 1);
    if (dst == NULL) {
        return NULL;
    }
    if (len != 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
    return dst;
}

static int capture_append_output(capture *cap, const char *src, size_t len)
{
    char *next;
    size_t next_cap;
    size_t need;

    if (cap->out_len + len + 1 < cap->out_len) {
        return -1;
    }
    need = cap->out_len + len + 1;
    if (need > cap->out_cap) {
        next_cap = cap->out_cap == 0 ? 512 : cap->out_cap;
        while (next_cap < need) {
            next_cap *= 2;
        }
        next = (char *)realloc(cap->out, next_cap);
        if (next == NULL) {
            return -1;
        }
        cap->out = next;
        cap->out_cap = next_cap;
    }
    if (len != 0) {
        memcpy(cap->out + cap->out_len, src, len);
    }
    cap->out_len += len;
    cap->out[cap->out_len] = '\0';
    return 0;
}

static int capture_record(record *records, size_t *count, const char *src, size_t len)
{
    if (*count >= MAX_RECORDS) {
        return -1;
    }
    records[*count].data = dup_bytes(src, len);
    if (records[*count].data == NULL) {
        return -1;
    }
    records[*count].len = len;
    (*count)++;
    return 0;
}

static int capture_write(void *userdata, const char *src, size_t len)
{
    capture *cap;

    cap = (capture *)userdata;
    if (capture_record(cap->writes, &cap->write_count, src, len) != 0 ||
        capture_append_output(cap, src, len) != 0) {
        cap->failed = 1;
        return -1;
    }
    return 0;
}

static int capture_trace(void *userdata, mdf_format format, const char *src, size_t len)
{
    capture *cap;

    (void)format;
    cap = (capture *)userdata;
    if (capture_record(cap->traces, &cap->trace_count, src, len) != 0) {
        cap->failed = 1;
        return -1;
    }
    return 0;
}

static void capture_free(capture *cap)
{
    size_t i;

    for (i = 0; i < cap->write_count; i++) {
        free(cap->writes[i].data);
    }
    for (i = 0; i < cap->trace_count; i++) {
        free(cap->traces[i].data);
    }
    free(cap->out);
    memset(cap, 0, sizeof(*cap));
}

static size_t chunk_read(void *userdata, char *dst, size_t cap, int *err)
{
    chunk_source *src;
    size_t n;

    (void)err;
    src = (chunk_source *)userdata;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->len - src->off;
    if (n > src->chunk) {
        n = src->chunk;
    }
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, src->src + src->off, n);
    src->off += n;
    return n;
}

static mdf_status render_capture(mdf_format format, mdf_options *opts, const char *markdown, size_t chunk, capture *cap)
{
    mdf *inst;
    mdf_source source;
    mdf_sink sink;
    chunk_source source_data;
    mdf_status st;

    inst = NULL;
    memset(cap, 0, sizeof(*cap));
    opts->write_trace.userdata = cap;
    opts->write_trace.emit = capture_trace;
    st = mdf_create(format, opts, &inst);
    if (st != MDF_OK) {
        return st;
    }
    source_data.src = markdown;
    source_data.len = strlen(markdown);
    source_data.off = 0;
    source_data.chunk = chunk;
    source.userdata = &source_data;
    source.read = chunk_read;
    sink.userdata = cap;
    sink.write = capture_write;
    st = inst->render(inst, &source, &sink);
    inst->destroy(inst);
    return st;
}

static int expect_trace_matches_writes(const capture *cap, const char *msg)
{
    size_t i;

    if (cap->write_count != cap->trace_count) {
        fprintf(stderr, "FAIL: %s: write_count=%lu trace_count=%lu\n",
                msg,
                (unsigned long)cap->write_count,
                (unsigned long)cap->trace_count);
        return 1;
    }
    for (i = 0; i < cap->write_count; i++) {
        if (cap->writes[i].len != cap->traces[i].len ||
            memcmp(cap->writes[i].data, cap->traces[i].data, cap->writes[i].len) != 0) {
            fprintf(stderr, "FAIL: %s: mismatch at emission %lu\n", msg, (unsigned long)i);
            return 1;
        }
    }
    return 0;
}

static int expect_write_sequence(const capture *cap, const char *const *expected, size_t expected_count, const char *msg)
{
    size_t i;

    if (cap->write_count != expected_count) {
        fprintf(stderr, "FAIL: %s: write_count=%lu expected=%lu\n",
                msg,
                (unsigned long)cap->write_count,
                (unsigned long)expected_count);
        return 1;
    }
    for (i = 0; i < expected_count; i++) {
        size_t expected_len;

        expected_len = strlen(expected[i]);
        if (cap->writes[i].len != expected_len ||
            memcmp(cap->writes[i].data, expected[i], expected_len) != 0) {
            fprintf(stderr,
                    "FAIL: %s: write %lu expected [%s] got [%.*s]\n",
                    msg,
                    (unsigned long)i,
                    expected[i],
                    (int)cap->writes[i].len,
                    cap->writes[i].data);
            return 1;
        }
    }
    return 0;
}

static int expect_output_equals(const capture *cap, const char *expected, const char *msg)
{
    if (cap->out == NULL || strcmp(cap->out, expected) != 0) {
        fprintf(stderr,
                "FAIL: %s: expected [%s] got [%s]\n",
                msg,
                expected,
                cap->out == NULL ? "" : cap->out);
        return 1;
    }
    return 0;
}

static int record_contains(const record *rec, const char *needle)
{
    size_t needle_len;
    size_t i;

    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > rec->len) {
        return 0;
    }
    for (i = 0; i + needle_len <= rec->len; i++) {
        if (memcmp(rec->data + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int expect_no_write_contains(const capture *cap, const char *needle, const char *msg)
{
    size_t i;

    for (i = 0; i < cap->write_count; i++) {
        if (record_contains(&cap->writes[i], needle)) {
            fprintf(stderr, "FAIL: %s: found in write %lu\n", msg, (unsigned long)i);
            return 1;
        }
    }
    return 0;
}

static char *strip_ansi(const char *src)
{
    size_t len;
    size_t i;
    size_t out_len;
    char *out;

    len = strlen(src);
    out = (char *)malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    i = 0;
    out_len = 0;
    while (i < len) {
        if ((unsigned char)src[i] == 0x1b) {
            i++;
            if (i < len && src[i] == '[') {
                i++;
                while (i < len && (src[i] < '@' || src[i] > '~')) {
                    i++;
                }
                if (i < len) {
                    i++;
                }
            } else if (i < len && src[i] == ']') {
                i++;
                while (i < len) {
                    if (src[i] == '\007') {
                        i++;
                        break;
                    }
                    if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
            } else if (i < len) {
                i++;
            }
        } else {
            out[out_len++] = src[i++];
        }
    }
    out[out_len] = '\0';
    return out;
}

static int expect_line_margins(const char *raw, int left, int max_cols, const char *msg)
{
    char *visible;
    const char *line;
    const char *end;
    size_t len;
    size_t pos;
    int cols;
    int fails;
    int i;

    visible = strip_ansi(raw);
    if (visible == NULL) {
        fprintf(stderr, "FAIL: %s: out of memory\n", msg);
        return 1;
    }
    fails = 0;
    line = visible;
    while (*line != '\0') {
        end = strchr(line, '\n');
        if (end == NULL) {
            end = line + strlen(line);
        }
        len = (size_t)(end - line);
        if (len == 0) {
            if (end[0] == '\0') {
                break;
            }
            line = end + 1;
            continue;
        }
        cols = 0;
        pos = 0;
        while (pos < len) {
            unsigned char c;

            c = (unsigned char)line[pos];
            if (c < 0x80) {
                pos++;
            } else if ((c & 0xe0) == 0xc0 && pos + 1 < len) {
                pos += 2;
            } else if ((c & 0xf0) == 0xe0 && pos + 2 < len) {
                pos += 3;
            } else if ((c & 0xf8) == 0xf0 && pos + 3 < len) {
                pos += 4;
            } else {
                pos++;
            }
            cols++;
        }
        if (cols > max_cols) {
            fprintf(stderr, "FAIL: %s: line too wide: %lu > %d: %.*s\n",
                    msg,
                    (unsigned long)cols,
                    max_cols,
                    (int)len,
                    line);
            fails++;
        }
        for (i = 0; i < left; i++) {
            if ((size_t)i >= len || line[i] != ' ') {
                fprintf(stderr, "FAIL: %s: missing left margin: %.*s\n", msg, (int)len, line);
                fails++;
                break;
            }
        }
        if (end[0] == '\0') {
            break;
        }
        line = end + 1;
    }
    free(visible);
    return fails;
}

static int expect_no_styled_line_without_margin(const char *raw, const char *msg)
{
    const char *p;

    if (raw[0] == '\033') {
        fprintf(stderr, "FAIL: %s: first line starts with style before margin\n", msg);
        return 1;
    }
    p = raw;
    while ((p = strstr(p, "\n\033")) != NULL) {
        fprintf(stderr, "FAIL: %s: wrapped line starts with style before margin\n", msg);
        return 1;
    }
    return 0;
}

static int expect_not_contains(const char *haystack, const char *needle, const char *msg)
{
    if (haystack != NULL && strstr(haystack, needle) != NULL) {
        fprintf(stderr, "FAIL: %s: found %s\n", msg, needle);
        return 1;
    }
    return 0;
}

static int expect_contains(const char *haystack, const char *needle, const char *msg)
{
    if (haystack == NULL || strstr(haystack, needle) == NULL) {
        fprintf(stderr, "FAIL: %s: missing %s\n", msg, needle);
        return 1;
    }
    return 0;
}

static int test_stream_trace_contract(void)
{
    static const char plain_markdown[] = "Alpha Beta\n";
    static const char *const plain_writes[] = {
        "Alpha",
        " ",
        "Beta",
        "\n",
    };
    static const char heading_markdown[] = "# Alpha Beta\n";
    static const char *const heading_writes[] = {
        "\033[1;32m#",
        " ",
        "\033[0m",
        "\033[1;32mAlpha",
        " ",
        "Beta",
        "\033[0m",
        "\n",
    };
    static const char markdown[] =
        "# The Outcome-Based Agile Framework\n"
        "\n"
        "> **Common Misreadings** imply lack of autonomy.\n"
        "- [Agile Manifesto](https://agilemanifesto.org/) links remain styled.\n"
        "\n"
        "Outcomes, not outputs. Always.\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, plain_markdown, 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "plain one-byte stream render succeeds");
    fails += expect_trace_matches_writes(&cap, "plain one-byte stream writes match traces");
    fails += expect_write_sequence(&cap,
                                   plain_writes,
                                   sizeof(plain_writes) / sizeof(plain_writes[0]),
                                   "plain one-byte stream emits each decided word boundary");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, plain_markdown, sizeof(plain_markdown), &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "plain full-chunk stream render succeeds");
    fails += expect_trace_matches_writes(&cap, "plain full-chunk stream writes match traces");
    fails += expect_write_sequence(&cap,
                                   plain_writes,
                                   sizeof(plain_writes) / sizeof(plain_writes[0]),
                                   "plain full-chunk stream emits each decided word boundary");
    capture_free(&cap);

    mdf_options_init(&opts);
    st = render_capture(MDF_FORMAT_ANSI, &opts, heading_markdown, 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled heading one-byte stream render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled heading one-byte stream writes match traces");
    fails += expect_write_sequence(&cap,
                                   heading_writes,
                                   sizeof(heading_writes) / sizeof(heading_writes[0]),
                                   "styled heading one-byte stream emits exact decisions");
    capture_free(&cap);

    mdf_options_init(&opts);
    st = render_capture(MDF_FORMAT_ANSI, &opts, heading_markdown, sizeof(heading_markdown), &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled heading full-chunk stream render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled heading full-chunk stream writes match traces");
    fails += expect_write_sequence(&cap,
                                   heading_writes,
                                   sizeof(heading_writes) / sizeof(heading_writes[0]),
                                   "styled heading full-chunk stream emits exact decisions");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.width = 34;
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, markdown, 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "stream trace contract render succeeds");
    fails += expect_trace_matches_writes(&cap, "stream trace contract writes match traces");
    fails += expect_no_write_contains(&cap,
                                      "The Outcome-Based Agile Framework",
                                      "heading is not fake-streamed as one sink write");
    fails += expect_no_write_contains(&cap,
                                      "Common Misreadings imply lack",
                                      "blockquote styled text is not coalesced across words");
    capture_free(&cap);
    return fails;
}

static int test_margin_contract(void)
{
    static const char markdown[] =
        "# Margin Heading\n"
        "\n"
        "alpha beta gamma delta epsilon zeta eta theta\n"
        "\n"
        "> quote text wraps under the same absolute margin\n"
        "\n"
        "- item one two three four five\n"
        "\n"
        "```\n"
        "code words wrap under margin too\n"
        "```\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.width = 24;
    opts.margin_left = 3;
    opts.margin_right = 2;
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, markdown, 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "boring margin corpus render succeeds");
    fails += expect_trace_matches_writes(&cap, "boring margin corpus writes match traces");
    fails += expect_line_margins(cap.out, 3, 22, "boring margins are absolute and right-bounded");
    fails += expect_not_contains(cap.out, "\n   \n", "empty lines do not receive lazy left margin spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.width = 28;
    opts.margin_left = 2;
    opts.margin_right = 3;
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts,
                        "# [Agile Manifesto](https://agilemanifesto.org/) wraps here\n",
                        1,
                        &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled margin link render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled margin link writes match traces");
    fails += expect_line_margins(cap.out, 2, 25, "styled margins are absolute and right-bounded");
    fails += expect_no_styled_line_without_margin(cap.out, "ansi styles never precede left margin");
    fails += expect_contains(cap.out, "\033]8;;https://agilemanifesto.org/\033\\",
                             "osc8 link remains present inside styled margin render");
    capture_free(&cap);
    return fails;
}

static int test_table_contract(void)
{
    static const char markdown[] =
        "| Link | Text |\n"
        "| --- | --- |\n"
        "| [Site](https://example.com/) | alpha beta |\n"
        "| plain | row wraps |\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.width = 40;
    opts.margin_left = 2;
    opts.margin_right = 2;
    opts.boring = 1;
    opts.table_buffer_mode = MDF_TABLE_BUFFER_FULL;
    st = render_capture(MDF_FORMAT_ANSI, &opts, markdown, 2, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "full-buffer table margin render succeeds");
    fails += expect_trace_matches_writes(&cap, "full-buffer table writes match traces");
    fails += expect_line_margins(cap.out, 2, 38, "full-buffer table honors margins");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.width = 40;
    opts.margin_left = 2;
    opts.margin_right = 2;
    opts.boring = 1;
    opts.table_buffer_mode = MDF_TABLE_BUFFER_ROW;
    st = render_capture(MDF_FORMAT_ANSI, &opts, markdown, 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "row-buffer table margin render succeeds");
    fails += expect_trace_matches_writes(&cap, "row-buffer table writes match traces");
    fails += expect_line_margins(cap.out, 2, 38, "row-buffer table honors margins");
    capture_free(&cap);
    return fails;
}

static int test_chart_trace_contract(void)
{
    static const char markdown[] =
        "```mdf-bar-chart,sort\n"
        "Build,40\n"
        "Test,25\n"
        "Ship,10\n"
        "```\n"
        "\n"
        "```mdf-vertical-bar-chart\n"
        "A,1\n"
        "B,3\n"
        "C,2\n"
        "```\n"
        "\n"
        "```mdf-tile-chart\n"
        "Build,40\n"
        "Test,25\n"
        "Ship,10\n"
        "```\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    char *visible;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.width = 60;
    opts.margin_left = 4;
    opts.margin_right = 3;
    opts.boring = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, markdown, 3, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi chart trace corpus render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi chart writes match traces");
    fails += expect_line_margins(cap.out, 4, 57, "ansi charts honor margins under trace");
    fails += expect_no_styled_line_without_margin(cap.out, "ansi chart styles never precede left margin");
    visible = strip_ansi(cap.out);
    fails += expect(visible != NULL, "ansi chart visible output strips styles");
    fails += expect_contains(visible, "53.3%", "horizontal chart percentage is present");
    fails += expect_contains(visible, "│", "vertical chart axis is present");
    fails += expect_contains(visible, "Build 40", "tile chart raw-value legend is present");
    free(visible);
    capture_free(&cap);
    return fails;
}

static int test_html_link_safety_contract(void)
{
    static const char markdown[] =
        "# [heading](javascript:alert%281%29)\n"
        "\n"
        "> [quote](data:text/html,boom)\n"
        "\n"
        "| Unsafe | Safe |\n"
        "| --- | --- |\n"
        "| [cell](vbscript:msgbox%281%29) | [site](https://example.com) |\n"
        "\n"
        "[file](file:///etc/passwd) [entity](&#106;avascript:alert%281%29) [rel](docs/page) [frag](#top) [proto](//example.com)\n"
        "<javascript:alert%281%29>\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    st = render_capture(MDF_FORMAT_HTML, &opts, markdown, 3, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "html link safety corpus render succeeds");
    fails += expect_not_contains(cap.out, "href=\"javascript:", "html rejects javascript hrefs");
    fails += expect_not_contains(cap.out, "href=\"data:", "html rejects data hrefs");
    fails += expect_not_contains(cap.out, "href=\"vbscript:", "html rejects vbscript hrefs");
    fails += expect_not_contains(cap.out, "href=\"file:", "html rejects file hrefs");
    fails += expect_not_contains(cap.out, "href=\"&#106;avascript:", "html rejects entity-obfuscated hrefs");
    fails += expect_contains(cap.out, "href=\"https://example.com\"", "html preserves https hrefs");
    fails += expect_contains(cap.out, "href=\"docs/page\"", "html preserves relative hrefs");
    fails += expect_contains(cap.out, "href=\"#top\"", "html preserves fragment hrefs");
    fails += expect_contains(cap.out, "href=\"//example.com\"", "html preserves protocol-relative hrefs");
    capture_free(&cap);
    return fails;
}

static int test_ansi_nested_emphasis_edge_contract(void)
{
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo_bar*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi unmatched nested underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi unmatched nested underscore writes match traces");
    fails += expect_contains(cap.out, "foo_bar", "ansi unmatched nested underscore preserves tail");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo*bar_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi unmatched nested star render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi unmatched nested star writes match traces");
    fails += expect_contains(cap.out, "foo*bar", "ansi unmatched nested star preserves tail");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo \\_bar_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped nested delimiter render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped nested delimiter writes match traces");
    fails += expect_contains(cap.out, "foo _bar_ baz", "ansi escaped nested delimiter remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*\\_)*\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped delimiter in pending emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped delimiter in pending emphasis writes match traces");
    fails += expect_contains(cap.out, "_)", "ansi escaped delimiter in pending emphasis consumes escape");
    fails += expect_not_contains(cap.out, "\\_)", "ansi escaped delimiter in pending emphasis does not expose backslash");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*\\_)*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi chunked escaped delimiter in pending emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi chunked escaped delimiter in pending emphasis writes match traces");
    fails += expect_contains(cap.out, "_)", "ansi chunked escaped delimiter in pending emphasis consumes escape");
    fails += expect_not_contains(cap.out, "\\_)", "ansi chunked escaped delimiter in pending emphasis does not expose backslash");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*\\_foo*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped leading underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped leading underscore writes match traces");
    fails += expect_contains(cap.out, "_foo", "ansi escaped leading underscore consumes escape");
    fails += expect_not_contains(cap.out, "\\_foo", "ansi escaped leading underscore does not expose backslash");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*a\\_*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped trailing underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped trailing underscore writes match traces");
    fails += expect_contains(cap.out, "a_", "ansi escaped trailing underscore consumes escape");
    fails += expect_not_contains(cap.out, "a\\_", "ansi escaped trailing underscore does not expose backslash");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo_bar_baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi intraword underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi intraword underscore writes match traces");
    fails += expect_contains(cap.out, "foo_bar_baz", "ansi intraword underscore remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*a b_c_d b*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi spaced intraword underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi spaced intraword underscore writes match traces");
    fails += expect_contains(cap.out, "a b_c_d b", "ansi spaced intraword underscore remains literal");
    fails += expect_not_contains(cap.out, "bc_d", "ansi spaced intraword underscore does not drop delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo___bar___baz*\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi intraword underscore run render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi intraword underscore run writes match traces");
    fails += expect_contains(cap.out, "foo___bar___baz", "ansi intraword underscore run remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar_baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi intraword nested underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi intraword nested underscore writes match traces");
    fails += expect_contains(cap.out, "foo _bar_baz", "ansi intraword nested underscore remains literal");
    fails += expect_not_contains(cap.out, "barbaz", "ansi intraword nested underscore does not drop delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi unmatched nested underscore across words render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi unmatched nested underscore across words writes match traces");
    fails += expect_contains(cap.out, "foo _bar baz", "ansi unmatched nested underscore across words preserves opener");
    fails += expect_not_contains(cap.out, "foo bar _baz", "ansi unmatched nested underscore across words does not move delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _ bar_*\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nonflanking nested underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nonflanking nested underscore writes match traces");
    fails += expect_contains(cap.out, "foo _ bar_", "ansi nonflanking nested underscore remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo * bar*_\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nonflanking nested star render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nonflanking nested star writes match traces");
    fails += expect_contains(cap.out, "foo * bar*", "ansi nonflanking nested star remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi pending nested underscore eof render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi pending nested underscore eof writes match traces");
    fails += expect_contains(cap.out, "foo _", "ansi pending nested underscore eof remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo *\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi pending nested star eof render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi pending nested star eof writes match traces");
    fails += expect_contains(cap.out, "foo *", "ansi pending nested star eof remains literal");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nested underscore after space render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nested underscore after space writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi nested underscore after space is parsed");
    fails += expect_not_contains(cap.out, "_bar_", "ansi nested underscore after space removes delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar__ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi longer nested underscore close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi longer nested underscore close writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi longer nested underscore close is parsed");
    fails += expect_not_contains(cap.out, "bar__", "ansi longer nested underscore close consumes delimiter run");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo __bar___ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi longer nested strong underscore close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi longer nested strong underscore close writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi longer nested strong underscore close is parsed");
    fails += expect_not_contains(cap.out, "bar_", "ansi longer nested strong underscore close consumes delimiter run");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo *bar** baz_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi longer nested star close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi longer nested star close writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi longer nested star close is parsed");
    fails += expect_not_contains(cap.out, "bar*", "ansi longer nested star close consumes delimiter run");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo **bar*** baz_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi longer nested strong star close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi longer nested strong star close writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi longer nested strong star close is parsed");
    fails += expect_not_contains(cap.out, "bar*", "ansi longer nested strong star close consumes delimiter run");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar \\* baz_* qux\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped star inside nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped star inside nested emphasis writes match traces");
    fails += expect_contains(cap.out, "foo bar * baz qux", "ansi escaped star inside nested emphasis remains visible");
    fails += expect_not_contains(cap.out, "_\\", "ansi escaped star inside nested emphasis does not leak delimiter state");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar \\[baz\\]_ qux*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped brackets inside nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped brackets inside nested emphasis writes match traces");
    fails += expect_contains(cap.out, "foo bar [baz] qux", "ansi escaped brackets inside nested emphasis remain visible");
    fails += expect_not_contains(cap.out, "\\[", "ansi escaped brackets inside nested emphasis do not leak backslash");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_*(_\\*__ ]\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi malformed nested close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi malformed nested close writes match traces");
    fails += expect_contains(cap.out, "*(__", "ansi malformed nested close preserves visible content");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.width = 7;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nested underscore wrapped render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nested underscore wrapped writes match traces");
    fails += expect_contains(cap.out, "foo bar\nbaz", "ansi nested underscore wraps without trailing space");
    fails += expect_not_contains(cap.out, "bar \nbaz", "ansi nested underscore does not emit trailing wrapped space");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo _bar_ baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nested emphasis inside strong render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nested emphasis inside strong writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi nested emphasis inside strong is parsed");
    fails += expect_not_contains(cap.out, "_bar_", "ansi nested emphasis inside strong removes delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo *bar* baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-delimiter nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-delimiter nested emphasis writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-delimiter nested emphasis preserves pending text");
    fails += expect_not_contains(cap.out, "*bar*", "ansi same-delimiter nested emphasis removes delimiters");
    fails += expect_not_contains(cap.out, "baz*", "ansi same-delimiter nested emphasis consumes outer close");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo*bar* baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi adjacent same-delimiter nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi adjacent same-delimiter nested emphasis writes match traces");
    fails += expect_output_equals(&cap, "foobar baz\n", "ansi adjacent same-delimiter nested emphasis preserves pending text");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo *bar* baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-marker nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-marker nested emphasis writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-marker nested emphasis preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo **bar** baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi longer same-marker nested run render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi longer same-marker nested run writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi longer same-marker nested run preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo * bar* baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi whitespace same-marker nested span render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi whitespace same-marker nested span writes match traces");
    fails += expect_output_equals(&cap, "foo  bar baz\n", "ansi whitespace same-marker nested span preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo *bar* baz*\nX\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-marker nested emphasis followed by text render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-marker nested emphasis followed by text writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz X\n", "ansi same-marker nested emphasis closes outer span");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo *bar* baz*\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-marker nested emphasis full chunk render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-marker nested emphasis full chunk writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-marker nested emphasis full chunk preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_foo _bar_ baz_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-marker nested underscore render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-marker nested underscore writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-marker nested underscore preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo **bar** baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-marker nested strong render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-marker nested strong writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-marker nested strong preserves spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI,
                        &opts,
                        "*foo *bar* baz*\n"
                        "_foo _bar_ baz_\n"
                        "**foo **bar** baz**\n"
                        "__foo __bar__ baz__\n"
                        "***foo **bar** baz***\n",
                        1,
                        &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi consecutive same-marker nested spans render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi consecutive same-marker nested spans writes match traces");
    fails += expect_output_equals(&cap,
                                  "foo bar baz foo bar baz foo bar baz foo bar baz foo bar baz\n",
                                  "ansi consecutive same-marker nested spans preserve spaces");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo *bar* baz**\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi same-delimiter nested emphasis full chunk render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi same-delimiter nested emphasis full chunk writes match traces");
    fails += expect_output_equals(&cap, "foo bar baz\n", "ansi same-delimiter nested emphasis full chunk preserves pending text");
    fails += expect_not_contains(cap.out, "baz*", "ansi same-delimiter nested emphasis full chunk consumes outer close");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo*bar* baz**\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi adjacent same-delimiter nested emphasis full chunk render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi adjacent same-delimiter nested emphasis full chunk writes match traces");
    fails += expect_output_equals(&cap, "foobar baz\n", "ansi adjacent same-delimiter nested emphasis full chunk preserves pending text");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "***foo _bar_ baz***\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi triple outer nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi triple outer nested emphasis writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi triple outer nested emphasis is parsed");
    fails += expect_not_contains(cap.out, "_bar_", "ansi triple outer nested emphasis removes delimiters");
    fails += expect_not_contains(cap.out, "baz*", "ansi triple outer nested emphasis consumes outer close");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "***foo _bar_ baz***\n", 64, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi triple outer nested emphasis full chunk render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi triple outer nested emphasis full chunk writes match traces");
    fails += expect_contains(cap.out, "foo bar baz", "ansi triple outer nested emphasis full chunk is parsed");
    fails += expect_not_contains(cap.out, "baz*", "ansi triple outer nested emphasis full chunk consumes outer close");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo _bar_ baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi nested emphasis space continuation render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi nested emphasis space continuation writes match traces");
    fails += expect_contains(cap.out,
                             "\033[1m\033[1;37mfoo \033[0m\033[1m\033[3m\033[1;35mbar",
                             "styled ansi nested emphasis resets before combined style");
    fails += expect_contains(cap.out,
                             "bar\033[0m\033[1m\033[1;37m baz",
                             "styled ansi nested emphasis resumes outer style after space");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _bar_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi same-kind nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi same-kind nested emphasis writes match traces");
    fails += expect_contains(cap.out,
                             "\033[3m\033[34mfoo \033[0mbar\033[3m\033[34m baz",
                             "styled ansi same-kind nested emphasis leaves inner text unstyled");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "# *foo _bar_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi heading same-kind nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi heading same-kind nested emphasis writes match traces");
    fails += expect_contains(cap.out,
                             "\033[1;32m\033[3m\033[34mfoo \033[0m\033[1;32mbar\033[0m\033[1;32m\033[3m\033[34m baz",
                             "styled ansi heading same-kind nested emphasis resumes heading and outer style");
    fails += expect_not_contains(cap.out,
                                 "baz\033[0m\033[0m",
                                 "styled ansi heading same-kind nested emphasis does not double reset");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo _bar_, baz**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi nested emphasis punctuation continuation render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi nested emphasis punctuation continuation writes match traces");
    fails += expect_contains(cap.out,
                             "bar\033[0m\033[1m\033[1;37m, baz",
                             "styled ansi nested emphasis resumes outer style after punctuation");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _[bar](https://e)_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi link inside nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi link inside nested emphasis writes match traces");
    fails += expect_contains(cap.out, "foo bar (https://e) baz", "ansi link inside nested emphasis emits parsed link text");
    fails += expect_not_contains(cap.out, "[bar](https://e)", "ansi link inside nested emphasis is parsed");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*foo _[bar](https://e)_ baz*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi link inside nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi link inside nested emphasis writes match traces");
    fails += expect_contains(cap.out, "\033[0m\033[4m\033[1;34mbar", "styled ansi nested link starts with link style after reset");
    fails += expect_not_contains(cap.out, "\033[0m\033[3m\033[34m\033[4m\033[1;34mbar", "styled ansi nested link does not leak nested emphasis into link");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "*[x](u)*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi emphasized link immediate close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi emphasized link immediate close writes match traces");
    fails += expect_output_equals(&cap, "x (u)\n", "ansi emphasized link immediate close consumes delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_[x](u)_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi underscore emphasized link immediate close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi underscore emphasized link immediate close writes match traces");
    fails += expect_output_equals(&cap, "x (u)\n", "ansi underscore emphasized link immediate close consumes delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**[x](u)**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi strong link immediate close render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi strong link immediate close writes match traces");
    fails += expect_output_equals(&cap, "x (u)\n", "ansi strong link immediate close consumes delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "# *[x](https://e) y*\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi heading emphasized link render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi heading emphasized link writes match traces");
    fails += expect_contains(cap.out,
                             ")\033[1;32m\033[3m\033[34m y",
                             "styled ansi heading emphasized link resumes owned heading style");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 0;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "**foo _[bar](https://e) baz_ qux**\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "styled ansi nested link resumes inner emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "styled ansi nested link resumes inner emphasis writes match traces");
    fails += expect_contains(cap.out, "\033[0m\033[1m\033[3m\033[1;35m\033[4m\033[1;34mbar", "styled ansi strong nested link applies nested style to label");
    fails += expect_contains(cap.out, ")\033[1m\033[3m\033[1;35m baz", "styled ansi nested link resumes combined style after url");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts,
                        "[`link`](http://link.example) [*em*](http://link.example) [**strong**](http://link.example)\n",
                        1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi osc8 styled link labels render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi osc8 styled link label writes match traces");
    fails += expect_contains(cap.out, "\033]8;;http://link.example\033\\", "ansi osc8 styled link labels retain their destination");
    fails += expect_not_contains(cap.out, "`link`", "ansi osc8 inline-code link label consumes code delimiters");
    fails += expect_not_contains(cap.out, "*em*", "ansi osc8 emphasized link label consumes emphasis delimiters");
    fails += expect_not_contains(cap.out, "**strong**", "ansi osc8 strong link label consumes strong delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts,
                        "[`code` and *em*](http://link.example)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi osc8 mixed code and emphasis link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi osc8 mixed link label writes match traces");
    fails += expect_contains(cap.out, "\033]8;;http://link.example\033\\", "ansi osc8 mixed link label retains its destination");
    fails += expect_not_contains(cap.out, "`code`", "ansi mixed link label consumes code delimiters");
    fails += expect_not_contains(cap.out, "*em*", "ansi mixed link label parses emphasis after code");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts,
                        "[plain `code` and *em*](http://link.example)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi osc8 plain-prefixed mixed link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi osc8 plain-prefixed mixed link label writes match traces");
    fails += expect_contains(cap.out, "\033]8;;http://link.example\033\\", "ansi osc8 plain-prefixed mixed link label retains its destination");
    fails += expect_not_contains(cap.out, "`code`", "ansi plain-prefixed mixed link label consumes code delimiters");
    fails += expect_not_contains(cap.out, "*em*", "ansi plain-prefixed mixed link label consumes emphasis delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts,
                        "[foo_bar_baz](http://link.example) [escaped \\*stars\\*](http://link.example)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi literal-delimiter link labels render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi literal-delimiter link label writes match traces");
    fails += expect_contains(cap.out, "foo_bar_baz", "ansi intraword underscore link label remains literal");
    fails += expect_contains(cap.out, "\\*stars\\*", "ansi escaped emphasis delimiters remain literal in link labels");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "[*foo\\* literal](https://x)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi escaped emphasis closer link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi escaped emphasis closer link label writes match traces");
    fails += expect_contains(cap.out, "*foo\\* literal", "ansi escaped emphasis closer remains literal in link labels");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.osc8 = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "[a **b *c* d** e](https://x)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi nested emphasis link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi nested emphasis link label writes match traces");
    fails += expect_contains(cap.out, "\033]8;;https://x\033\\", "ansi nested emphasis link label retains its destination");
    fails += expect_not_contains(cap.out, "**b *c* d**", "ansi nested emphasis link label consumes nested delimiters");
    fails += expect_not_contains(cap.out, "*c*", "ansi nested emphasis link label consumes inner delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "[``foo```](http://link.example)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi malformed code link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi malformed code link label writes match traces");
    fails += expect_contains(cap.out, "``foo```", "ansi malformed code link label preserves mismatched delimiters");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "[``foo``bar``](http://link.example)\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi internal code closer link label render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi internal code closer link label writes match traces");
    fails += expect_contains(cap.out, "foobar``", "ansi link label honors its first code-span closer");
    fails += expect_not_contains(cap.out, "``foo``bar``", "ansi link label consumes only the opening code delimiter");
    capture_free(&cap);

    mdf_options_init(&opts);
    opts.boring = 1;
    st = render_capture(MDF_FORMAT_ANSI, &opts, "_***both***_\n", 1, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "ansi triple nested emphasis render succeeds");
    fails += expect_trace_matches_writes(&cap, "ansi triple nested emphasis writes match traces");
    fails += expect_contains(cap.out, "both", "ansi triple nested emphasis preserves content");
    fails += expect_not_contains(cap.out, "*both", "ansi triple nested emphasis consumes opening delimiters");
    fails += expect_not_contains(cap.out, "both*", "ansi triple nested emphasis consumes closing delimiters");
    capture_free(&cap);
    return fails;
}

static int test_html_blockquote_nested_emphasis_contract(void)
{
    static const char markdown[] =
        "> _**TL;DR:** AI has the **production executive**._\n";
    mdf_options opts;
    capture cap;
    mdf_status st;
    int fails;

    fails = 0;
    mdf_options_init(&opts);
    st = render_capture(MDF_FORMAT_HTML, &opts, markdown, 3, &cap);
    fails += expect(st == MDF_OK && cap.failed == 0, "html quoted nested emphasis render succeeds");
    fails += expect_contains(cap.out, "class=\"mdf-content\"", "html quoted line has content wrapper");
    fails += expect_not_contains(cap.out, "**TL;DR:**", "html quoted nested leading strong is parsed");
    fails += expect_not_contains(cap.out, "**production executive**", "html quoted nested mid-line strong is parsed");
    fails += expect_contains(cap.out,
                             "font-weight:700;font-style:italic;\">TL;DR:",
                             "html quoted leading strong keeps bold italic style");
    fails += expect_contains(cap.out,
                             "font-weight:700;font-style:italic;\">production executive",
                             "html quoted mid-line strong keeps bold italic style");
    capture_free(&cap);
    return fails;
}

int main(void)
{
    int fails;

    fails = 0;
    fails += test_stream_trace_contract();
    fails += test_margin_contract();
    fails += test_table_contract();
    fails += test_chart_trace_contract();
    fails += test_html_link_safety_contract();
    fails += test_ansi_nested_emphasis_edge_contract();
    fails += test_html_blockquote_nested_emphasis_contract();
    return fails == 0 ? 0 : 1;
}
