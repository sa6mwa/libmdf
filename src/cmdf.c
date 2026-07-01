#include "libmdf/mdf.h"
#include "cmdf_fonts.h"
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

typedef struct prefixed_file_source {
    FILE *fp;
    char *prefix;
    size_t prefix_len;
    size_t prefix_off;
    size_t max_chunk;
    double delay_seconds;
    size_t reads;
} prefixed_file_source;

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

static size_t prefixed_file_read(void *userdata, char *dst, size_t cap, int *err)
{
    prefixed_file_source *src;
    int fd;
    size_t n;
    ssize_t got;

    src = (prefixed_file_source *)userdata;
    if (src->max_chunk > 0 && cap > src->max_chunk) {
        cap = src->max_chunk;
    }
    if (src->reads > 0) {
        if (sleep_seconds(src->delay_seconds) != 0) {
            *err = errno == 0 ? EIO : errno;
            return 0;
        }
    }
    if (src->prefix_off < src->prefix_len) {
        n = src->prefix_len - src->prefix_off;
        if (n > cap) {
            n = cap;
        }
        memcpy(dst, src->prefix + src->prefix_off, n);
        src->prefix_off += n;
        if (n > 0) {
            src->reads++;
        }
        return n;
    }
    fd = fileno(src->fp);
    if (fd < 0) {
        *err = errno == 0 ? EIO : errno;
        return 0;
    }
    do {
        got = read(fd, dst, cap);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        *err = errno == 0 ? EIO : errno;
        return 0;
    }
    if (got > 0) {
        src->reads++;
    }
    return (size_t)got;
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

static int append_prescan_byte(char **buf, size_t *len, size_t *cap, char ch)
{
    char *next;
    size_t new_cap;

    if (*len + 1 < *len) {
        return -1;
    }
    if (*len + 1 > *cap) {
        new_cap = *cap == 0 ? 512 : *cap;
        while (new_cap < *len + 1) {
            if (new_cap > ((size_t)-1) / 2) {
                return -1;
            }
            new_cap *= 2;
        }
        next = (char *)realloc(*buf, new_cap);
        if (next == NULL) {
            return -1;
        }
        *buf = next;
        *cap = new_cap;
    }
    (*buf)[*len] = ch;
    *len += 1;
    return 0;
}

static int copy_trimmed_title(const char *start, const char *end, char **out)
{
    char *title;
    size_t len;

    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '#')) {
        if (end[-1] == '#') {
            const char *hash = end;

            while (hash > start && hash[-1] == '#') {
                hash--;
            }
            if (hash == start || (hash[-1] != ' ' && hash[-1] != '\t')) {
                break;
            }
            end = hash - 1;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
                end--;
            }
            continue;
        }
        end--;
    }
    len = (size_t)(end - start);
    title = (char *)malloc(len + 1);
    if (title == NULL) {
        return -1;
    }
    memcpy(title, start, len);
    title[len] = '\0';
    *out = title;
    return 0;
}

static int detect_html_title_line(const char *line_start, const char *line_end, char **title_out, int *decided)
{
    const char *q;
    int spaces;
    int hashes;

    q = line_start;
    spaces = 0;
    while (q < line_end && *q == ' ' && spaces < 4) {
        q++;
        spaces++;
    }
    if (q == line_end) {
        *decided = 0;
        return 0;
    }
    *decided = 1;
    if (spaces < 4 && *q == '#') {
        hashes = 0;
        while (q < line_end && *q == '#' && hashes < 7) {
            q++;
            hashes++;
        }
        if (hashes >= 1 && hashes <= 6 && (q == line_end || *q == ' ' || *q == '\t')) {
            return copy_trimmed_title(q, line_end, title_out);
        }
    }
    return 0;
}

static int read_title_prescan_byte(int fd, char *ch, char **prefix, size_t *prefix_len, size_t *prefix_cap)
{
    ssize_t n;

    do {
        n = read(fd, ch, 1);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (append_prescan_byte(prefix, prefix_len, prefix_cap, *ch) != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 1;
}

static int detect_html_title_from_file(FILE *fp, char **title_out, char **prefix_out, size_t *prefix_len_out)
{
    char *prefix;
    size_t prefix_len;
    size_t prefix_cap;
    size_t line_start;
    int fd;

    *title_out = NULL;
    *prefix_out = NULL;
    *prefix_len_out = 0;
    fd = fileno(fp);
    if (fd < 0) {
        return -1;
    }
    prefix = NULL;
    prefix_len = 0;
    prefix_cap = 0;
    for (;;) {
        char ch;
        int spaces;
        int hash_count;
        int rc;
        int saw_tab;

        line_start = prefix_len;
        spaces = 0;
        saw_tab = 0;
        for (;;) {
            rc = read_title_prescan_byte(fd, &ch, &prefix, &prefix_len, &prefix_cap);
            if (rc < 0) {
                free(prefix);
                return -1;
            }
            if (rc == 0) {
                break;
            }
            if (ch == '\n') {
                break;
            }
            if (ch == '\r') {
                continue;
            }
            if (ch == '\t') {
                saw_tab = 1;
                continue;
            }
            if (ch == ' ') {
                spaces++;
                continue;
            }
            if (!saw_tab && spaces < 4 && ch == '#') {
                hash_count = 1;
                for (;;) {
                    rc = read_title_prescan_byte(fd, &ch, &prefix, &prefix_len, &prefix_cap);
                    if (rc < 0) {
                        free(prefix);
                        return -1;
                    }
                    if (rc == 0) {
                        break;
                    }
                    if (ch != '#') {
                        break;
                    }
                    hash_count++;
                }
                if (hash_count >= 1 && hash_count <= 6 &&
                    (rc == 0 || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t')) {
                    while (rc > 0 && ch != '\n') {
                        rc = read_title_prescan_byte(fd, &ch, &prefix, &prefix_len, &prefix_cap);
                        if (rc < 0) {
                            free(prefix);
                            return -1;
                        }
                    }
                    {
                        const char *line;
                        const char *line_end;
                        int decided;

                        line = prefix + line_start;
                        line_end = prefix + prefix_len;
                        while (line_end > line && (line_end[-1] == '\n' || line_end[-1] == '\r')) {
                            line_end--;
                        }
                        decided = 0;
                        if (detect_html_title_line(line, line_end, title_out, &decided) != 0) {
                            free(prefix);
                            return -1;
                        }
                    }
                }
                *prefix_out = prefix;
                *prefix_len_out = prefix_len;
                return 0;
            }
            *prefix_out = prefix;
            *prefix_len_out = prefix_len;
            return 0;
        }
        if (prefix_len == line_start) {
            break;
        }
    }
    *prefix_out = prefix;
    *prefix_len_out = prefix_len;
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
    const char *title_override;
    const char *trace_writes_path;
    int simulate_enabled;
    int format_explicit;
    int html_content_width_flag;
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"html", no_argument, NULL, 'H'},
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
        {"table-buffer", required_argument, NULL, 1002},
        {"table-wire", required_argument, NULL, 1003},
        {"simulate", no_argument, NULL, 1004},
        {"simulate-chunk", required_argument, NULL, 'S'},
        {"simulate-delay", required_argument, NULL, 1005},
        {"trace-writes", required_argument, NULL, 1006},
        {NULL, 0, NULL, 0}
    };
    FILE *in_fp;
    FILE *out_fp;
    mdf_options opts;
    mdf_format format;
    mdf *renderer;
    file_source source_data;
    prefixed_file_source prefixed_source_data;
    file_sink sink_data;
    trace_output trace_data;
    mdf_source source;
    mdf_sink sink;
    mdf_status st;
    FILE *trace_fp;
    char *prefix_buf;
    size_t prefix_len;
    char *detected_title;
    int use_prefixed_source;

    mdf_options_init(&opts);
    opts.osc8 = mdf_detect_osc8_support();
    format = MDF_FORMAT_ANSI;
    out_path = NULL;
    simulate_chunk = 0;
    simulate_delay_seconds = 0.0;
    width_flag = 0;
    list_themes = 0;
    theme_name = "default";
    title_override = NULL;
    trace_writes_path = NULL;
    simulate_enabled = 0;
    format_explicit = 0;
    html_content_width_flag = 0;
    memset(&trace_data, 0, sizeof(trace_data));
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hVHbo:t:T:w:8:S:", long_options, NULL)) != -1) {
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
        case 'b':
            opts.boring = 1;
            break;
        case 'o':
            out_path = optarg;
            break;
        case 't':
            theme_name = optarg;
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
        default:
            usage(stderr);
            return 2;
        }
    }
    if (list_themes) {
        print_themes();
        return 0;
    }
    if (!mdf_theme_exists(theme_name)) {
        fprintf(stderr, "cmdf: unknown theme %s\n", theme_name);
        print_themes();
        return 2;
    }
    opts.theme_name = theme_name;
    if (!format_explicit && has_html_extension(out_path)) {
        format = MDF_FORMAT_HTML;
        fprintf(stderr, "cmdf: warning: inferring --html from output path %s\n", out_path);
    }
    if (format == MDF_FORMAT_HTML && opts.table_wire_mode == MDF_TABLE_WIRE_ASCII) {
        fprintf(stderr, "cmdf: --table-wire ascii is not supported with --html; use line or space\n");
        return 2;
    }
    if (format == MDF_FORMAT_HTML) {
        if (width_flag > 0 && !html_content_width_flag) {
            opts.html_content_width_ch = (double)width_flag;
        }
        cmdf_enable_embedded_fonts(&opts);
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
    prefix_buf = NULL;
    prefix_len = 0;
    detected_title = NULL;
    use_prefixed_source = 0;
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
    if (format == MDF_FORMAT_HTML && title_override == NULL) {
        if (detect_html_title_from_file(in_fp, &detected_title, &prefix_buf, &prefix_len) != 0) {
            fprintf(stderr, "cmdf: detect HTML title: %s\n", errno == 0 ? "input read failed" : strerror(errno));
            if (out_fp != stdout) fclose(out_fp);
            if (in_fp != stdin) fclose(in_fp);
            return 1;
        }
        use_prefixed_source = 1;
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
        free(detected_title);
        free(prefix_buf);
        if (trace_fp != NULL && trace_fp != stderr) fclose(trace_fp);
        if (out_fp != stdout) fclose(out_fp);
        if (in_fp != stdin) fclose(in_fp);
        return 1;
    }
    if (format == MDF_FORMAT_HTML && (title_override != NULL || detected_title != NULL)) {
        st = mdf_set_html_title(renderer, title_override != NULL ? title_override : detected_title);
        if (st != MDF_OK) {
            fprintf(stderr, "cmdf: set HTML title: %s\n", mdf_status_string(st));
            renderer->destroy(renderer);
            free(detected_title);
            free(prefix_buf);
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
    prefixed_source_data.fp = in_fp;
    prefixed_source_data.prefix = prefix_buf;
    prefixed_source_data.prefix_len = prefix_len;
    prefixed_source_data.prefix_off = 0;
    prefixed_source_data.max_chunk = simulate_chunk;
    prefixed_source_data.delay_seconds = simulate_delay_seconds;
    prefixed_source_data.reads = 0;
    if (use_prefixed_source) {
        source.userdata = &prefixed_source_data;
        source.read = prefixed_file_read;
    } else {
        source.userdata = &source_data;
        source.read = file_read;
    }
    sink.userdata = &sink_data;
    sink.write = file_write;
    st = renderer->render(renderer, &source, &sink);
    rc = 0;
    if (st != MDF_OK) {
        fprintf(stderr, "cmdf: render: %s: %s\n", mdf_status_string(st), renderer->error(renderer));
        rc = 1;
    }
    renderer->destroy(renderer);
    free(detected_title);
    free(prefix_buf);
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
