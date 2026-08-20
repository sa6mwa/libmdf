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

static int make_deck_input_file(char *path, size_t path_cap)
{
    int fd;
    const char *src;

    src = "---\n"
          "theme: \"tokyo-night\"\n"
          "---\n"
          "# Front\n"
          "\n"
          "---\n"
          "\n"
          "# Second\n"
          "\n"
          "Body.\n"
          "\n"
          "---\n"
          "\n"
          "# Chart\n"
          "\n"
          "```mdf-bar-chart\n"
          "A,1\n"
          "B,2\n"
          "```\n";
    strcpy(path, "/tmp/libmdf-cmdf-deck-XXXXXX");
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

static int run_cmdf_stderr(char *const argv[], run_result *out)
{
    int pipefd[2];
    pid_t pid;
    char chunk[256];
    ssize_t n;
    long started;
    long finished;
    size_t cap;
    int status;
    int null_fd;

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
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            close(null_fd);
        }
        if (dup2(pipefd[1], STDERR_FILENO) < 0) {
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

static int run_cmdf_stdin_first_chunk(char *const argv[],
                                      const char *prefix,
                                      const char *suffix,
                                      const char *before_suffix_marker,
                                      run_result *out,
                                      char **first_chunk_out,
                                      long *first_ms_out)
{
    int stdout_pipe[2];
    int stdin_pipe[2];
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
    if (pipe(stdout_pipe) != 0) {
        return -1;
    }
    if (pipe(stdin_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }
    started = now_ms();
    pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stdin_pipe[1]);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(stdout_pipe[1]);
        close(stdin_pipe[0]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(stdout_pipe[1]);
    close(stdin_pipe[0]);
    if (write_all(stdin_pipe[1], prefix, strlen(prefix)) != 0) {
        close(stdout_pipe[0]);
        close(stdin_pipe[1]);
        return -1;
    }
    cap = 0;
    first_ms = -1;
    saw_first = 0;
    first_chunk = NULL;
    first_len = 0;
    first_cap = 0;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(stdout_pipe[0], &rfds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        status = select(stdout_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (status < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(stdout_pipe[0]);
            close(stdin_pipe[1]);
            free(first_chunk);
            return -1;
        }
        if (status == 0) {
            close(stdout_pipe[0]);
            close(stdin_pipe[1]);
            free(first_chunk);
            errno = ETIMEDOUT;
            return -1;
        }
        n = read(stdout_pipe[0], chunk, sizeof(chunk));
        if (n <= 0) {
            close(stdout_pipe[0]);
            close(stdin_pipe[1]);
            free(first_chunk);
            errno = n == 0 ? EPIPE : errno;
            return -1;
        }
        first_ms = now_ms() - started;
        saw_first = 1;
        if (grow_buf(&first_chunk, &first_len, &first_cap, chunk, (size_t)n) != 0 ||
            grow_buf(&out->buf, &out->len, &cap, chunk, (size_t)n) != 0) {
            close(stdout_pipe[0]);
            close(stdin_pipe[1]);
            free(first_chunk);
            return -1;
        }
        if (before_suffix_marker == NULL || strstr(first_chunk, before_suffix_marker) != NULL) {
            break;
        }
    }
    if (saw_first && write_all(stdin_pipe[1], suffix, strlen(suffix)) != 0) {
        close(stdout_pipe[0]);
        close(stdin_pipe[1]);
        free(first_chunk);
        return -1;
    }
    close(stdin_pipe[1]);
    while ((n = read(stdout_pipe[0], chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(stdout_pipe[0]);
            free(first_chunk);
            return -1;
        }
        if (grow_buf(&out->buf, &out->len, &cap, chunk, (size_t)n) != 0) {
            close(stdout_pipe[0]);
            free(first_chunk);
            return -1;
        }
    }
    close(stdout_pipe[0]);
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

static int count_substrings(const char *haystack, const char *needle)
{
    int count;
    size_t needle_len;

    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    count = 0;
    needle_len = strlen(needle);
    while ((haystack = strstr(haystack, needle)) != NULL) {
        count++;
        haystack += needle_len;
    }
    return count;
}

int main(int argc, char **argv)
{
    char input_path[64];
    char deck_input_path[64];
    char output_path[96];
    char output_path_2[96];
    char regular_font_path[96];
    char italic_font_path[96];
    char font_dir[64];
    char font_dir_uri[96];
    char directory_regular_font_path[128];
    char directory_italic_font_path[128];
    char *ascii_html_args[8];
    char *help_args[3];
    char *chunk_args[8];
    char *implicit_args[7];
    char *infer_html_args[6];
    char *html_title_args[8];
    char *deck_title_args[8];
    char *deck_frontmatter_title_args[4];
    char *html_frontmatter_title_args[4];
    char *html_title_stdin_args[5];
    char *html_autotitle_stdin_args[3];
    char *deck_title_stdin_args[5];
    char *html_paragraph_title_args[5];
    char *html_trace_args[8];
    char *html_stream_args[8];
    char *deck_trace_args[8];
    char *deck_args[8];
    char *html_deck_order_args[8];
    char *deck_html_order_args[8];
    char *deck_theme_default_args[9];
    char *deck_malformed_theme_args[4];
    char *deck_output_args[10];
    char *deck_fade_args[8];
    char *deck_center_front_text_args[8];
    char *deck_content_width_args[8];
    char *deck_width_args[8];
    char *deck_boring_args[8];
    char *bad_transition_args[6];
    char *transition_without_deck_args[5];
    char *slide_numbers_without_deck_args[7];
    char *theme_args[8];
    char *ascii_deck_args[8];
    char *margin_args[10];
    char *bad_delay_args[6];
    char *external_font_args[6];
    char *dump_font_args[12];
    char *first_chunk;
    long first_ms;
    char *trace_args[8];
    char *trace;
    char *html_text;
    run_result result;
    int fails;
    FILE *font_fp;
    int first_font_byte;

    if (argc != 2) {
        fprintf(stderr, "usage: test_cmdf CMDf_PATH\n");
        return 2;
    }
    if (make_input_file(input_path, sizeof(input_path)) != 0) {
        fprintf(stderr, "make input: %s\n", strerror(errno));
        return 1;
    }
    if (make_deck_input_file(deck_input_path, sizeof(deck_input_path)) != 0) {
        unlink(input_path);
        fprintf(stderr, "make deck input: %s\n", strerror(errno));
        return 1;
    }

    fails = 0;
    snprintf(regular_font_path, sizeof(regular_font_path),
             "/tmp/libmdf-cmdf-regular-%ld.woff2", (long)getpid());
    snprintf(italic_font_path, sizeof(italic_font_path),
             "/tmp/libmdf-cmdf-italic-%ld.woff2", (long)getpid());
    unlink(regular_font_path);
    unlink(italic_font_path);
    strcpy(font_dir, "/tmp/libmdf-cmdf-font-dir-XXXXXX");
    if (mkdtemp(font_dir) == NULL) {
        unlink(input_path);
        unlink(deck_input_path);
        return 1;
    }
    snprintf(directory_regular_font_path, sizeof(directory_regular_font_path),
             "%s/JetBrainsMono-Regular.woff2", font_dir);
    snprintf(directory_italic_font_path, sizeof(directory_italic_font_path),
             "%s/JetBrainsMono-Italic.woff2", font_dir);
    snprintf(font_dir_uri, sizeof(font_dir_uri), "file://%s", font_dir);

    help_args[0] = argv[1];
    help_args[1] = "--help";
    help_args[2] = NULL;
    if (run_cmdf(help_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        fprintf(stderr, "run cmdf help: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf help exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "--deck") != NULL &&
                    strstr(result.buf, "--transition MODE") != NULL &&
                    strstr(result.buf, "--slide-numbers") != NULL &&
                    strstr(result.buf, "--deck-center-front-text") != NULL,
                    "cmdf help lists deck flags");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "--html-disable-embedded-font") != NULL &&
                    strstr(result.buf, "--html-font-regular-uri") != NULL &&
                    strstr(result.buf, "--html-font-italic-uri") != NULL &&
                    strstr(result.buf, "--html-dump-font-regular-path") != NULL &&
                    strstr(result.buf, "--html-dump-font-italic-path") != NULL,
                    "cmdf help lists paired HTML font flags");
    free(result.buf);

    external_font_args[0] = argv[1];
    external_font_args[1] = "--html";
    external_font_args[2] = "--html-disable-embedded-font";
    external_font_args[3] = input_path;
    external_font_args[4] = NULL;
    if (run_cmdf(external_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 &&
                    result.buf != NULL &&
                    strstr(result.buf, "JetBrainsMono-Regular.woff2") != NULL &&
                    strstr(result.buf, "JetBrainsMono-Italic.woff2") != NULL &&
                    strstr(result.buf, "data:font/woff2;base64,") == NULL,
                    "cmdf disabled embedded font uses paired external JetBrains URIs");
    free(result.buf);

    dump_font_args[0] = argv[1];
    dump_font_args[1] = "--html";
    dump_font_args[2] = "--html-font-uri";
    dump_font_args[3] = "web-fonts";
    dump_font_args[4] = "--html-dump-font-regular-path";
    dump_font_args[5] = regular_font_path;
    dump_font_args[6] = "--html-dump-font-italic-path";
    dump_font_args[7] = italic_font_path;
    dump_font_args[8] = input_path;
    dump_font_args[9] = NULL;
    if (run_cmdf(dump_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(regular_font_path);
        unlink(italic_font_path);
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 &&
                    result.buf != NULL && strstr(result.buf, "web-fonts/JetBrainsMono-Regular.woff2") != NULL &&
                    strstr(result.buf, "web-fonts/JetBrainsMono-Italic.woff2") != NULL &&
                    strstr(result.buf, "data:font/woff2;base64,") == NULL,
                    "cmdf explicit dump destinations preserve HTML font references");
    free(result.buf);
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "cmdf writes regular WOFF2 font at dump path");
    font_fp = fopen(italic_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "cmdf writes italic WOFF2 font at dump path");

    font_fp = fopen(regular_font_path, "wb");
    if (font_fp != NULL) {
        fputs("preserve", font_fp);
        fclose(font_fp);
    }
    if (run_cmdf(dump_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(regular_font_path);
        unlink(italic_font_path);
        return 1;
    }
    free(result.buf);
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'p', "cmdf preserves an existing dumped font by default");

    dump_font_args[0] = argv[1];
    dump_font_args[1] = "--html";
    dump_font_args[2] = "--html-dump-font-force";
    dump_font_args[3] = "--html-dump-font-regular-path";
    dump_font_args[4] = regular_font_path;
    dump_font_args[5] = "--html-dump-font-italic-path";
    dump_font_args[6] = italic_font_path;
    dump_font_args[7] = input_path;
    dump_font_args[8] = NULL;
    if (run_cmdf(dump_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(regular_font_path);
        unlink(italic_font_path);
        return 1;
    }
    free(result.buf);
    font_fp = fopen(regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "cmdf force dump overwrites an existing font");
    unlink(regular_font_path);
    unlink(italic_font_path);

    dump_font_args[0] = argv[1];
    dump_font_args[1] = "--html";
    dump_font_args[2] = "--html-font-uri";
    dump_font_args[3] = font_dir_uri;
    dump_font_args[4] = "--html-dump-font";
    dump_font_args[5] = input_path;
    dump_font_args[6] = NULL;
    if (run_cmdf(dump_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(directory_regular_font_path);
        unlink(directory_italic_font_path);
        rmdir(font_dir);
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 &&
                    result.buf != NULL &&
                    strstr(result.buf, "file://") != NULL &&
                    strstr(result.buf, directory_regular_font_path) != NULL &&
                    strstr(result.buf, "data:font/woff2;base64,") == NULL,
                    "cmdf file URI derives paired destinations and references");
    free(result.buf);
    font_fp = fopen(directory_regular_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "cmdf font directory writes regular WOFF2 destination");
    font_fp = fopen(directory_italic_font_path, "rb");
    first_font_byte = font_fp == NULL ? EOF : fgetc(font_fp);
    if (font_fp != NULL) fclose(font_fp);
    fails += expect(first_font_byte == 'w', "cmdf font directory writes italic WOFF2 destination");
    unlink(directory_regular_font_path);
    unlink(directory_italic_font_path);

    dump_font_args[0] = argv[1];
    dump_font_args[1] = "--html";
    dump_font_args[2] = "--html-font-uri";
    dump_font_args[3] = "https://example.invalid/fonts";
    dump_font_args[4] = "--html-dump-font-path";
    dump_font_args[5] = font_dir;
    dump_font_args[6] = input_path;
    dump_font_args[7] = NULL;
    if (run_cmdf(dump_font_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(directory_regular_font_path);
        unlink(directory_italic_font_path);
        rmdir(font_dir);
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 &&
                    result.buf != NULL &&
                    strstr(result.buf, "https://example.invalid/fonts/JetBrainsMono-Regular.woff2") != NULL &&
                    strstr(result.buf, "https://example.invalid/fonts/JetBrainsMono-Italic.woff2") != NULL &&
                    strstr(result.buf, "data:font/woff2;base64,") == NULL,
                    "cmdf dump path preserves remote font URI references");
    free(result.buf);
    unlink(directory_regular_font_path);
    unlink(directory_italic_font_path);
    rmdir(font_dir);

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
        unlink(deck_input_path);
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

    html_title_stdin_args[0] = argv[1];
    html_title_stdin_args[1] = "--html";
    html_title_stdin_args[2] = "-T";
    html_title_stdin_args[3] = "Pipe Title";
    html_title_stdin_args[4] = NULL;
    first_chunk = NULL;
    first_ms = -1;
    if (run_cmdf_stdin_first_chunk(html_title_stdin_args,
                                   "unfinished paragraph",
                                   "\n\n# Later Heading\n",
                                   "<title>Pipe Title</title>",
                                   &result,
                                   &first_chunk,
                                   &first_ms) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html stdin title stream probe: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html stdin title override exits successfully");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<title>Pipe Title</title>") != NULL,
                    "cmdf html stdin title override emits shell before completed input");
    fails += expect(first_ms >= 0 && first_ms < 1500L,
                    "cmdf html stdin title override does not wait for a heading");
    free(first_chunk);
    free(result.buf);

    html_autotitle_stdin_args[0] = argv[1];
    html_autotitle_stdin_args[1] = "--html";
    html_autotitle_stdin_args[2] = NULL;
    first_chunk = NULL;
    first_ms = -1;
    if (run_cmdf_stdin_first_chunk(html_autotitle_stdin_args,
                                   "unfinished paragraph",
                                   "\n\n# Later Heading\n",
                                   "<title>mdf</title>",
                                   &result,
                                   &first_chunk,
                                   &first_ms) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html stdin autotitle skip stream probe: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html stdin autotitle skip exits successfully");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<title>mdf</title>") != NULL,
                    "cmdf html stdin autotitle skip emits default shell before completed input");
    fails += expect(first_ms >= 0 && first_ms < 1500L,
                    "cmdf html stdin autotitle skip does not wait for paragraph newline");
    fails += expect(result.buf != NULL && strstr(result.buf, "unfinished paragraph") != NULL,
                    "cmdf html stdin autotitle skip replays prescanned paragraph prefix");
    free(first_chunk);
    free(result.buf);

    first_chunk = NULL;
    first_ms = -1;
    if (run_cmdf_stdin_first_chunk(html_autotitle_stdin_args,
                                   "    ",
                                   "indented paragraph\n# Later Heading\n",
                                   "<title>mdf</title>",
                                   &result,
                                   &first_chunk,
                                   &first_ms) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf html stdin indented autotitle stream probe: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html stdin indented autotitle exits successfully");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<title>mdf</title>") != NULL,
                    "cmdf html stdin indented autotitle emits default shell before completed input");
    fails += expect(first_ms >= 0 && first_ms < 1500L,
                    "cmdf html stdin indented autotitle does not wait after four spaces");
    free(first_chunk);
    free(result.buf);

    deck_title_stdin_args[0] = argv[1];
    deck_title_stdin_args[1] = "--deck";
    deck_title_stdin_args[2] = "-T";
    deck_title_stdin_args[3] = "Deck Pipe Title";
    deck_title_stdin_args[4] = NULL;
    first_chunk = NULL;
    first_ms = -1;
    if (run_cmdf_stdin_first_chunk(deck_title_stdin_args,
                                   "# First streamed slide\n\n---\n",
                                   "\n# Second streamed slide\n",
                                   "<title>Deck Pipe Title</title>",
                                   &result,
                                   &first_chunk,
                                   &first_ms) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf deck stdin title stream probe: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck stdin title override exits successfully");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<title>Deck Pipe Title</title>") != NULL,
                    "cmdf deck stdin title override emits first slide before completed input");
    fails += expect(first_chunk != NULL && strstr(first_chunk, "<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->") != NULL,
                    "cmdf deck emits libmdf provenance comment");
    fails += expect(first_ms >= 0 && first_ms < 1500L,
                    "cmdf deck stdin title override streams after first slide boundary");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\" data-transition=\"fade\"") != NULL &&
                    strstr(result.buf, "First streamed slide") != NULL &&
                    strstr(result.buf, "Second streamed slide") != NULL,
                    "cmdf deck stdin title override renders streamed deck content");
    free(first_chunk);
    free(result.buf);

    snprintf(output_path, sizeof(output_path), "/tmp/libmdf-cmdf-%ld.html", (long)getpid());
    snprintf(output_path_2, sizeof(output_path_2), "/tmp/libmdf-cmdf-%ld-2.html", (long)getpid());
    unlink(output_path);
    unlink(output_path_2);
    infer_html_args[0] = argv[1];
    infer_html_args[1] = "-o";
    infer_html_args[2] = output_path;
    infer_html_args[3] = input_path;
    infer_html_args[4] = NULL;
    if (run_cmdf_stderr(infer_html_args, &result) != 0) {
        unlink(input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf inferred html: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf inferred html run exits successfully");
    fails += expect(result.buf != NULL && strstr(result.buf, "warning") != NULL &&
                    strstr(result.buf, "--html") != NULL,
                    "cmdf warns when inferring html output from extension");
    free(result.buf);
    html_text = NULL;
    if (read_file_text(output_path, &html_text) != 0) {
        unlink(input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "read inferred html output: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(strstr(html_text, "<!doctype html>") != NULL,
                    "cmdf inferred .html output renders html");
    fails += expect(strstr(html_text, "<title>Demo</title>") != NULL,
                    "cmdf inferred html title uses first heading without marker");
    free(html_text);

    deck_args[0] = argv[1];
    deck_args[1] = "--deck";
    deck_args[2] = "--slide-numbers";
    deck_args[3] = "-x";
    deck_args[4] = "cross";
    deck_args[5] = deck_input_path;
    deck_args[6] = NULL;
    if (run_cmdf(deck_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck run exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\" data-transition=\"cross\"") != NULL &&
                    strstr(result.buf, ".mdf-deck[data-transition=\"cross\"] .mdf-slide{transition:opacity 1600ms ease,visibility 0s linear 1600ms;}") != NULL &&
                    strstr(result.buf, ".mdf-deck[data-transition=\"cross\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 1600ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(result.buf, "<title>Front</title>") != NULL &&
                    strstr(result.buf, "<section class=\"mdf-slide mdf-slide-front\" data-slide=\"1\" aria-hidden=\"false\">") != NULL &&
                    strstr(result.buf, "<section class=\"mdf-slide\" data-slide=\"2\" aria-hidden=\"true\">") != NULL &&
                    strstr(result.buf, "<div class=\"mdf-slide-number\" aria-hidden=\"true\"></div>") != NULL,
                    "cmdf deck emits deck shell, slides, and slide-number placeholders");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "touchstart") != NULL &&
                    strstr(result.buf, "touchend") != NULL &&
                    strstr(result.buf, "Math.abs(dx)>=48") != NULL &&
                    strstr(result.buf, "Math.abs(dy)<48") != NULL &&
                    strstr(result.buf, "box.scrollHeight>box.clientHeight+4") != NULL,
                    "cmdf deck emits mobile swipe navigation");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "mdf-fullscreen-button") != NULL &&
                    strstr(result.buf, "toggleFullscreen") != NULL &&
                    strstr(result.buf, "case'f'") != NULL &&
                    strstr(result.buf, "requestFullscreen") != NULL &&
                    strstr(result.buf, "mdf-fallback-fullscreen") != NULL,
                    "cmdf deck emits fullscreen control");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "function syncSlides()") != NULL &&
                    strstr(result.buf, "data-mdf-tabindex") != NULL &&
                    strstr(result.buf, "el.setAttribute('tabindex','-1')") != NULL &&
                    strstr(result.buf, "t.closest('a[href],button,summary,input,select,textarea')") != NULL &&
                    strstr(result.buf, "deck.dataset.current=String(cur+1)") != NULL,
                    "cmdf deck emits inactive slide focus management");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "@media (max-height:520px)") != NULL &&
                    strstr(result.buf, "var vh=Math.max") != NULL &&
                    strstr(result.buf, "if(vh<520)") != NULL,
                    "cmdf deck emits short-viewport autofit rules");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "mdf-slide-content mdf-slide-content-centered") != NULL &&
                    strstr(result.buf, "<div class=\"mdf-slide-header\"><span class=\"mdf-heading\"") != NULL &&
                    strstr(result.buf, "</div><div class=\"mdf-slide-body\"><div class=\"mdf-slide-body-inner\">") != NULL,
                    "cmdf deck centers non-front slide body content by default");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "theme:") == NULL &&
                    strstr(result.buf, "tokyo-night") == NULL &&
                    strstr(result.buf, "color:rgb(255,175,215)") != NULL &&
                    strstr(result.buf, "<div class=\"mdf-chart-block\"") != NULL,
                    "cmdf deck consumes front matter theme and renders chart slide");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, ".mdf-slide-number{font-weight:700;opacity:.72;font-size:clamp(.8rem,1.6vw,1.1rem);color:rgb(135,175,215);}") != NULL,
                    "cmdf deck slide numbers use resolved theme heading color");
    free(result.buf);

    deck_args[0] = argv[1];
    deck_args[1] = "--deck";
    deck_args[2] = "--html-font-uri";
    deck_args[3] = "deck-fonts";
    deck_args[4] = deck_input_path;
    deck_args[5] = NULL;
    if (run_cmdf(deck_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0 &&
                    result.buf != NULL &&
                    strstr(result.buf, "deck-fonts/JetBrainsMono-Regular.woff2") != NULL &&
                    strstr(result.buf, "deck-fonts/JetBrainsMono-Italic.woff2") != NULL &&
                    strstr(result.buf, "data:font/woff2;base64,") == NULL,
                    "cmdf deck external font URI omits embedded faces");
    free(result.buf);

    deck_boring_args[0] = argv[1];
    deck_boring_args[1] = "--deck";
    deck_boring_args[2] = "--boring";
    deck_boring_args[3] = deck_input_path;
    deck_boring_args[4] = NULL;
    if (run_cmdf(deck_boring_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck boring: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --deck --boring exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\"") != NULL &&
                    strstr(result.buf, "html,body{margin:0;min-height:100%;background:transparent;}") != NULL &&
                    strstr(result.buf, "body{color:rgb(0,0,0);") != NULL,
                    "cmdf --boring applies base html colors in deck mode");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<div class=\"mdf-chart-block\"") != NULL &&
                    strstr(result.buf, "background-color:rgb(205,0,205)") == NULL &&
                    strstr(result.buf, "background-color:rgb(59,156,255)") == NULL,
                    "cmdf --boring suppresses themed chart colors in deck mode");
    free(result.buf);

    deck_theme_default_args[0] = argv[1];
    deck_theme_default_args[1] = "--deck";
    deck_theme_default_args[2] = "--theme";
    deck_theme_default_args[3] = "default";
    deck_theme_default_args[4] = deck_input_path;
    deck_theme_default_args[5] = NULL;
    if (run_cmdf(deck_theme_default_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck explicit default theme: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck explicit default theme exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "theme:") == NULL &&
                    strstr(result.buf, "color:rgb(255,175,215)") == NULL,
                    "cmdf --theme default overrides front matter theme");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "color:rgb(135,175,215)") == NULL,
                    "cmdf --theme default overrides slide-number theme color");
    free(result.buf);

    {
        int fd;
        const char *src;

        src = "---\n"
              "theme: \"\n"
              "---\n"
              "# Malformed Theme\n"
              "\n"
              "Visible body\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
    }
    deck_malformed_theme_args[0] = argv[1];
    deck_malformed_theme_args[1] = "--deck";
    deck_malformed_theme_args[2] = input_path;
    deck_malformed_theme_args[3] = NULL;
    if (run_cmdf(deck_malformed_theme_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck malformed theme: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck malformed front matter theme exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "Malformed Theme") != NULL &&
                    strstr(result.buf, "Visible body") != NULL &&
                    strstr(result.buf, "theme:") == NULL,
                    "cmdf deck malformed front matter theme is consumed");
    free(result.buf);

    html_deck_order_args[0] = argv[1];
    html_deck_order_args[1] = "--html";
    html_deck_order_args[2] = "--deck";
    html_deck_order_args[3] = deck_input_path;
    html_deck_order_args[4] = NULL;
    if (run_cmdf(html_deck_order_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf html then deck: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --html --deck exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\"") != NULL &&
                    strstr(result.buf, "<main class=\"mdf-document\">") == NULL,
                    "cmdf --html --deck renders deck mode");
    free(result.buf);

    deck_html_order_args[0] = argv[1];
    deck_html_order_args[1] = "--deck";
    deck_html_order_args[2] = "--html";
    deck_html_order_args[3] = deck_input_path;
    deck_html_order_args[4] = NULL;
    if (run_cmdf(deck_html_order_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck then html: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --deck --html exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\"") != NULL &&
                    strstr(result.buf, "<main class=\"mdf-document\">") == NULL,
                    "cmdf --deck --html keeps deck mode");
    free(result.buf);

    deck_fade_args[0] = argv[1];
    deck_fade_args[1] = "--deck";
    deck_fade_args[2] = "--transition";
    deck_fade_args[3] = "fade";
    deck_fade_args[4] = deck_input_path;
    deck_fade_args[5] = NULL;
    if (run_cmdf(deck_fade_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck fade transition: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --transition fade exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\" data-transition=\"fade\"") != NULL &&
                    strstr(result.buf, ".mdf-deck[data-transition=\"fade\"] .mdf-slide{transition:opacity 520ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(result.buf, ".mdf-deck[data-transition=\"fade\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 520ms ease,visibility 0s linear 0s;}") != NULL &&
                    strstr(result.buf, "matchMedia('(prefers-reduced-motion: reduce)')") != NULL &&
                    strstr(result.buf, "deck.dataset.transition==='fade'&&!reduceMotion&&old!==cur") != NULL &&
                    strstr(result.buf, "setTimeout(function(){apply();void deck.offsetWidth;deck.classList.remove('mdf-blackout');},520)") != NULL,
                    "cmdf --transition fade is reflected in deck output");
    free(result.buf);

    deck_center_front_text_args[0] = argv[1];
    deck_center_front_text_args[1] = "--deck";
    deck_center_front_text_args[2] = "--deck-center-front-text";
    deck_center_front_text_args[3] = deck_input_path;
    deck_center_front_text_args[4] = NULL;
    if (run_cmdf(deck_center_front_text_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck center front text: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --deck-center-front-text exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<section class=\"mdf-slide mdf-slide-front mdf-center-front-text\" data-slide=\"1\" aria-hidden=\"false\">") != NULL &&
                    count_substrings(result.buf, "<section class=\"mdf-slide mdf-slide-front mdf-center-front-text\"") == 1 &&
                    strstr(result.buf, "<section class=\"mdf-slide mdf-center-front-text\"") == NULL,
                    "cmdf --deck-center-front-text only marks the front slide");
    free(result.buf);

    deck_content_width_args[0] = argv[1];
    deck_content_width_args[1] = "--deck";
    deck_content_width_args[2] = "--html-content-width";
    deck_content_width_args[3] = "72";
    deck_content_width_args[4] = deck_input_path;
    deck_content_width_args[5] = NULL;
    if (run_cmdf(deck_content_width_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck content width: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --deck --html-content-width exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\"") != NULL &&
                    strstr(result.buf, "--mdf-content-max-width:72ch;") != NULL,
                    "cmdf --html-content-width applies in deck mode");
    free(result.buf);

    deck_width_args[0] = argv[1];
    deck_width_args[1] = "--deck";
    deck_width_args[2] = "-w";
    deck_width_args[3] = "68";
    deck_width_args[4] = deck_input_path;
    deck_width_args[5] = NULL;
    if (run_cmdf(deck_width_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck width: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf --deck -w exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<main class=\"mdf-deck\"") != NULL &&
                    strstr(result.buf, "--mdf-content-max-width:68ch;") != NULL,
                    "cmdf -w maps to html content width in deck mode");
    free(result.buf);

    bad_transition_args[0] = argv[1];
    bad_transition_args[1] = "--deck";
    bad_transition_args[2] = "-x";
    bad_transition_args[3] = "wipe";
    bad_transition_args[4] = deck_input_path;
    bad_transition_args[5] = NULL;
    if (run_cmdf_stderr(bad_transition_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf bad deck transition: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 2,
                    "cmdf invalid deck transition exits with usage error");
    fails += expect(result.buf != NULL && strstr(result.buf, "invalid deck transition") != NULL,
                    "cmdf invalid deck transition explains allowed values");
    free(result.buf);

    transition_without_deck_args[0] = argv[1];
    transition_without_deck_args[1] = "--transition";
    transition_without_deck_args[2] = "fade";
    transition_without_deck_args[3] = input_path;
    transition_without_deck_args[4] = NULL;
    if (run_cmdf_stderr(transition_without_deck_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf transition without deck rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 2,
                    "cmdf rejects --transition without --deck");
    fails += expect(result.buf != NULL && strstr(result.buf, "deck options require --deck") != NULL,
                    "cmdf --transition without --deck explains deck requirement");
    free(result.buf);

    slide_numbers_without_deck_args[0] = argv[1];
    slide_numbers_without_deck_args[1] = "--slide-numbers";
    slide_numbers_without_deck_args[2] = "-o";
    slide_numbers_without_deck_args[3] = output_path_2;
    slide_numbers_without_deck_args[4] = input_path;
    slide_numbers_without_deck_args[5] = NULL;
    unlink(output_path_2);
    if (run_cmdf_stderr(slide_numbers_without_deck_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf slide numbers without deck rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 2,
                    "cmdf rejects --slide-numbers without --deck even when .html is inferred");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "inferring --html") != NULL &&
                    strstr(result.buf, "deck options require --deck") != NULL,
                    "cmdf --slide-numbers without --deck explains inferred html and deck requirement");
    free(result.buf);
    unlink(output_path_2);

    deck_output_args[0] = argv[1];
    deck_output_args[1] = "--deck";
    deck_output_args[2] = "--transition";
    deck_output_args[3] = "hard";
    deck_output_args[4] = "-o";
    deck_output_args[5] = output_path_2;
    deck_output_args[6] = deck_input_path;
    deck_output_args[7] = NULL;
    unlink(output_path_2);
    if (run_cmdf(deck_output_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck output: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck output run exits successfully");
    free(result.buf);
    html_text = NULL;
    if (read_file_text(output_path_2, &html_text) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "read deck output: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(strstr(html_text, "<main class=\"mdf-deck\" data-transition=\"hard\"") != NULL,
                    "cmdf --deck -o writes deck html even with .html path");
    free(html_text);

    deck_title_args[0] = argv[1];
    deck_title_args[1] = "--deck";
    deck_title_args[2] = "-T";
    deck_title_args[3] = "Deck & <Title>";
    deck_title_args[4] = "-o";
    deck_title_args[5] = output_path_2;
    deck_title_args[6] = deck_input_path;
    deck_title_args[7] = NULL;
    if (run_cmdf(deck_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck title override: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck title override exits successfully");
    free(result.buf);
    html_text = NULL;
    if (read_file_text(output_path_2, &html_text) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "read deck title override output: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(strstr(html_text, "<title>Deck &amp; &lt;Title&gt;</title>") != NULL,
                    "cmdf deck title override is escaped");
    free(html_text);

    {
        int fd;
        const char *src;

        src = "---   \n"
              "title: Metadata Title\n"
              "theme: \"tokyo-night\"\n"
              "tags:\n"
              "  - alpha\n"
              "  - beta\n"
              "# title pre-scan comment\n"
              "--- \t\n"
              "# Deck Heading Title\n"
              "\n"
              "---\n"
              "\n"
              "# Second\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
    }
    deck_frontmatter_title_args[0] = argv[1];
    deck_frontmatter_title_args[1] = "--deck";
    deck_frontmatter_title_args[2] = input_path;
    deck_frontmatter_title_args[3] = NULL;
    if (run_cmdf(deck_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Deck Heading Title</title>") != NULL &&
                    strstr(result.buf, "Metadata Title") == NULL &&
                    strstr(result.buf, "title pre-scan comment") == NULL &&
                    strstr(result.buf, "theme:") == NULL &&
                    strstr(result.buf, "tags:") == NULL &&
                    strstr(result.buf, "alpha") == NULL &&
                    strstr(result.buf, "beta") == NULL,
                    "cmdf deck title detection skips spaced front matter delimiters and unindented comments");
    free(result.buf);

    html_frontmatter_title_args[0] = argv[1];
    html_frontmatter_title_args[1] = "--html";
    html_frontmatter_title_args[2] = input_path;
    html_frontmatter_title_args[3] = NULL;
    if (run_cmdf(html_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf html front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Deck Heading Title</title>") != NULL &&
                    strstr(result.buf, "Metadata Title") == NULL &&
                    strstr(result.buf, "title pre-scan comment") == NULL &&
                    strstr(result.buf, "theme:") == NULL &&
                    strstr(result.buf, "tags:") == NULL &&
                    strstr(result.buf, "alpha") == NULL &&
                    strstr(result.buf, "beta") == NULL,
                    "cmdf html title detection skips spaced front matter delimiters and unindented comments");
    free(result.buf);

    {
        int fd;
        const char *src;

        src = "---\n"
              "# Intro\n"
              "---\n"
              "# Later\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
    }
    if (run_cmdf(deck_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck rejected front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck rejected front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Intro</title>") != NULL &&
                    strstr(result.buf, "<title>Later</title>") == NULL,
                    "cmdf deck title detection scans no-key front matter-looking block");
    free(result.buf);

    if (run_cmdf(html_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf html rejected front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html rejected front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Intro</title>") != NULL &&
                    strstr(result.buf, "<title>Later</title>") == NULL,
                    "cmdf html title detection scans no-key front matter-looking block");
    free(result.buf);

    {
        int fd;
        const char *src;

        src = "---\n"
              "theme: default\n"
              "# Intro\n";
        fd = open(input_path, O_WRONLY | O_TRUNC);
        if (fd < 0) {
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
        if (write_all(fd, src, strlen(src)) != 0 || close(fd) != 0) {
            close(fd);
            unlink(input_path);
            unlink(deck_input_path);
            return 1;
        }
    }
    if (run_cmdf(deck_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf deck unclosed front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf deck unclosed front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Intro</title>") != NULL,
                    "cmdf deck title detection scans unclosed keyed front matter body");
    free(result.buf);

    if (run_cmdf(html_frontmatter_title_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf html unclosed front matter title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html unclosed front matter title exits successfully");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "<title>Intro</title>") != NULL,
                    "cmdf html title detection scans unclosed keyed front matter body");
    free(result.buf);

    html_title_args[0] = argv[1];
    html_title_args[1] = "--html";
    html_title_args[2] = "-T";
    html_title_args[3] = "Manual & <Title>";
    html_title_args[4] = "-o";
    html_title_args[5] = output_path_2;
    html_title_args[6] = input_path;
    html_title_args[7] = NULL;
    if (run_cmdf(html_title_args, &result) != 0) {
        unlink(input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "run cmdf html title override: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html title override exits successfully");
    free(result.buf);
    html_text = NULL;
    if (read_file_text(output_path_2, &html_text) != 0) {
        unlink(input_path);
        unlink(output_path);
        unlink(output_path_2);
        fprintf(stderr, "read html title override output: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(strstr(html_text, "<title>Manual &amp; &lt;Title&gt;</title>") != NULL,
                    "cmdf html title override is escaped");
    free(html_text);
    unlink(output_path);
    unlink(output_path_2);

    {
        int fd;
        const char *src;

        src = "\t\n# Tab Title\n\nbody\n";
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
    html_paragraph_title_args[0] = argv[1];
    html_paragraph_title_args[1] = "--html";
    html_paragraph_title_args[2] = input_path;
    html_paragraph_title_args[3] = NULL;
    if (run_cmdf(html_paragraph_title_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf tab-blank-before-title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html tab-blank-before-heading exits successfully");
    fails += expect(result.buf != NULL && strstr(result.buf, "<title>mdf</title>") != NULL,
                    "cmdf html title stops when leading tab rules out a heading");
    free(result.buf);

    {
        int fd;
        const char *src;

        src = "intro paragraph\n\n# Later Heading\n";
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
    html_paragraph_title_args[0] = argv[1];
    html_paragraph_title_args[1] = "--html";
    html_paragraph_title_args[2] = input_path;
    html_paragraph_title_args[3] = NULL;
    if (run_cmdf(html_paragraph_title_args, &result) != 0) {
        unlink(input_path);
        fprintf(stderr, "run cmdf paragraph-before-title: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
                    "cmdf html paragraph-before-heading exits successfully");
    fails += expect(result.buf != NULL && strstr(result.buf, "<title>mdf</title>") != NULL,
                    "cmdf html title defaults when a paragraph precedes the first heading");
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

    deck_trace_args[0] = argv[1];
    deck_trace_args[1] = "--deck";
    deck_trace_args[2] = "--trace-writes";
    deck_trace_args[3] = "/tmp/libmdf-cmdf-deck-trace.out";
    deck_trace_args[4] = deck_input_path;
    deck_trace_args[5] = NULL;
    if (run_cmdf_stderr(deck_trace_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        fprintf(stderr, "run cmdf deck trace rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects --trace-writes for deck output");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "--trace-writes is only supported for ANSI output") != NULL,
                    "cmdf deck trace rejection explains ANSI-only trace support");
    free(result.buf);
    unlink("/tmp/libmdf-cmdf-deck-trace.out");

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

    ascii_deck_args[0] = argv[1];
    ascii_deck_args[1] = "--deck";
    ascii_deck_args[2] = "--table-wire";
    ascii_deck_args[3] = "ascii";
    ascii_deck_args[4] = deck_input_path;
    ascii_deck_args[5] = NULL;
    if (run_cmdf_stderr(ascii_deck_args, &result) != 0) {
        unlink(input_path);
        unlink(deck_input_path);
        fprintf(stderr, "run cmdf deck ascii-wire rejection: %s\n", strerror(errno));
        return 1;
    }
    fails += expect(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
                    "cmdf rejects --table-wire ascii with deck output");
    fails += expect(result.buf != NULL &&
                    strstr(result.buf, "--table-wire ascii is not supported with HTML output") != NULL,
                    "cmdf deck ascii-wire rejection explains HTML wire modes");
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
    unlink(deck_input_path);
    return fails == 0 ? 0 : 1;
}
