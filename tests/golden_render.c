#include "libmdf/mdf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct file_source {
    FILE *fp;
    int err;
} file_source;

typedef struct count_sink {
    size_t bytes;
} count_sink;

static size_t file_read(void *userdata, char *dst, size_t cap, int *err)
{
    file_source *src;
    size_t n;

    src = (file_source *)userdata;
    n = fread(dst, 1, cap, src->fp);
    if (n == 0 && ferror(src->fp)) {
        src->err = errno != 0 ? errno : EIO;
        if (err != NULL) {
            *err = src->err;
        }
    }
    return n;
}

static int stdout_write(void *userdata, const char *src, size_t len)
{
    FILE *fp;

    fp = (FILE *)userdata;
    return fwrite(src, 1, len, fp) == len ? 0 : -1;
}

static int count_write(void *userdata, const char *src, size_t len)
{
    count_sink *sink;

    (void)src;
    sink = (count_sink *)userdata;
    if (len > (size_t)-1 - sink->bytes) {
        return -1;
    }
    sink->bytes += len;
    return 0;
}

static int parse_int_arg(const char *s, int *out)
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

static int parse_transition(const char *s, mdf_deck_transition *out)
{
    if (strcmp(s, "fade") == 0) {
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

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--ansi|--html|--deck] [--repeat N] [-w N] [--boring] "
            "[--margin-left N] [--margin-right N] [--slide-numbers] "
            "[-x fade|cross|hard] input.md\n",
            argv0);
}

int main(int argc, char **argv)
{
    mdf_format format;
    mdf_options opts;
    const char *input;
    FILE *fp;
    file_source src_data;
    mdf_source src;
    mdf_sink sink;
    mdf *renderer;
    mdf_status st;
    count_sink counted;
    int repeat;
    int i;

    format = MDF_FORMAT_ANSI;
    input = NULL;
    repeat = 1;
    mdf_options_init(&opts);

    i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--ansi") == 0) {
            format = MDF_FORMAT_ANSI;
        } else if (strcmp(argv[i], "--html") == 0) {
            format = MDF_FORMAT_HTML;
        } else if (strcmp(argv[i], "--deck") == 0) {
            format = MDF_FORMAT_HTML_DECK;
        } else if (strcmp(argv[i], "--repeat") == 0) {
            i++;
            if (i >= argc || parse_int_arg(argv[i], &repeat) != 0 || repeat <= 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--width") == 0) {
            i++;
            if (i >= argc || parse_int_arg(argv[i], &opts.width) != 0 || opts.width <= 0) {
                usage(argv[0]);
                return 2;
            }
            opts.html_content_width_ch = (double)opts.width;
        } else if (strcmp(argv[i], "--boring") == 0) {
            opts.boring = 1;
        } else if (strcmp(argv[i], "--margin-left") == 0) {
            i++;
            if (i >= argc || parse_int_arg(argv[i], &opts.margin_left) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--margin-right") == 0) {
            i++;
            if (i >= argc || parse_int_arg(argv[i], &opts.margin_right) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--slide-numbers") == 0) {
            opts.slide_numbers = 1;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--transition") == 0) {
            i++;
            if (i >= argc || parse_transition(argv[i], &opts.deck_transition) != 0) {
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

    if (input == NULL) {
        usage(argv[0]);
        return 2;
    }

    fp = fopen(input, "rb");
    if (fp == NULL) {
        fprintf(stderr, "%s: %s\n", input, strerror(errno));
        return 1;
    }

    renderer = NULL;
    st = mdf_create(format, &opts, &renderer);
    if (st != MDF_OK) {
        fprintf(stderr, "mdf_create: %s\n", mdf_status_string(st));
        fclose(fp);
        return 1;
    }

    memset(&counted, 0, sizeof(counted));
    for (i = 0; i < repeat; i++) {
        if (i != 0) {
            if (fseek(fp, 0, SEEK_SET) != 0) {
                fprintf(stderr, "%s: %s\n", input, strerror(errno));
                renderer->destroy(renderer);
                fclose(fp);
                return 1;
            }
            clearerr(fp);
        }
        src_data.fp = fp;
        src_data.err = 0;
        src.userdata = &src_data;
        src.read = file_read;
        sink.userdata = repeat == 1 ? (void *)stdout : (void *)&counted;
        sink.write = repeat == 1 ? stdout_write : count_write;
        st = renderer->render(renderer, &src, &sink);
        if (st != MDF_OK) {
            fprintf(stderr, "render: %s: %s\n", mdf_status_string(st),
                    renderer->error(renderer) != NULL ? renderer->error(renderer) : "");
            renderer->destroy(renderer);
            fclose(fp);
            return 1;
        }
    }
    if (repeat > 1 && counted.bytes == 0) {
        fprintf(stderr, "render: produced no output\n");
        renderer->destroy(renderer);
        fclose(fp);
        return 1;
    }
    if (repeat == 1 && fflush(stdout) != 0) {
        fprintf(stderr, "stdout: %s\n", strerror(errno));
        renderer->destroy(renderer);
        fclose(fp);
        return 1;
    }

    renderer->destroy(renderer);
    fclose(fp);
    return 0;
}
