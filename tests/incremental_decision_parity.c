#include <libmdf/mdf.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct emission {
    char *data;
    size_t len;
} emission;

typedef struct emission_log {
    emission *items;
    size_t len;
    size_t cap;
    int failed;
} emission_log;

typedef struct capture {
    emission_log writes;
    emission_log traces;
} capture;

typedef struct memory_source {
    const char *data;
    size_t len;
    size_t off;
} memory_source;

static int append_emission(emission_log *log, const char *src, size_t len)
{
    emission *next;
    char *copy;
    size_t cap;

    if (log->len == log->cap) {
        cap = log->cap == 0 ? 64 : log->cap * 2;
        if (cap < log->cap || cap > (size_t)-1 / sizeof(*next)) {
            return -1;
        }
        next = (emission *)realloc(log->items, cap * sizeof(*next));
        if (next == NULL) {
            return -1;
        }
        log->items = next;
        log->cap = cap;
    }
    copy = (char *)malloc(len == 0 ? 1 : len);
    if (copy == NULL) {
        return -1;
    }
    if (len > 0) {
        memcpy(copy, src, len);
    }
    log->items[log->len].data = copy;
    log->items[log->len].len = len;
    log->len++;
    return 0;
}

static void free_emission_log(emission_log *log)
{
    size_t i;

    for (i = 0; i < log->len; i++) {
        free(log->items[i].data);
    }
    free(log->items);
    memset(log, 0, sizeof(*log));
}

static void free_capture(capture *cap)
{
    free_emission_log(&cap->writes);
    free_emission_log(&cap->traces);
}

static int capture_write(void *userdata, const char *src, size_t len)
{
    capture *cap;

    cap = (capture *)userdata;
    if (append_emission(&cap->writes, src, len) != 0) {
        cap->writes.failed = 1;
        return -1;
    }
    return 0;
}

static int capture_trace(void *userdata, mdf_format format, const char *src, size_t len)
{
    capture *cap;

    (void)format;
    cap = (capture *)userdata;
    if (append_emission(&cap->traces, src, len) != 0) {
        cap->traces.failed = 1;
        return -1;
    }
    return 0;
}

static size_t memory_read(void *userdata, char *dst, size_t cap, int *err)
{
    memory_source *source;
    size_t n;

    (void)err;
    source = (memory_source *)userdata;
    n = source->len - source->off;
    if (n > cap) {
        n = cap;
    }
    if (n > 0) {
        memcpy(dst, source->data + source->off, n);
        source->off += n;
    }
    return n;
}

static int parse_nonnegative(const char *src, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(src, &end, 10);
    if (errno != 0 || end == src || *end != '\0' || value < 0 || value > INT_MAX) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *fp;
    long file_len;
    char *data;
    size_t n;

    *out = NULL;
    *out_len = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0 || (unsigned long)file_len > (size_t)-1) {
        fprintf(stderr, "%s: unable to determine input size\n", path);
        fclose(fp);
        return -1;
    }
    data = (char *)malloc((size_t)file_len == 0 ? 1 : (size_t)file_len);
    if (data == NULL) {
        fprintf(stderr, "%s: out of memory\n", path);
        fclose(fp);
        return -1;
    }
    n = fread(data, 1, (size_t)file_len, fp);
    if (fclose(fp) != 0 || n != (size_t)file_len) {
        fprintf(stderr, "%s: unable to read input\n", path);
        free(data);
        return -1;
    }
    *out = data;
    *out_len = (size_t)file_len;
    return 0;
}

static int run_baseline(const mdf_options *base_opts,
                        const char *data,
                        size_t len,
                        capture *cap)
{
    mdf_options opts;
    memory_source source_data;
    mdf_source source;
    mdf_sink sink;
    mdf *renderer;
    mdf_status st;

    opts = *base_opts;
    opts.write_trace.userdata = cap;
    opts.write_trace.emit = capture_trace;
    source_data.data = data;
    source_data.len = len;
    source_data.off = 0;
    source.userdata = &source_data;
    source.read = memory_read;
    sink.userdata = cap;
    sink.write = capture_write;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st == MDF_OK) {
        st = renderer->render(renderer, &source, &sink);
    }
    if (renderer != NULL) {
        renderer->destroy(renderer);
    }
    return st == MDF_OK && !cap->writes.failed && !cap->traces.failed ? 0 : -1;
}

static int run_incremental(const mdf_options *base_opts,
                           const char *data,
                           size_t len,
                           size_t fragment_len,
                           capture *cap)
{
    mdf_options opts;
    mdf_sink sink;
    mdf *renderer;
    mdf_status st;
    size_t off;
    size_t n;

    opts = *base_opts;
    opts.write_trace.userdata = cap;
    opts.write_trace.emit = capture_trace;
    sink.userdata = cap;
    sink.write = capture_write;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    off = 0;
    while (st == MDF_OK && off < len) {
        n = len - off;
        if (n > fragment_len) {
            n = fragment_len;
        }
        st = renderer->feed(renderer, data + off, n, &sink);
        if (st == MDF_OK) {
            st = renderer->flush(renderer, &sink);
        }
        off += n;
    }
    if (st == MDF_OK) {
        st = renderer->finish_document(renderer, &sink);
    }
    if (renderer != NULL) {
        renderer->destroy(renderer);
    }
    return st == MDF_OK && !cap->writes.failed && !cap->traces.failed ? 0 : -1;
}

static void print_emission(FILE *fp, const emission *item);

static int logs_equal(const emission_log *actual,
                      const emission_log *expected,
                      const char *actual_name,
                      const char *expected_name,
                      const char *input,
                      size_t fragment_len)
{
    size_t i;

    for (i = 0; i < actual->len && i < expected->len; i++) {
        if (actual->items[i].len != expected->items[i].len ||
            memcmp(actual->items[i].data, expected->items[i].data,
                   actual->items[i].len) != 0) {
            fprintf(stderr, "%s: fragment %lu: %s emission %lu differs from %s: ",
                    input, (unsigned long)fragment_len, actual_name,
                    (unsigned long)i, expected_name);
            print_emission(stderr, &actual->items[i]);
            fprintf(stderr, " expected ");
            print_emission(stderr, &expected->items[i]);
            fputc('\n', stderr);
            return -1;
        }
    }
    if (actual->len != expected->len) {
        fprintf(stderr, "%s: fragment %lu: %s count %lu != %s count %lu\n",
                input, (unsigned long)fragment_len, actual_name,
                (unsigned long)actual->len, expected_name,
                (unsigned long)expected->len);
        return -1;
    }
    return 0;
}

static int capture_is_self_consistent(const capture *cap,
                                      const char *input,
                                      size_t fragment_len)
{
    return logs_equal(&cap->traces, &cap->writes, "trace", "sink write",
                      input, fragment_len) == 0 ? 0 : -1;
}

static void print_emission(FILE *fp, const emission *item)
{
    size_t i;

    fprintf(fp, "len=%lu hex=", (unsigned long)item->len);
    for (i = 0; i < item->len; i++) {
        fprintf(fp, "%02x", (unsigned int)(unsigned char)item->data[i]);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --ansi [--boring] [-w width] "
            "[--margin-left columns] [--margin-right columns] input.md\n", argv0);
}

int main(int argc, char **argv)
{
    static const size_t fragments[] = {1, 2, 3, 7, 31, 4096};
    mdf_options opts;
    capture baseline;
    capture incremental;
    char *data;
    const char *input;
    size_t len;
    size_t i;
    int value;
    int rc;

    mdf_options_init(&opts);
    input = NULL;
    i = 1;
    while (i < (size_t)argc) {
        if (strcmp(argv[i], "--ansi") == 0) {
            /* ANSI is the only supported decision-stream contract. */
        } else if (strcmp(argv[i], "--boring") == 0) {
            opts.boring = 1;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--width") == 0) {
            i++;
            if (i >= (size_t)argc || parse_nonnegative(argv[i], &value) != 0 || value == 0) {
                usage(argv[0]);
                return 2;
            }
            opts.width = value;
        } else if (strcmp(argv[i], "--margin-left") == 0) {
            i++;
            if (i >= (size_t)argc || parse_nonnegative(argv[i], &opts.margin_left) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--margin-right") == 0) {
            i++;
            if (i >= (size_t)argc || parse_nonnegative(argv[i], &opts.margin_right) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (input == NULL) {
            input = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
        i++;
    }
    if (input == NULL || read_file(input, &data, &len) != 0) {
        return 2;
    }
    memset(&baseline, 0, sizeof(baseline));
    rc = run_baseline(&opts, data, len, &baseline);
    if (rc == 0) {
        rc = capture_is_self_consistent(&baseline, input, 0);
    }
    for (i = 0; rc == 0 && i < sizeof(fragments) / sizeof(fragments[0]); i++) {
        memset(&incremental, 0, sizeof(incremental));
        rc = run_incremental(&opts, data, len, fragments[i], &incremental);
        if (rc == 0) {
            rc = capture_is_self_consistent(&incremental, input, fragments[i]);
        }
        if (rc == 0) {
            rc = logs_equal(&incremental.traces, &baseline.traces,
                            "incremental trace", "baseline trace", input,
                            fragments[i]);
        }
        free_capture(&incremental);
    }
    free_capture(&baseline);
    free(data);
    return rc == 0 ? 0 : 1;
}
