#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LIBMDF_TEST_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define LIBMDF_TEST_ASAN 1
#endif
#ifndef LIBMDF_TEST_ASAN
#  define LIBMDF_TEST_ASAN 0
#endif

typedef struct run_result {
    char *buf;
    size_t len;
    long elapsed_ms;
    int status;
} run_result;

static long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static int write_all(int fd, const char *src, size_t len)
{
    ssize_t n;

    while (len > 0) {
        n = write(fd, src, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        src += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int make_input_file(char *path, size_t path_cap)
{
    int fd;
    const char *src;

    src = "abc\n";
    strcpy(path, "/tmp/libmdf-cmdf-delay-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, src, strlen(src)) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(path);
        return -1;
    }
    (void)path_cap;
    return 0;
}

static int grow_buf(char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    char *next;
    size_t want;

    if (*len + n + 1 < *len) {
        return -1;
    }
    want = *len + n + 1;
    if (want > *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 256 : *cap;
        while (new_cap < want) {
            new_cap *= 2;
        }
        next = (char *)realloc(*buf, new_cap);
        if (next == NULL) {
            return -1;
        }
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int run_cmdf(char *const argv[], run_result *out)
{
    int pipefd[2];
    pid_t pid;
    char chunk[256];
    ssize_t n;
    long started;
    long finished;
    size_t cap;
    int status;

    memset(out, 0, sizeof(*out));
    if (pipe(pipefd) != 0) {
        return -1;
    }
    started = now_ms();
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    cap = 0;
    while ((n = read(pipefd[0], chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipefd[0]);
            return -1;
        }
        if (grow_buf(&out->buf, &out->len, &cap, chunk, (size_t)n) != 0) {
            close(pipefd[0]);
            return -1;
        }
    }
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    finished = now_ms();
    out->status = status;
    out->elapsed_ms = finished - started;
    return 0;
}

static int run_cmdf_first_chunk(char *const argv[], run_result *out, char **first_chunk_out, long *first_ms_out)
{
    int pipefd[2];
    pid_t pid;
    fd_set rfds;
    struct timeval tv;
    char chunk[256];
    ssize_t n;
    long started;
    long first_ms;
    long finished;
    size_t cap;
    int status;
    int saw_first;
    char *first_chunk;
    size_t first_len;
    size_t first_cap;

    memset(out, 0, sizeof(*out));
    *first_chunk_out = NULL;
    *first_ms_out = -1;
    if (pipe(pipefd) != 0) {
        return -1;
    }
    started = now_ms();
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    cap = 0;
    first_ms = -1;
    saw_first = 0;
    first_chunk = NULL;
    first_len = 0;
    first_cap = 0;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        status = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (status < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipefd[0]);
            free(first_chunk);
            return -1;
        }
        if (status == 0) {
            close(pipefd[0]);
            free(first_chunk);
            errno = ETIMEDOUT;
            return -1;
        }
        n = read(pipefd[0], chunk, sizeof(chunk));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipefd[0]);
            free(first_chunk);
            return -1;
        }
        if (!saw_first) {
            first_ms = now_ms() - started;
            saw_first = 1;
            if (grow_buf(&first_chunk, &first_len, &first_cap, chunk, (size_t)n) != 0) {
                close(pipefd[0]);
                free(first_chunk);
                return -1;
            }
        }
        if (grow_buf(&out->buf, &out->len, &cap, chunk, (size_t)n) != 0) {
            close(pipefd[0]);
            free(first_chunk);
            return -1;
        }
    }
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0) {
        free(first_chunk);
        return -1;
    }
    finished = now_ms();
    out->status = status;
    out->elapsed_ms = finished - started;
    *first_chunk_out = first_chunk;
    *first_ms_out = first_ms;
    return 0;
}

static int read_file_text(const char *path, char **buf_out)
{
    FILE *fp;
    char chunk[256];
    size_t n;
    char *buf;
    size_t len;
    size_t cap;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    buf = NULL;
    len = 0;
    cap = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (grow_buf(&buf, &len, &cap, chunk, n) != 0) {
            fclose(fp);
            free(buf);
            return -1;
        }
    }
    if (ferror(fp)) {
        fclose(fp);
        free(buf);
        return -1;
    }
    fclose(fp);
    if (buf == NULL) {
        buf = (char *)malloc(1);
        if (buf == NULL) {
            return -1;
        }
        buf[0] = '\0';
    }
    *buf_out = buf;
    return 0;
}

static int base64_value(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    if (c == '=') {
        return -2;
    }
    return -1;
}

static int decode_base64_text(const char *src, size_t src_len, char **buf_out, size_t *len_out)
{
    char *buf;
    size_t cap;
    size_t len;
    size_t i;
    int a;
    int b;
    int c;
    int d;

    cap = ((src_len + 3) / 4) * 3;
    buf = cap == 0 ? NULL : (char *)malloc(cap);
    if (cap != 0 && buf == NULL) {
        return -1;
    }
    len = 0;
    i = 0;
    while (i < src_len) {
        if (i + 4 > src_len) {
            free(buf);
            errno = EINVAL;
            return -1;
        }
        a = base64_value((unsigned char)src[i]);
        b = base64_value((unsigned char)src[i + 1]);
        c = base64_value((unsigned char)src[i + 2]);
        d = base64_value((unsigned char)src[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            free(buf);
            errno = EINVAL;
            return -1;
        }
        buf[len++] = (char)((a << 2) | (b >> 4));
        if (c != -2) {
            buf[len++] = (char)(((b & 15) << 4) | (c >> 2));
            if (d != -2) {
                buf[len++] = (char)(((c & 3) << 6) | d);
            }
        }
        i += 4;
    }
    *buf_out = buf;
    *len_out = len;
    return 0;
}

static int append_trace_escaped(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len)
{
    static const char octal[] = "01234567";
    size_t i;
    char esc[4];

    for (i = 0; i < src_len; i++) {
        unsigned char c;

        c = (unsigned char)src[i];
        if (c == '\n') {
            if (grow_buf(buf, len, cap, "\\n", 2) != 0) return -1;
        } else if (c == '\t') {
            if (grow_buf(buf, len, cap, "\\t", 2) != 0) return -1;
        } else if (c == '\r') {
            if (grow_buf(buf, len, cap, "\\r", 2) != 0) return -1;
        } else if (c == 27) {
            if (grow_buf(buf, len, cap, "\\33", 3) != 0) return -1;
        } else if (c == '\\' || c == '"') {
            char pair[2];

            pair[0] = '\\';
            pair[1] = (char)c;
            if (grow_buf(buf, len, cap, pair, 2) != 0) return -1;
        } else if (c >= 32 && c <= 126) {
            if (grow_buf(buf, len, cap, (const char *)&src[i], 1) != 0) return -1;
        } else {
            esc[0] = '\\';
            esc[1] = octal[(c >> 6) & 7];
            esc[2] = octal[(c >> 3) & 7];
            esc[3] = octal[c & 7];
            if (grow_buf(buf, len, cap, esc, sizeof(esc)) != 0) return -1;
        }
    }
    return 0;
}

static int trace_ndjson_to_calls(const char *src, char **out)
{
    const char *line;
    char *buf;
    size_t len;
    size_t cap;

    line = src;
    buf = NULL;
    len = 0;
    cap = 0;
    while (*line != '\0') {
        const char *line_end;
        const char *bytes_field;
        const char *data_field;
        const char *data_end;
        unsigned long bytes;
        char *decoded;
        size_t decoded_len;
        char num[32];
        int n;

        line_end = strchr(line, '\n');
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        if (line_end == line) {
            line = *line_end == '\0' ? line_end : line_end + 1;
            continue;
        }
        bytes_field = strstr(line, "\"bytes\":");
        data_field = strstr(line, "\"data_b64\":\"");
        if (bytes_field == NULL || data_field == NULL || bytes_field > line_end || data_field > line_end) {
            free(buf);
            errno = EINVAL;
            return -1;
        }
        bytes = strtoul(bytes_field + 8, NULL, 10);
        data_field += 12;
        data_end = data_field;
        while (data_end < line_end && *data_end != '"') {
            data_end++;
        }
        if (data_end >= line_end) {
            free(buf);
            errno = EINVAL;
            return -1;
        }
        decoded = NULL;
        decoded_len = 0;
        if (decode_base64_text(data_field, (size_t)(data_end - data_field), &decoded, &decoded_len) != 0) {
            free(buf);
            return -1;
        }
        if (decoded_len != (size_t)bytes) {
            free(decoded);
            free(buf);
            errno = EINVAL;
            return -1;
        }
        if (grow_buf(&buf, &len, &cap, "emit(\"", 6) != 0) {
            free(decoded);
            free(buf);
            return -1;
        }
        if (append_trace_escaped(&buf, &len, &cap, decoded, decoded_len) != 0) {
            free(decoded);
            free(buf);
            return -1;
        }
        n = snprintf(num, sizeof(num), "\", %lu)\n", bytes);
        free(decoded);
        if (n < 0 || grow_buf(&buf, &len, &cap, num, (size_t)n) != 0) {
            free(buf);
            return -1;
        }
        line = *line_end == '\0' ? line_end : line_end + 1;
    }
    if (buf == NULL) {
        buf = (char *)malloc(1);
        if (buf == NULL) {
            return -1;
        }
        buf[0] = '\0';
    }
    *out = buf;
    return 0;
}

static int run_cmdf_trace(char *const cmdv[], const char *input_path, char **trace_out)
{
    int status;
    pid_t pid;
    char trace_path[64];
    char *argv[32];
    char *raw_trace;
    int devnull;
    int i;

    strcpy(trace_path, "/tmp/libmdf-cmdf-trace-XXXXXX");
    status = mkstemp(trace_path);
    if (status < 0) {
        return -1;
    }
    close(status);

    i = 0;
    while (*cmdv != NULL && i + 3 < (int)(sizeof(argv) / sizeof(argv[0]))) {
        argv[i++] = *cmdv++;
    }
    argv[i++] = "--trace-writes";
    argv[i++] = trace_path;
    argv[i] = NULL;

    pid = fork();
    if (pid < 0) {
        unlink(trace_path);
        return -1;
    }
    if (pid == 0) {
        int fd;

        fd = open(input_path, O_RDONLY);
        if (fd < 0) {
            _exit(127);
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(fd);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull < 0) {
            _exit(127);
        }
        if (dup2(devnull, STDOUT_FILENO) < 0 || dup2(devnull, STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(devnull);
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        unlink(trace_path);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(trace_path);
        errno = EIO;
        return -1;
    }
    raw_trace = NULL;
    if (read_file_text(trace_path, &raw_trace) != 0) {
        unlink(trace_path);
        return -1;
    }
    if (trace_ndjson_to_calls(raw_trace, trace_out) != 0) {
        free(raw_trace);
        unlink(trace_path);
        return -1;
    }
    free(raw_trace);
    unlink(trace_path);
    return 0;
}

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char input_path[64];
    char *ascii_html_args[8];
    char *chunk_args[8];
    char *implicit_args[7];
    char *html_trace_args[8];
    char *html_stream_args[8];
    char *theme_args[8];
    char *margin_args[10];
    char *bad_delay_args[6];
    char *first_chunk;
    long first_ms;
    char *trace_args[8];
    char *trace;
    run_result result;
    int fails;

    if (argc != 2) {
        fprintf(stderr, "usage: test_cmdf CMDf_PATH\n");
        return 2;
    }
    if (make_input_file(input_path, sizeof(input_path)) != 0) {
        fprintf(stderr, "make input: %s\n", strerror(errno));
        return 1;
    }

    fails = 0;

    chunk_args[0] = argv[1];
    chunk_args[1] = "-b";
    chunk_args[2] = "--simulate-chunk";
    chunk_args[3] = "1";
    chunk_args[4] = "--simulate-delay";
    chunk_args[5] = "30ms";
    chunk_args[6] = input_path;
    chunk_args[7] = NULL;
    if (run_cmdf(chunk_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf chunked: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf chunked delayed run exits successfully");
    fails += expect(result.buf != NULL && strcmp(result.buf, "abc\n") == 0,
                    "cmdf chunked delayed run preserves rendered output");
    fails += expect(result.elapsed_ms >= 60L,
                    "cmdf chunked delayed run sleeps between simulated reads");
    free(result.buf);

    implicit_args[0] = argv[1];
    implicit_args[1] = "-b";
    implicit_args[2] = "--simulate-delay";
    implicit_args[3] = "30ms";
    implicit_args[4] = input_path;
    implicit_args[5] = NULL;
    if (run_cmdf(implicit_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf implicit simulate: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf implicit simulate delay exits successfully");
    fails += expect(result.buf != NULL && strcmp(result.buf, "abc\n") == 0,
                    "cmdf implicit simulate delay preserves rendered output");
    fails += expect(result.elapsed_ms >= 20L,
                    "cmdf simulate delay implies simulated chunking");
    free(result.buf);

    {
        int fd;
        const char *src;

        src = "alpha\n\nbeta gamma\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            return 1;
        }
    }
    margin_args[0] = argv[1];
    margin_args[1] = "-b";
    margin_args[2] = "-w";
    margin_args[3] = "10";
    margin_args[4] = "--margin-left";
    margin_args[5] = "2";
    margin_args[6] = "--margin-right";
    margin_args[7] = "1";
    margin_args[8] = input_path;
    margin_args[9] = NULL;
    if (run_cmdf(margin_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf margins: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf margin run exits successfully");
    fails += expect(result.buf != NULL && strcmp(result.buf, "  alpha\n\n  beta\n  gamma\n") == 0,
                    "cmdf margins indent nonblank lines and leave blank lines empty");
    free(result.buf);

    html_stream_args[0] = argv[1];
    html_stream_args[1] = "-H";
    html_stream_args[2] = "--simulate-chunk";
    html_stream_args[3] = "1";
    html_stream_args[4] = "--simulate-delay";
    html_stream_args[5] = "30ms";
    html_stream_args[6] = input_path;
    html_stream_args[7] = NULL;
    {
        int fd;
        const char *src;

        src = "# Demo\n\nbody\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            return 1;
        }
    }
    first_chunk = NULL;
    first_ms = -1;
    if (run_cmdf_first_chunk(html_stream_args, &result, &first_chunk, &first_ms) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html stream probe: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html delayed stream exits successfully");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<!doctype html>") != NULL,
                    "cmdf html emits document shell in the first streamed chunk");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "@font-face{font-family:\"JetBrains Mono\";src:url(data:font/woff2;base64,") != NULL &&
                    strstr(result.buf, "format('woff2')") != NULL,
                    "cmdf html embeds JetBrains Mono font data");
    fails += expect(first_ms >= 0 && first_ms + 20L < result.elapsed_ms,
                    "cmdf html emits before process completion under simulate delay");
    free(first_chunk);
    free(result.buf);

    bad_delay_args[0] = argv[1];
    bad_delay_args[1] = "-b";
    bad_delay_args[2] = "--simulate-delay";
    bad_delay_args[3] = "0.03";
    bad_delay_args[4] = input_path;
    bad_delay_args[5] = NULL;
    if (run_cmdf(bad_delay_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf invalid simulate delay: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects unitless fractional simulate delay like mdf");
    free(result.buf);

    html_trace_args[0] = argv[1];
    html_trace_args[1] = "-H";
    html_trace_args[2] = "--trace-writes";
    html_trace_args[3] = "/tmp/libmdf-cmdf-html-trace.out";
    html_trace_args[4] = input_path;
    html_trace_args[5] = NULL;
    if (run_cmdf(html_trace_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html trace rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects --trace-writes for html output");
    free(result.buf);
    unlink("/tmp/libmdf-cmdf-html-trace.out");

    ascii_html_args[0] = argv[1];
    ascii_html_args[1] = "-H";
    ascii_html_args[2] = "--table-wire";
    ascii_html_args[3] = "ascii";
    ascii_html_args[4] = input_path;
    ascii_html_args[5] = NULL;
    if (run_cmdf(ascii_html_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html ascii-wire rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects --table-wire ascii with html output");
    free(result.buf);

    theme_args[0] = argv[1];
    theme_args[1] = "--theme";
    theme_args[2] = "not-a-theme";
    theme_args[3] = input_path;
    theme_args[4] = NULL;
    if (run_cmdf(theme_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf unknown-theme rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects unknown theme names");
    free(result.buf);

    if (!LIBMDF_TEST_ASAN) {
        trace_args[0] = argv[1];
        trace_args[1] = "-w";
        trace_args[2] = "80";
        trace_args[3] = "-S";
        trace_args[4] = "1";
        trace_args[5] = NULL;
        unlink(input_path);
        if (make_input_file(input_path, sizeof(input_path)) != 0) {
            fprintf(stderr, "make input: %s\n", strerror(errno));
            return 1;
        }
        {
            int fd;
            const char *src;

            src = "# Header here\n";
            fd = open(input_path, O_WRONLY | O_TRUNC);
            if (fd < 0) {
                unlink(input_path);
                return 1;
            }
            if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
                close(fd);
                unlink(input_path);
                return 1;
            }
        }
        trace = NULL;
        if (run_cmdf_trace(trace_args, input_path, &trace) != 0) {
            unlink(input_path);
            fprintf(stderr, "run cmdf header emission trace: %s\n", strerror(errno));
            return 1;
        }
        fails += expect(trace != NULL &&
                        strstr(trace, "emit(\"\\33[1;32m#\", 8)") != NULL &&
                        strstr(trace, "emit(\" \", 1)") != NULL &&
                        strstr(trace, "emit(\"\\33[0m\", 4)") != NULL &&
                        strstr(trace, "emit(\"\\33[1;32mHeader\", 13)") != NULL &&
                        strstr(trace, "emit(\"here\", 4)") != NULL &&
                        strstr(trace, "emit(\"\\n\", 1)") != NULL,
                        "cmdf traces heading renderer emissions as decision chunks");
        free(trace);

        {
            int fd;
            const char *src;

            src = "> **\"Governance exists to support autonomy\"** does **not** imply\n";
            fd = open(input_path, O_WRONLY | O_TRUNC);
            if (fd < 0) {
                unlink(input_path);
                return 1;
            }
            if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
                close(fd);
                unlink(input_path);
                return 1;
            }
        }
        trace = NULL;
        if (run_cmdf_trace(trace_args, input_path, &trace) != 0) {
            unlink(input_path);
            fprintf(stderr, "run cmdf bold quote emission trace: %s\n", strerror(errno));
            return 1;
        }
        fails += expect(trace != NULL &&
                        strstr(trace, "emit(\"\\33[90m>\", 6)") != NULL &&
                        strstr(trace, "emit(\"\\33[1m\\33[1;37m\\\"Governance\", 22)") != NULL &&
                        strstr(trace, "emit(\"exists\", 6)") != NULL &&
                        strstr(trace, "emit(\"to\", 2)") != NULL &&
                        strstr(trace, "emit(\"support\", 7)") != NULL &&
                        strstr(trace, "emit(\"autonomy\\\"\", 9)") != NULL &&
                        strstr(trace, "emit(\"\\33[0m \", 5)") != NULL &&
                        strstr(trace, "emit(\"\\33[1m\\33[1;37mnot\", 14)") != NULL &&
                        strstr(trace, "emit(\"imply\", 5)") != NULL,
                        "cmdf traces quoted bold text word emissions");
        free(trace);
    }

    unlink(input_path);
    return fails == 0 ? 0 : 1;
}
