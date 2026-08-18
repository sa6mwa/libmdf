#include "libmdf/mdf.h"
#include "mdf_internal.h"

#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct file_source {
    FILE *fp;
    size_t max_chunk;
    double delay_seconds;
    size_t reads;
} file_source;

static int sleep_seconds(double seconds)
{
    struct timespec req;
    struct timespec rem;
    time_t whole;
    double frac;

    if (seconds <= 0.0) {
        return 0;
    }
    whole = (time_t)seconds;
    frac = seconds - (double)whole;
    req.tv_sec = whole;
    req.tv_nsec = (long)(frac * 1000000000.0 + 0.5);
    if (req.tv_nsec >= 1000000000L) {
        req.tv_sec += 1;
        req.tv_nsec -= 1000000000L;
    }
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return -1;
        }
        req = rem;
    }
    return 0;
}

static size_t file_read(void *userdata, char *dst, size_t cap, int *err)
{
    file_source *src;
    int fd;
    ssize_t n;

    src = (file_source *)userdata;
    if (src->max_chunk > 0 && cap > src->max_chunk) {
        cap = src->max_chunk;
    }
    if (src->reads > 0) {
        if (sleep_seconds(src->delay_seconds) != 0) {
            *err = errno == 0 ? EIO : errno;
            return 0;
        }
    }
    fd = fileno(src->fp);
    if (fd < 0) {
        *err = errno == 0 ? EIO : errno;
        return 0;
    }
    do {
        n = read(fd, dst, cap);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        *err = errno == 0 ? EIO : errno;
        return 0;
    }
    if (n > 0) {
        src->reads++;
    }
    return (size_t)n;
}


typedef struct file_sink {
    FILE *fp;
} file_sink;

typedef struct trace_output {
    FILE *trace_fp;
    unsigned long seq;
} trace_output;

#define CMDF_DEFAULT_SIMULATE_DELAY_SECONDS 0.02

static int file_write(void *userdata, const char *src, size_t len)
{
    file_sink *sink;
    int fd;
    size_t off;

    sink = (file_sink *)userdata;
    fd = fileno(sink->fp);
    if (fd < 0) {
        return -1;
    }
    if (len == 0) {
        if (write(fd, "", 0) < 0) {
            return -1;
        }
        return 0;
    }
    off = 0;
    while (off < len) {
        ssize_t n;

        n = write(fd, src + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int trace_write_base64(FILE *fp, const unsigned char *src, size_t len)
{
    static const char base64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    unsigned int value;

    i = 0;
    while (i + 3 <= len) {
        value = ((unsigned int)src[i] << 16) |
                ((unsigned int)src[i + 1] << 8) |
                (unsigned int)src[i + 2];
        if (fputc(base64_table[(value >> 18) & 63], fp) == EOF ||
            fputc(base64_table[(value >> 12) & 63], fp) == EOF ||
            fputc(base64_table[(value >> 6) & 63], fp) == EOF ||
            fputc(base64_table[value & 63], fp) == EOF) {
            return -1;
        }
        i += 3;
    }
    if (i < len) {
        value = (unsigned int)src[i] << 16;
        if (i + 1 < len) {
            value |= (unsigned int)src[i + 1] << 8;
        }
        if (fputc(base64_table[(value >> 18) & 63], fp) == EOF ||
            fputc(base64_table[(value >> 12) & 63], fp) == EOF) {
            return -1;
        }
        if (i + 1 < len) {
            if (fputc(base64_table[(value >> 6) & 63], fp) == EOF) {
                return -1;
            }
        } else {
            if (fputc('=', fp) == EOF) {
                return -1;
            }
        }
        if (fputc('=', fp) == EOF) {
            return -1;
        }
    }
    return 0;
}

static int trace_log_emit(trace_output *trace, mdf_format format, const char *src, size_t len)
{
    const char *format_name;

    format_name = format == MDF_FORMAT_HTML ? "html" : "ansi";
    trace->seq++;
    if (fprintf(trace->trace_fp,
                "{\"seq\":%lu,\"format\":\"%s\",\"op\":\"emit\",\"bytes\":%lu,\"data_b64\":\"",
                trace->seq,
                format_name,
                (unsigned long)len) < 0) {
        return -1;
    }
    if (trace_write_base64(trace->trace_fp, (const unsigned char *)src, len) != 0) {
        return -1;
    }
    if (fputs("\"}\n", trace->trace_fp) == EOF) {
        return -1;
    }
    if (fflush(trace->trace_fp) != 0) {
        return -1;
    }
    return 0;
}

static int trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    return trace_log_emit((trace_output *)userdata, format, src, len);
}

static void usage(FILE *fp)
{
    fprintf(fp, "cmdf %s\n", LIBMDF_VERSION);
    fprintf(fp, "Usage: cmdf [flags] [input]\n\n");
    fprintf(fp, "Flags:\n");
    fprintf(fp, "  -h, --help                 Show help\n");
    fprintf(fp, "  -V, --version              Show version\n");
    fprintf(fp, "      --html                 Render HTML\n");
    fprintf(fp, "      --deck                 Render HTML slide deck\n");
    fprintf(fp, "  -b, --boring               Boring ANSI output\n");
    fprintf(fp, "  -o, --output PATH          Output file\n");
    fprintf(fp, "  -t, --theme NAME           Theme name\n");
    fprintf(fp, "  -T, --title TITLE          HTML document title\n");
    fprintf(fp, "  -w, --width WIDTH          ANSI output width\n");
    fprintf(fp, "      --margin-left N        ANSI left margin in spaces\n");
    fprintf(fp, "      --margin-right N       ANSI right margin in spaces\n");
    fprintf(fp, "  -8, --osc8 MODE            OSC8 mode: auto|on|off\n");
    fprintf(fp, "      --list-themes          List available themes\n");
    fprintf(fp, "      --html-content-width N HTML content max width in ch\n");
    fprintf(fp, "      --html-disable-embedded-font Use external JetBrains Mono web fonts\n");
    fprintf(fp, "      --html-font-uri URI    External regular JetBrains Mono font URI\n");
    fprintf(fp, "      --html-font-italic-uri URI External italic JetBrains Mono font URI\n");
    fprintf(fp, "      --html-dump-font       Write external local font URIs to their paths\n");
    fprintf(fp, "      --html-dump-font-force Replace existing font files while dumping\n");
    fprintf(fp, "      --html-dump-font-path DIR Write and reference paired fonts in DIR\n");
    fprintf(fp, "      --html-dump-font-regular-path PATH Override regular font destination\n");
    fprintf(fp, "      --html-dump-font-italic-path PATH Override italic font destination\n");
    fprintf(fp, "  -x, --transition MODE      Deck transition: fade|cross|hard\n");
    fprintf(fp, "      --slide-numbers        Show deck slide numbers after the first slide\n");
    fprintf(fp, "      --deck-center-front-text Center-align first-slide paragraph text\n");
    fprintf(fp, "      --table-buffer MODE    Table buffering mode: full|row\n");
    fprintf(fp, "      --table-wire MODE      Table wire mode: line|ascii|space\n");
    fprintf(fp, "      --simulate             Simulate input streaming with default chunk\n");
    fprintf(fp, "      --simulate-chunk N     Simulate input streaming with max N bytes per read\n");
    fprintf(fp, "      --simulate-delay D     Delay duration between simulated reads; implies --simulate\n");
    fprintf(fp, "      --trace-writes PATH    Write ANSI renderer-emission NDJSON trace to PATH or - for stderr\n");
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value <= 0 || value > 10000) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static const char *html_local_font_path(const char *uri)
{
    if (uri == NULL || uri[0] == '\0' ||
        strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
        return NULL;
    }
    if (strncmp(uri, "file://", 7) == 0) {
        uri += 7;
        if (uri[0] == '\0' || uri[0] != '/') {
            return NULL;
        }
    }
    return uri;
}

static int html_font_path_join(char *dst, size_t cap, const char *dir, const char *name)
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

static int parse_nonnegative_int(const char *s, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < 0 || value > 10000) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int parse_double_positive(const char *s, double *out)
{
    char *end;
    double value;

    errno = 0;
    value = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || value <= 0.0) {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_duration_seconds(const char *s, double *out)
{
    double total;
    const char *p;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    total = 0.0;
    p = s;
    while (*p != '\0') {
        char *end;
        double value;
        double scale;

        errno = 0;
        value = strtod(p, &end);
        if (errno != 0 || end == p || value < 0.0) {
            return -1;
        }
        p = end;
        if (p[0] == 'n' && p[1] == 's') {
            scale = 0.000000001;
            p += 2;
        } else if (p[0] == 'u' && p[1] == 's') {
            scale = 0.000001;
            p += 2;
        } else if ((unsigned char)p[0] == 0xc2 && (unsigned char)p[1] == 0xb5 && p[2] == 's') {
            scale = 0.000001;
            p += 3;
        } else if (p[0] == 'm' && p[1] == 's') {
            scale = 0.001;
            p += 2;
        } else if (p[0] == 's') {
            scale = 1.0;
            p += 1;
        } else if (p[0] == 'm') {
            scale = 60.0;
            p += 1;
        } else if (p[0] == 'h') {
            scale = 3600.0;
            p += 1;
        } else {
            return -1;
        }
        total += value * scale;
    }
    if (total <= 0.0) {
        return -1;
    }
    *out = total;
    return 0;
}

static int parse_table_buffer(const char *s, mdf_table_buffer_mode *out)
{
    if (strcmp(s, "full") == 0 || strcmp(s, "") == 0) {
        *out = MDF_TABLE_BUFFER_FULL;
        return 0;
    }
    if (strcmp(s, "row") == 0) {
        *out = MDF_TABLE_BUFFER_ROW;
        return 0;
    }
    return -1;
}

static int parse_table_wire(const char *s, mdf_table_wire_mode *out)
{
    if (strcmp(s, "line") == 0 || strcmp(s, "") == 0) {
        *out = MDF_TABLE_WIRE_LINE;
        return 0;
    }
    if (strcmp(s, "ascii") == 0) {
        *out = MDF_TABLE_WIRE_ASCII;
        return 0;
    }
    if (strcmp(s, "space") == 0) {
        *out = MDF_TABLE_WIRE_SPACE;
        return 0;
    }
    return -1;
}

static int parse_deck_transition(const char *s, mdf_deck_transition *out)
{
    if (strcmp(s, "fade") == 0 || strcmp(s, "") == 0) {
        *out = MDF_DECK_TRANSITION_FADE;
        return 0;
    }
    if (strcmp(s, "cross") == 0) {
        *out = MDF_DECK_TRANSITION_CROSS;
        return 0;
    }
    if (strcmp(s, "hard") == 0) {
        *out = MDF_DECK_TRANSITION_HARD;
        return 0;
    }
    return -1;
}

static void print_themes(void)
{
    size_t i;

    for (i = 0; i < mdf_theme_count(); i++) {
        printf("%s\n", mdf_theme_name(i));
    }
}

static int has_html_extension(const char *path)
{
    const char *dot;
    size_t len;

    if (path == NULL) {
        return 0;
    }
    dot = strrchr(path, '.');
    if (dot == NULL) {
        return 0;
    }
    len = strlen(dot);
    if (len == 5) {
        return (dot[0] == '.') &&
               (dot[1] == 'h' || dot[1] == 'H') &&
               (dot[2] == 't' || dot[2] == 'T') &&
               (dot[3] == 'm' || dot[3] == 'M') &&
               (dot[4] == 'l' || dot[4] == 'L');
    }
    if (len == 4) {
        return (dot[0] == '.') &&
               (dot[1] == 'h' || dot[1] == 'H') &&
               (dot[2] == 't' || dot[2] == 'T') &&
               (dot[3] == 'm' || dot[3] == 'M');
    }
    return 0;
}

int main(int argc, char **argv)
{
    int opt;
    int rc;
    const char *out_path;
    const char *in_path;
    size_t simulate_chunk;
    double simulate_delay_seconds;
    int width_flag;
    int list_themes;
    const char *theme_name;
    int theme_explicit;
    const char *title_override;
    const char *trace_writes_path;
    int simulate_enabled;
    int format_explicit;
    int deck_requested;
    int deck_option_seen;
    int html_content_width_flag;
    int html_disable_embedded_font;
    int html_dump_font;
    int html_dump_font_force;
    const char *html_font_uri;
    const char *html_font_italic_uri;
    const char *html_font_path;
    const char *html_font_regular_path;
    const char *html_font_italic_path;
    const char *regular_font_dump_path;
    const char *italic_font_dump_path;
    char html_font_regular_path_buf[4096];
    char html_font_italic_path_buf[4096];
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"html", no_argument, NULL, 'H'},
        {"deck", no_argument, NULL, 1009},
        {"boring", no_argument, NULL, 'b'},
        {"output", required_argument, NULL, 'o'},
        {"theme", required_argument, NULL, 't'},
        {"title", required_argument, NULL, 'T'},
        {"width", required_argument, NULL, 'w'},
        {"margin-left", required_argument, NULL, 1007},
        {"margin-right", required_argument, NULL, 1008},
        {"osc8", required_argument, NULL, '8'},
        {"list-themes", no_argument, NULL, 1000},
        {"html-content-width", required_argument, NULL, 1001},
        {"html-disable-embedded-font", no_argument, NULL, 1012},
        {"html-font-uri", required_argument, NULL, 1013},
        {"html-font-italic-uri", required_argument, NULL, 1014},
        {"html-dump-font", no_argument, NULL, 1015},
        {"html-dump-font-force", no_argument, NULL, 1018},
        {"html-dump-font-path", required_argument, NULL, 1016},
        {"html-dump-font-regular-path", required_argument, NULL, 1019},
        {"html-dump-font-italic-path", required_argument, NULL, 1017},
        {"table-buffer", required_argument, NULL, 1002},
        {"table-wire", required_argument, NULL, 1003},
        {"simulate", no_argument, NULL, 1004},
        {"simulate-chunk", required_argument, NULL, 'S'},
        {"simulate-delay", required_argument, NULL, 1005},
        {"trace-writes", required_argument, NULL, 1006},
        {"transition", required_argument, NULL, 'x'},
        {"slide-numbers", no_argument, NULL, 1010},
        {"deck-center-front-text", no_argument, NULL, 1011},
        {NULL, 0, NULL, 0}
    };
    FILE *in_fp;
    FILE *out_fp;
    mdf_options opts;
    mdf_format format;
    mdf *renderer;
    file_source source_data;
    file_sink sink_data;
    trace_output trace_data;
    mdf_source source;
    mdf_sink sink;
    mdf_status st;
    FILE *trace_fp;

    mdf_options_init(&opts);
    opts.osc8 = mdf_detect_osc8_support();
    format = MDF_FORMAT_ANSI;
    out_path = NULL;
    simulate_chunk = 0;
    simulate_delay_seconds = 0.0;
    width_flag = 0;
    list_themes = 0;
    theme_name = NULL;
    theme_explicit = 0;
    title_override = NULL;
    trace_writes_path = NULL;
    simulate_enabled = 0;
    format_explicit = 0;
    deck_requested = 0;
    deck_option_seen = 0;
    html_content_width_flag = 0;
    html_disable_embedded_font = 0;
    html_dump_font = 0;
    html_dump_font_force = 0;
    html_font_uri = NULL;
    html_font_italic_uri = NULL;
    html_font_path = NULL;
    html_font_regular_path = NULL;
    html_font_italic_path = NULL;
    regular_font_dump_path = NULL;
    italic_font_dump_path = NULL;
    memset(&trace_data, 0, sizeof(trace_data));
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVHbo:t:T:w:8:S:x:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            printf("%s\n", LIBMDF_VERSION);
            return 0;
        case 'H':
            format = MDF_FORMAT_HTML;
            format_explicit = 1;
            break;
        case 1009:
            format = MDF_FORMAT_HTML_DECK;
            format_explicit = 1;
            deck_requested = 1;
            break;
        case 'b':
            opts.boring = 1;
            break;
        case 'o':
            out_path = optarg;
            break;
        case 't':
            theme_name = optarg;
            theme_explicit = 1;
            break;
        case 'T':
            title_override = optarg;
            break;
        case 'w':
            if (parse_int(optarg, &width_flag) != 0) {
                fprintf(stderr, "cmdf: invalid width: %s\n", optarg);
                return 2;
            }
            opts.width = width_flag;
            break;
        case '8':
            if (strcmp(optarg, "on") == 0) {
                opts.osc8 = 1;
            } else if (strcmp(optarg, "off") == 0) {
                opts.osc8 = 0;
            } else if (strcmp(optarg, "auto") == 0) {
                opts.osc8 = mdf_detect_osc8_support();
            } else {
                fprintf(stderr, "cmdf: invalid OSC8 mode: %s\n", optarg);
                return 2;
            }
            break;
        case 'S':
            {
                int parsed;

                if (parse_int(optarg, &parsed) != 0) {
                    fprintf(stderr, "cmdf: invalid simulate chunk: %s\n", optarg);
                    return 2;
                }
                simulate_chunk = (size_t)parsed;
            }
            break;
        case 'x':
            deck_option_seen = 1;
            if (parse_deck_transition(optarg, &opts.deck_transition) != 0) {
                fprintf(stderr, "cmdf: invalid deck transition: %s (expected fade, cross, or hard)\n", optarg);
                return 2;
            }
            break;
        case 1000:
            list_themes = 1;
            break;
        case 1001:
            if (parse_double_positive(optarg, &opts.html_content_width_ch) != 0) {
                fprintf(stderr, "cmdf: invalid html content width: %s\n", optarg);
                return 2;
            }
            html_content_width_flag = 1;
            break;
        case 1012:
            html_disable_embedded_font = 1;
            break;
        case 1013:
            html_font_uri = optarg;
            break;
        case 1014:
            html_font_italic_uri = optarg;
            break;
        case 1015:
            html_dump_font = 1;
            break;
        case 1018:
            html_dump_font = 1;
            html_dump_font_force = 1;
            break;
        case 1016:
            html_font_path = optarg;
            break;
        case 1017:
            html_font_italic_path = optarg;
            break;
        case 1019:
            html_font_regular_path = optarg;
            break;
        case 1002:
            if (parse_table_buffer(optarg, &opts.table_buffer_mode) != 0) {
                fprintf(stderr, "cmdf: invalid table buffer mode: %s\n", optarg);
                return 2;
            }
            break;
        case 1003:
            if (parse_table_wire(optarg, &opts.table_wire_mode) != 0) {
                fprintf(stderr, "cmdf: invalid table wire mode: %s\n", optarg);
                return 2;
            }
            break;
        case 1004:
            simulate_chunk = 3;
            simulate_delay_seconds = CMDF_DEFAULT_SIMULATE_DELAY_SECONDS;
            simulate_enabled = 1;
            break;
        case 1005:
            if (parse_duration_seconds(optarg, &simulate_delay_seconds) != 0) {
                fprintf(stderr, "cmdf: invalid simulate delay: %s\n", optarg);
                return 2;
            }
            simulate_enabled = 1;
            break;
        case 1006:
            trace_writes_path = optarg;
            break;
        case 1007:
            if (parse_nonnegative_int(optarg, &opts.margin_left) != 0) {
                fprintf(stderr, "cmdf: invalid left margin: %s\n", optarg);
                return 2;
            }
            break;
        case 1008:
            if (parse_nonnegative_int(optarg, &opts.margin_right) != 0) {
                fprintf(stderr, "cmdf: invalid right margin: %s\n", optarg);
                return 2;
            }
            break;
        case 1010:
            deck_option_seen = 1;
            opts.slide_numbers = 1;
            break;
        case 1011:
            deck_option_seen = 1;
            opts.deck_center_front_text = 1;
            break;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (list_themes) {
        print_themes();
        return 0;
    }
    if (theme_explicit && !mdf_theme_exists(theme_name)) {
        fprintf(stderr, "cmdf: unknown theme %s\n", theme_name);
        print_themes();
        return 2;
    }
    opts.theme_name = theme_name;
    if (deck_requested) {
        format = MDF_FORMAT_HTML_DECK;
    }
    if (!format_explicit && has_html_extension(out_path)) {
        format = MDF_FORMAT_HTML;
        fprintf(stderr, "cmdf: warning: inferring --html from output path %s\n", out_path);
    }
    if (deck_option_seen && !deck_requested) {
        fprintf(stderr, "cmdf: deck options require --deck\n");
        return 2;
    }
    if ((format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) && opts.table_wire_mode == MDF_TABLE_WIRE_ASCII) {
        fprintf(stderr, "cmdf: --table-wire ascii is not supported with HTML output; use line or space\n");
        return 2;
    }
    if (format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) {
        if (width_flag > 0 && !html_content_width_flag) {
            opts.html_content_width_ch = (double)width_flag;
        }
    }
    if ((html_disable_embedded_font || html_font_uri != NULL || html_font_italic_uri != NULL ||
         html_dump_font || html_font_path != NULL || html_font_regular_path != NULL || html_font_italic_path != NULL) &&
        format != MDF_FORMAT_HTML && format != MDF_FORMAT_HTML_DECK) {
        fprintf(stderr, "cmdf: HTML font options require HTML or deck output\n");
        return 2;
    }
    if ((html_font_uri == NULL) != (html_font_italic_uri == NULL)) {
        fprintf(stderr, "cmdf: --html-font-uri and --html-font-italic-uri must be used together\n");
        return 2;
    }
    if (html_font_path != NULL) {
        if (html_font_path_join(html_font_regular_path_buf, sizeof(html_font_regular_path_buf),
                                html_font_path, "JetBrainsMono-Regular.woff2") != 0 ||
            html_font_path_join(html_font_italic_path_buf, sizeof(html_font_italic_path_buf),
                                html_font_path, "JetBrainsMono-Italic.woff2") != 0) {
            fprintf(stderr, "cmdf: HTML font path is too long\n");
            return 2;
        }
        if (html_font_regular_path == NULL) {
            html_font_regular_path = html_font_regular_path_buf;
        }
        if (html_font_italic_path == NULL) {
            html_font_italic_path = html_font_italic_path_buf;
        }
    }
    if ((html_font_regular_path == NULL) != (html_font_italic_path == NULL)) {
        fprintf(stderr, "cmdf: paired font paths require both regular and italic destinations\n");
        return 2;
    }
    if (html_font_regular_path != NULL) {
        html_font_uri = html_font_regular_path;
        html_font_italic_uri = html_font_italic_path;
        html_disable_embedded_font = 1;
        html_dump_font = 1;
    }
    if (html_dump_font) {
        regular_font_dump_path = html_local_font_path(html_font_uri);
        italic_font_dump_path = html_local_font_path(html_font_italic_uri);
        if (regular_font_dump_path == NULL || italic_font_dump_path == NULL) {
            fprintf(stderr, "cmdf: --html-dump-font requires paired local paths or file:// URIs\n");
            return 2;
        }
        st = html_dump_font_force ?
            mdf_dump_html_jetbrains_mono_font_force(regular_font_dump_path, italic_font_dump_path) :
            mdf_dump_html_jetbrains_mono_font(regular_font_dump_path, italic_font_dump_path);
        if (st != MDF_OK) {
            fprintf(stderr, "cmdf: dump HTML fonts: %s\n", mdf_status_string(st));
            return 1;
        }
    }
    if (trace_writes_path != NULL && format != MDF_FORMAT_ANSI) {
        fprintf(stderr, "cmdf: --trace-writes is only supported for ANSI output\n");
        return 2;
    }
    if (format == MDF_FORMAT_ANSI && width_flag == 0) {
        opts.width = mdf_terminal_width(STDOUT_FILENO, 80);
    }
    if ((simulate_enabled || simulate_delay_seconds > 0.0) && simulate_chunk == 0) {
        simulate_chunk = 3;
    }
    if (argc - optind > 1) {
        fprintf(stderr, "cmdf: expected at most one input\n");
        return 2;
    }
    in_path = argc - optind == 1 ? argv[optind] : NULL;
    in_fp = stdin;
    out_fp = stdout;
    trace_fp = NULL;
    if (in_path != NULL) {
        in_fp = fopen(in_path, "rb");
        if (in_fp == NULL) {
            fprintf(stderr, "cmdf: open input %s: %s\n", in_path, strerror(errno));
            return 1;
        }
    }
    if (out_path != NULL) {
        out_fp = fopen(out_path, "wb");
        if (out_fp == NULL) {
            fprintf(stderr, "cmdf: open output %s: %s\n", out_path, strerror(errno));
            if (in_fp != stdin) fclose(in_fp);
            return 1;
        }
    }
    if (trace_writes_path != NULL) {
        if (strcmp(trace_writes_path, "-") == 0) {
            trace_fp = stderr;
        } else {
            trace_fp = fopen(trace_writes_path, "wb");
            if (trace_fp == NULL) {
                fprintf(stderr, "cmdf: open trace output %s: %s\n", trace_writes_path, strerror(errno));
                if (out_fp != stdout) fclose(out_fp);
                if (in_fp != stdin) fclose(in_fp);
                return 1;
            }
        }
        trace_data.trace_fp = trace_fp;
        trace_data.seq = 0;
        opts.write_trace.userdata = &trace_data;
        opts.write_trace.emit = trace_emit;
    }
    renderer = NULL;
    st = mdf_create(format, &opts, &renderer);
    if (st != MDF_OK) {
        fprintf(stderr, "cmdf: create renderer: %s\n", mdf_status_string(st));
        if (trace_fp != NULL && trace_fp != stderr) fclose(trace_fp);
        if (out_fp != stdout) fclose(out_fp);
        if (in_fp != stdin) fclose(in_fp);
        return 1;
    }
    if ((format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) &&
        (html_disable_embedded_font || html_font_uri != NULL)) {
        st = mdf_set_html_jetbrains_mono_font_uris(renderer, html_font_uri, html_font_italic_uri);
        if (st != MDF_OK) {
            fprintf(stderr, "cmdf: set HTML font URIs: %s\n", mdf_status_string(st));
            renderer->destroy(renderer);
            if (trace_fp != NULL && trace_fp != stderr) fclose(trace_fp);
            if (out_fp != stdout) fclose(out_fp);
            if (in_fp != stdin) fclose(in_fp);
            return 1;
        }
    }
    if ((format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) && title_override != NULL) {
        st = mdf_set_html_title(renderer, title_override);
        if (st != MDF_OK) {
            fprintf(stderr, "cmdf: set HTML title: %s\n", mdf_status_string(st));
            renderer->destroy(renderer);
            if (trace_fp != NULL && trace_fp != stderr) fclose(trace_fp);
            if (out_fp != stdout) fclose(out_fp);
            if (in_fp != stdin) fclose(in_fp);
            return 1;
        }
    }
    source_data.fp = in_fp;
    source_data.max_chunk = simulate_chunk;
    source_data.delay_seconds = simulate_delay_seconds;
    source_data.reads = 0;
    sink_data.fp = out_fp;
    source.userdata = &source_data;
    source.read = file_read;
    sink.userdata = &sink_data;
    sink.write = file_write;
    st = renderer->render(renderer, &source, &sink);
    rc = 0;
    if (st != MDF_OK) {
        fprintf(stderr, "cmdf: render: %s: %s\n", mdf_status_string(st), renderer->error(renderer));
        rc = 1;
    }
    renderer->destroy(renderer);
    if (trace_fp != NULL && trace_fp != stderr && fclose(trace_fp) != 0) {
        fprintf(stderr, "cmdf: close trace output: %s\n", strerror(errno));
        rc = 1;
    }
    if (out_fp != stdout && fclose(out_fp) != 0) {
        fprintf(stderr, "cmdf: close output: %s\n", strerror(errno));
        rc = 1;
    }
    if (in_fp != stdin) {
        fclose(in_fp);
    }
    return rc;
}
