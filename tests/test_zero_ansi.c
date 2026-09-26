#include <libmdf/mdf.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct capture {
    char *data;
    size_t len;
    size_t cap;
    size_t last_len;
    size_t writes;
    size_t traces;
    size_t micro_words;
    unsigned int heading_words;
    int coalesced_heading;
    int mismatch;
} capture;

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int capture_write(void *userdata, const char *src, size_t len)
{
    static const char *const words[] = {"#", "The", "Outcome-Based", "Agile", "Framework"};
    capture *c;
    char *next;
    size_t i;
    size_t cap;

    c = (capture *)userdata;
    if (c->len + len + 1 > c->cap) {
        cap = (c->len + len + 1) * 2;
        next = (char *)realloc(c->data, cap);
        if (next == NULL) return -1;
        c->data = next;
        c->cap = cap;
    }
    memcpy(c->data + c->len, src, len);
    c->len += len;
    c->data[c->len] = '\0';
    c->last_len = len;
    c->writes++;
    if (len == 2 && memcmp(src, "µ", 2) == 0) c->micro_words++;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (len == strlen(words[i]) && memcmp(src, words[i], len) == 0) {
            c->heading_words |= 1u << i;
        }
    }
    if (len >= strlen("The Outcome-Based Agile Framework") &&
        strstr(c->data + c->len - len, "The Outcome-Based Agile Framework") != NULL) {
        c->coalesced_heading = 1;
    }
    return 0;
}

static int capture_trace(void *userdata, mdf_format format, const char *src, size_t len)
{
    capture *c;

    c = (capture *)userdata;
    if (format != MDF_FORMAT_ANSI || c->traces + 1 != c->writes || len != c->last_len ||
        memcmp(c->data + c->len - c->last_len, src, len) != 0) {
        fprintf(stderr, "trace mismatch: format=%d writes=%lu traces=%lu last=%lu trace=%lu\n", (int)format, (unsigned long)c->writes, (unsigned long)c->traces, (unsigned long)c->last_len, (unsigned long)len);
        c->mismatch = 1;
    }
    c->traces++;
    return c->mismatch ? -1 : 0;
}

static int render(mdf_options *opts, const char *markdown, size_t fragment, capture *c)
{
    mdf *renderer;
    mdf_sink sink;
    mdf_status st;
    size_t off;
    size_t n;
    size_t writes;
    int fails;

    memset(c, 0, sizeof(*c));
    opts->write_trace.userdata = c;
    opts->write_trace.emit = capture_trace;
    renderer = NULL;
    sink.userdata = c;
    sink.write = capture_write;
    st = mdf_create(MDF_FORMAT_ANSI, opts, &renderer);
    if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
    for (off = 0; st == MDF_OK && off < strlen(markdown); off += n) {
        n = strlen(markdown) - off;
        if (n > fragment) n = fragment;
        st = renderer->feed(renderer, markdown + off, n);
        if (st == MDF_OK) st = renderer->flush(renderer);
    }
    if (st == MDF_OK) st = renderer->finish_document(renderer);
    if (st != MDF_OK) fprintf(stderr, "render failure: mode=%d boring=%d row=%d fragment=%lu: %s\n", (int)opts->ansi_mode, opts->boring, (int)opts->table_buffer_mode, (unsigned long)fragment, renderer == NULL ? "construction" : renderer->error(renderer));
    fails = expect(st == MDF_OK, "escape-policy render succeeds");
    if (renderer != NULL) {
        if (opts->ansi_mode == MDF_ANSI_OFF ||
            (opts->ansi_mode == MDF_ANSI_AUTO && (opts->output_fd < 0 || !isatty(opts->output_fd)))) {
            writes = c->writes;
            st = renderer->reset(renderer);
            if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
            fails += expect(st == MDF_OK && writes == c->writes, "plain reset/rebind emits no terminal cleanup");
        }
        renderer->destroy(renderer);
    }
    fails += expect(!c->mismatch && c->writes == c->traces, "every real write matches its trace exactly");
    return fails;
}

static size_t max_line(const char *s)
{
    size_t longest;
    size_t len;

    longest = len = 0;
    while (*s != '\0') {
        if (*s++ == '\n') {
            if (len > longest) longest = len;
            len = 0;
        } else len++;
    }
    return len > longest ? len : longest;
}

static int test_plain_contract(void)
{
    static const char markdown[] =
        "# The Outcome-Based Agile Framework\n\n"
        "> **Strong** and *emphasis* and ~~strike~~ with `code` and UTF-8 å界.\n\n"
        "- [Label **bold**](https://example.com/path) and <https://example.com>.\n"
        "- [x] Complete\n- [ ] Pending\n\n---\n\n"
        "```c\nint x = 3;\n```\n\n"
        "| First | Second |\n| --- | --- |\n| **value** | [link](https://example.org) |\n| next | `code` |\n\n"
        "```mdf-bar-chart\nA,1\nB,2\n```\n\n"
        "```mdf-vertical-bar-chart\nA,1\nB,2\n```\n\n"
        "```mdf-tile-chart\nA,1\nB,2\n```\n";
    mdf_options opts;
    capture c;
    int boring;
    int row;
    int fragment;
    int fails;

    fails = 0;
    for (boring = 0; boring <= 1; boring++) {
        for (row = 0; row <= 1; row++) {
            for (fragment = 1; fragment <= 4096; fragment *= 4096) {
                mdf_options_init(&opts);
                opts.ansi_mode = MDF_ANSI_OFF;
                opts.boring = boring;
                opts.osc8 = 1;
                opts.table_buffer_mode = row ? MDF_TABLE_BUFFER_ROW : MDF_TABLE_BUFFER_FULL;
                opts.width = 80;
                opts.margin_left = 2;
                opts.margin_right = 3;
                fails += render(&opts, markdown, (size_t)fragment, &c);
                fails += expect(c.data != NULL && strchr(c.data, '\033') == NULL,
                                "plain text, code, tables, charts, and OSC8 have zero ESC bytes");
                fails += expect(c.heading_words == 31u && !c.coalesced_heading,
                                "heading marker and each word are separate decided emissions");
                fails += expect(c.data != NULL && strstr(c.data, "å界") != NULL,
                                "plain mode preserves UTF-8 characters");
                fails += expect(c.data != NULL && strstr(c.data, "https://example.com/path") != NULL,
                                "plain mode preserves visible link destinations");
                free(c.data);
            }
        }
    }
    return fails;
}

static int test_destinations(void)
{
    static const char paragraph[] =
        "alpha beta gamma delta alpha beta gamma delta alpha beta gamma delta "
        "alpha beta gamma delta alpha beta gamma delta alpha beta gamma delta\n";
    mdf_options opts;
    mdf *renderer;
    capture c;
    struct winsize ws;
    int master;
    int slave;
    int pipefd[2];
    int fails;
    char *out;
    mdf_status st;

    fails = 0;
    mdf_options_init(&opts);
    fails += expect(opts.ansi_mode == MDF_ANSI_AUTO && opts.output_fd == -1 && opts.width == 0,
                    "library defaults detect destination rather than process stdout");
    renderer = NULL;
    out = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, NULL, &renderer);
    if (st == MDF_OK) st = renderer->render_cstr(renderer, "# Default\n", &out);
    fails += expect(st == MDF_OK && out != NULL && strcmp(out, "# Default\n") == 0,
                    "NULL options and string sink default to plain text");
    if (renderer != NULL) {
        renderer->string_free(renderer, out);
        renderer->destroy(renderer);
    }
    if (setenv("COLUMNS", "200", 1) != 0 || pipe(pipefd) != 0) return fails + 1;
    opts.output_fd = pipefd[1];
    fails += render(&opts, paragraph, 1, &c);
    fails += expect(c.data != NULL && max_line(c.data) <= 80 && max_line(c.data) > 60,
                    "non-terminal fd defaults to 80 regardless of COLUMNS");
    free(c.data);
    opts.ansi_mode = MDF_ANSI_ON;
    fails += render(&opts, "# Forced\n", 1, &c);
    fails += expect(c.data != NULL && strchr(c.data, '\033') != NULL, "explicit ON permits escapes for a pipe");
    free(c.data);
    close(pipefd[0]);
    close(pipefd[1]);

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) return fails + 1;
    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) { close(master); return fails + 1; }
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 100;
    ws.ws_row = 24;
    if (ioctl(slave, TIOCSWINSZ, &ws) != 0) return fails + 1;
    mdf_options_init(&opts);
    opts.output_fd = slave;
    fails += render(&opts, "# Terminal\n", 1, &c);
    fails += expect(c.data != NULL && strchr(c.data, '\033') != NULL, "AUTO themes a terminal destination");
    free(c.data);
    opts.ansi_mode = MDF_ANSI_OFF;
    opts.osc8 = 1;
    fails += render(&opts, paragraph, 1, &c);
    fails += expect(c.data != NULL && strchr(c.data, '\033') == NULL && max_line(c.data) > 80 && max_line(c.data) <= 100,
                    "OFF on a terminal keeps detected width and suppresses all escapes");
    free(c.data);
    opts.width = 30;
    fails += render(&opts, paragraph, 1, &c);
    fails += expect(c.data != NULL && max_line(c.data) <= 30, "explicit width overrides terminal detection");
    free(c.data);
    close(slave);
    close(master);
    opts.ansi_mode = (mdf_ansi_mode)99;
    renderer = NULL;
    fails += expect(mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) == MDF_ERROR_INVALID && renderer == NULL,
                    "invalid escape policies fail construction");
    return fails;
}

static int test_input_escapes(void)
{
    static const char manual[] = "\033]hidden\030\033[31\032\033[31mµ界\033[0m";
    static const char source[] =
        "# \033[31mVisible\033[0m\n\n"
        "UTF-8 µ界✓ with \033]8;;https://hidden.example/✓\033\\link\033]8;;\007.\n\n"
        "```text\n\033[1mcode\033[0m\n```\n\n"
        "| A | B |\n| --- | --- |\n| \033[32mcell\033[0m | µ |\n\n"
        "\033Psecret payload\033\\\033Xsecret\033\\\033^secret\033\\\033_secret\033\\"
        "\033(B\0337tail\n\n"
        "\302\23331mC1\302\2330m \23332mraw\2330m\n"
        "\302\235hidden\302\234after\n";
    static const char clean[] =
        "# Visible\n\n"
        "UTF-8 µ界✓ with link.\n\n"
        "```text\ncode\n```\n\n"
        "| A | B |\n| --- | --- |\n| cell | µ |\n\n"
        "tail\n\nC1 raw\nafter\n";
    mdf_options opts;
    capture actual;
    capture expected;
    mdf *renderer;
    mdf_sink sink;
    mdf_token token;
    mdf_status st;
    size_t fragment;
    size_t i;
    int fails;

    fails = 0;
    for (fragment = 1; fragment <= 4096; fragment *= 4096) {
        mdf_options_init(&opts);
        opts.ansi_mode = MDF_ANSI_OFF;
        fails += render(&opts, source, fragment, &actual);
        fails += render(&opts, clean, fragment, &expected);
        fails += expect(actual.data != NULL && expected.data != NULL && strcmp(actual.data, expected.data) == 0,
                        "input CSI, OSC8, control strings, ESC commands, and C1 are stripped before parsing");
        free(actual.data);
        free(expected.data);
    }
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    fails += render(&opts, "left\n\033]hidden\302", 1, &actual);
    fails += expect(actual.data != NULL && strcmp(actual.data, "left\n") == 0,
                    "EOF discards unfinished control payloads, including a partial UTF-8 lead");
    free(actual.data);
    memset(&actual, 0, sizeof(actual));
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    opts.write_trace.userdata = &actual;
    opts.write_trace.emit = capture_trace;
    sink.userdata = &actual;
    sink.write = capture_write;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
    memset(&token, 0, sizeof(token));
    token.type = MDF_TOKEN_TEXT;
    for (i = 0; st == MDF_OK && i < strlen(manual); i++) {
        token.text = manual + i;
        token.len = 1;
        st = renderer->write_token(renderer, &token);
    }
    if (st == MDF_OK) st = renderer->finish(renderer);
    fails += expect(st == MDF_OK && actual.data != NULL && strcmp(actual.data, "µ界\n") == 0,
                    "manual one-byte tokens strip source escapes and preserve UTF-8");
    fails += expect(!actual.mismatch && actual.writes == actual.traces, "manual plain tokens preserve sink/trace identity");
    if (renderer != NULL) renderer->destroy(renderer);
    free(actual.data);
    return fails;
}

typedef struct allocation_probe {
    size_t calls;
    size_t max_size;
    size_t limit;
    size_t call_limit;
} allocation_probe;

static void *probe_alloc(void *userdata, size_t size)
{
    allocation_probe *probe;
    probe = (allocation_probe *)userdata;
    probe->calls++;
    if (size > probe->max_size) probe->max_size = size;
    if (probe->limit > 0 && size > probe->limit) return NULL;
    if (probe->call_limit > 0 && probe->calls > probe->call_limit) return NULL;
    return malloc(size);
}

static void probe_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

static int test_bounded_input_filter(void)
{
    mdf_options opts;
    allocation_probe probe;
    capture c;
    mdf_sink sink;
    mdf *renderer;
    mdf_status st;
    size_t calls;
    size_t writes;
    int i;
    int fails;

    memset(&probe, 0, sizeof(probe));
    memset(&c, 0, sizeof(c));
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    opts.allocator.userdata = &probe;
    opts.allocator.alloc = probe_alloc;
    opts.allocator.realloc = NULL;
    opts.allocator.free = probe_free;
    opts.write_trace.userdata = &c;
    opts.write_trace.emit = capture_trace;
    renderer = NULL;
    sink.userdata = &c;
    sink.write = capture_write;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
    if (st == MDF_OK) st = renderer->feed(renderer, "alpha \033]0;", 10);
    fails = expect(st == MDF_OK && c.data != NULL && strcmp(c.data, "alpha") == 0,
                   "visible decision reaches sink before input control string completes");
    calls = probe.calls;
    writes = c.writes;
    for (i = 0; st == MDF_OK && i < 100000; i++) st = renderer->feed(renderer, "x", 1);
    fails += expect(st == MDF_OK && probe.calls == calls && c.writes == writes,
                    "100KB control payload uses constant state with no allocations or sink writes");
    if (st == MDF_OK) st = renderer->feed(renderer, "\007beta\n", 6);
    if (st == MDF_OK) st = renderer->finish_document(renderer);
    fails += expect(st == MDF_OK && c.data != NULL && strcmp(c.data, "alpha beta\n") == 0,
                    "visible input resumes immediately after a control payload");
    if (st == MDF_OK) st = renderer->reset(renderer);
    if (st == MDF_OK) st = renderer->feed(renderer, "\033[", 2);
    if (st == MDF_OK) st = renderer->reset(renderer);
    if (st == MDF_OK) st = renderer->feed(renderer, "# Recovered\n", 12);
    if (st == MDF_OK) st = renderer->finish_document(renderer);
    fails += expect(st == MDF_OK && c.data != NULL && strstr(c.data, "# Recovered\n") != NULL,
                    "reset discards an unfinished input escape without losing later content");
    fails += expect(!c.mismatch && c.writes == c.traces, "bounded filtering and recovery preserve write/trace identity");
    if (renderer != NULL) renderer->destroy(renderer);
    free(c.data);
    return fails;
}

static int test_plain_links(void)
{
    static const char *const sources[] = {
        "**before [link](https://example.org) after text**\n",
        "*before [link](https://example.org) after text*\n",
        "_before [link](https://example.org) after text_\n"
    };
    mdf_options opts;
    capture c;
    size_t i;
    int boring;
    size_t fragment;
    int fails;

    fails = 0;
    for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        for (boring = 0; boring <= 1; boring++) {
            for (fragment = 1; fragment <= 4096; fragment *= 4096) {
                mdf_options_init(&opts);
                opts.ansi_mode = MDF_ANSI_OFF;
                opts.boring = boring;
                opts.width = 24;
                fails += render(&opts, sources[i], fragment, &c);
                fails += expect(c.data != NULL && strcmp(c.data,
                                "before link\n(https://example.org)\nafter text\n") == 0,
                                "plain emphasized links emit their URL before following text");
                free(c.data);
            }
        }
    }
    return fails;
}

static int test_cancelled_escapes(void)
{
    static const char *const starts[] = {"\033", "\033[31", "\033]title", "\033Pdata",
                                        "\033Xdata", "\033^data", "\033_data", "\033("};
    mdf_options opts;
    capture c;
    char source[64];
    size_t i;
    size_t fragment;
    int cancel;
    int fails;

    fails = 0;
    for (i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        for (cancel = 0x18; cancel <= 0x1a; cancel += 2) {
            sprintf(source, "left%s%cright\n", starts[i], cancel);
            for (fragment = 1; fragment <= 4096; fragment *= 4096) {
                mdf_options_init(&opts);
                opts.ansi_mode = MDF_ANSI_OFF;
                fails += render(&opts, source, fragment, &c);
                fails += expect(c.data != NULL && strcmp(c.data, "leftright\n") == 0,
                                "CAN and SUB cancel escapes without consuming following text");
                free(c.data);
            }
        }
    }
    return fails;
}

static int test_large_manual_token(void)
{
    mdf_options opts;
    allocation_probe probe;
    capture c;
    mdf_sink sink;
    mdf *renderer;
    mdf_token token;
    mdf_status st;
    char *text;
    size_t i;
    size_t count;
    int fails;

    text = (char *)malloc(120000);
    if (text == NULL) return 1;
    for (i = 0; i < 120000; i += 3) memcpy(text + i, "µ ", 3);
    memset(&probe, 0, sizeof(probe));
    probe.limit = 65536;
    memset(&c, 0, sizeof(c));
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    opts.allocator.userdata = &probe;
    opts.allocator.alloc = probe_alloc;
    opts.allocator.free = probe_free;
    opts.write_trace.userdata = &c;
    opts.write_trace.emit = capture_trace;
    sink.userdata = &c;
    sink.write = capture_write;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
    memset(&token, 0, sizeof(token));
    token.type = MDF_TOKEN_TEXT;
    token.text = text;
    token.len = 120000;
    if (st == MDF_OK) st = renderer->write_token(renderer, &token);
    fails = expect(st == MDF_OK && c.writes > 0 && probe.max_size < 65536,
                   "large UTF-8 manual token emits decided words with bounded allocations");
    if (st == MDF_OK) st = renderer->finish(renderer);
    count = 0;
    for (i = 0; i + 1 < c.len; i++) {
        if ((unsigned char)c.data[i] == 0xc2 && (unsigned char)c.data[i + 1] == 0xb5) count++;
    }
    fails += expect(st == MDF_OK && count == 40000 && c.micro_words == 40000 && strchr(c.data, '\033') == NULL,
                    "all large-token UTF-8 words reach the sink without escapes");
    fails += expect(!c.mismatch && c.writes == c.traces, "large token preserves exact write/trace sequence");
    if (renderer != NULL) renderer->destroy(renderer);
    free(c.data);
    free(text);
    return fails;
}

static int test_chart_allocation_growth(void)
{
    mdf_options opts;
    allocation_probe probe;
    capture c;
    mdf_sink sink;
    mdf *renderer;
    mdf_token token;
    mdf_status st;
    char *text;
    size_t i;
    int fails;

    text = (char *)malloc(60003);
    if (text == NULL) return 1;
    for (i = 0; i < 60000; i += 2) memcpy(text + i, "µ", 2);
    memcpy(text + 60000, ",1\n", 3);
    memset(&probe, 0, sizeof(probe));
    /* Plenty for a single chart decision, but reject one allocation per
     * normalized character instead of depending on timing or buffer sizes. */
    probe.call_limit = 128;
    memset(&c, 0, sizeof(c));
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    opts.allocator.userdata = &probe;
    opts.allocator.alloc = probe_alloc;
    opts.allocator.free = probe_free;
    opts.write_trace.userdata = &c;
    opts.write_trace.emit = capture_trace;
    sink.userdata = &c;
    sink.write = capture_write;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
    memset(&token, 0, sizeof(token));
    token.type = MDF_TOKEN_CHART_BLOCK;
    token.level = 1;
    token.text = text;
    token.len = 60003;
    if (st == MDF_OK) st = renderer->write_token(renderer, &token);
    if (st == MDF_OK) st = renderer->finish(renderer);
    fails = expect(st == MDF_OK && probe.calls <= 128,
                   "large UTF-8 chart renders within a bounded allocation-call budget");
    fails += expect(c.data != NULL && strstr(c.data, "µµµ") != NULL && strchr(c.data, '\033') == NULL,
                    "normalized chart preserves visible UTF-8 labels without escapes");
    fails += expect(!c.mismatch && c.writes == c.traces, "chart normalization preserves sink/trace identity");
    if (renderer != NULL) renderer->destroy(renderer);
    free(c.data);
    free(text);
    return fails;
}

static int test_manual_newline_filter(void)
{
    static const struct {
        const char *prefix;
        const char *suffix;
        const char *expected;
    } cases[] = {
        {"left\033[31", "hello", "left\nhello\n"},
        {"left\033", "hello", "left\nhello\n"},
        {"left\033(", "hello", "left\nhello\n"},
        {"left\302\23331", "hello", "left\nhello\n"},
        {"left\302", "hello", "left\302\nhello\n"},
        {"left\033]title", "hidden\007hello", "lefthello\n"},
        {"left\033Pdata", "hidden\033\\hello", "lefthello\n"},
        {"left\033Xdata", "hidden\033\\hello", "lefthello\n"},
        {"left\033^data", "hidden\033\\hello", "lefthello\n"},
        {"left\033_data", "hidden\033\\hello", "lefthello\n"},
        {"left\033]title\033", "\\hidden\007hello", "lefthello\n"}
    };
    mdf_options opts;
    capture c;
    mdf_sink sink;
    mdf *renderer;
    mdf_token token;
    mdf_status st;
    size_t i;
    size_t off;
    size_t fragment;
    int fails;

    fails = 0;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (fragment = 1; fragment <= 4096; fragment *= 4096) {
            memset(&c, 0, sizeof(c));
            mdf_options_init(&opts);
            opts.ansi_mode = MDF_ANSI_OFF;
            opts.write_trace.userdata = &c;
            opts.write_trace.emit = capture_trace;
            sink.userdata = &c;
            sink.write = capture_write;
            renderer = NULL;
            st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
            if (st == MDF_OK) st = renderer->set_sink(renderer, &sink);
            memset(&token, 0, sizeof(token));
            token.type = MDF_TOKEN_TEXT;
            for (off = 0; st == MDF_OK && off < strlen(cases[i].prefix); off += token.len) {
                token.text = cases[i].prefix + off;
                token.len = strlen(cases[i].prefix) - off;
                if (token.len > fragment) token.len = fragment;
                st = renderer->write_token(renderer, &token);
            }
            token.type = MDF_TOKEN_NEWLINE;
            token.text = NULL;
            token.len = 0;
            if (st == MDF_OK) st = renderer->write_token(renderer, &token);
            token.type = MDF_TOKEN_TEXT;
            for (off = 0; st == MDF_OK && off < strlen(cases[i].suffix); off += token.len) {
                token.text = cases[i].suffix + off;
                token.len = strlen(cases[i].suffix) - off;
                if (token.len > fragment) token.len = fragment;
                st = renderer->write_token(renderer, &token);
            }
            if (st == MDF_OK) st = renderer->finish(renderer);
            fails += expect(st == MDF_OK && c.data != NULL && strcmp(c.data, cases[i].expected) == 0,
                            "manual newlines cancel CSI/ESC and remain hidden inside control strings");
            fails += expect(!c.mismatch && c.writes == c.traces, "manual newline filtering preserves exact sink/trace identity");
            if (renderer != NULL) renderer->destroy(renderer);
            free(c.data);
        }
    }
    return fails;
}

static int test_html_unchanged(void)
{
    static const char markdown[] = "# Heading\n\n**strong** and [link](https://example.com)\n\n| A | B |\n| --- | --- |\n| `code` | *em* |\n";
    mdf_options opts;
    mdf *renderer;
    char *styled;
    char *plain;
    mdf_status st;
    int format;
    int fails;

    fails = 0;
    for (format = MDF_FORMAT_HTML; format <= MDF_FORMAT_HTML_DECK; format++) {
        mdf_options_init(&opts);
        opts.html_font_source = MDF_HTML_FONT_SOURCE_EXTERNAL;
        opts.ansi_mode = MDF_ANSI_ON;
        renderer = NULL;
        styled = plain = NULL;
        st = mdf_create((mdf_format)format, &opts, &renderer);
        if (st == MDF_OK) st = renderer->render_cstr(renderer, markdown, &styled);
        if (renderer != NULL) renderer->destroy(renderer);
        opts.ansi_mode = MDF_ANSI_OFF;
        if (st == MDF_OK) st = mdf_create((mdf_format)format, &opts, &renderer);
        if (st == MDF_OK) st = renderer->render_cstr(renderer, markdown, &plain);
        fails += expect(st == MDF_OK && styled != NULL && plain != NULL && strcmp(styled, plain) == 0,
                        "HTML/deck output is identical with ANSI policy ON or OFF");
        if (renderer != NULL) {
            renderer->string_free(renderer, styled);
            renderer->string_free(renderer, plain);
            renderer->destroy(renderer);
        }
    }
    return fails;
}

int main(void)
{
    int fails;

    fails = test_plain_contract();
    fails += test_destinations();
    fails += test_html_unchanged();
    fails += test_input_escapes();
    fails += test_bounded_input_filter();
    fails += test_plain_links();
    fails += test_cancelled_escapes();
    fails += test_large_manual_token();
    fails += test_chart_allocation_growth();
    fails += test_manual_newline_filter();
    return fails == 0 ? 0 : 1;
}
