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

int main(void)
{
    int fails;

    fails = 0;
    fails += test_stream_trace_contract();
    fails += test_margin_contract();
    fails += test_table_contract();
    fails += test_html_link_safety_contract();
    return fails == 0 ? 0 : 1;
}
