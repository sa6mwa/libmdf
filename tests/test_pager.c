#define _XOPEN_SOURCE 600

#include <libmdf/mdf.h>

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct capture {
    char *data;
    size_t len;
    size_t cap;
} capture;

static int capture_append(capture *out, const char *src, size_t len)
{
    char *next;
    size_t required;
    size_t cap;

    required = out->len + len + 1;
    if (required < out->len) return -1;
    if (required > out->cap) {
        cap = out->cap == 0 ? 4096 : out->cap;
        while (cap < required) cap *= 2;
        next = (char *)realloc(out->data, cap);
        if (next == NULL) return -1;
        out->data = next;
        out->cap = cap;
    }
    memcpy(out->data + out->len, src, len);
    out->len += len;
    out->data[out->len] = '\0';
    return 0;
}

static int wait_for_output(int fd, capture *out, long milliseconds)
{
    long elapsed;

    elapsed = 0;
    while (elapsed < milliseconds) {
        fd_set reads;
        struct timeval timeout;
        char bytes[1024];
        ssize_t n;
        int ready;

        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        ready = select(fd + 1, &reads, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) return -1;
        if (ready > 0) {
            n = read(fd, bytes, sizeof(bytes));
            if (n > 0 && capture_append(out, bytes, (size_t)n) != 0) return -1;
        }
        elapsed += 20;
    }
    return 0;
}

static int wait_for_marker(int fd, capture *out, const char *marker)
{
    long elapsed;

    elapsed = 0;
    while (elapsed < 2000) {
        if (out->data != NULL && strstr(out->data, marker) != NULL) return 0;
        if (wait_for_output(fd, out, 20) != 0) return -1;
        elapsed += 20;
    }
    return -1;
}

static int write_all(int fd, const char *src, size_t len)
{
    while (len > 0) {
        ssize_t n;

        n = write(fd, src, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        src += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int ignored_trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    (void)userdata;
    (void)format;
    (void)src;
    (void)len;
    return 0;
}

static pid_t start_pager(const char *cmdf, const char *path, int direct, int osc8, int traced, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    if (direct) {
        mdf_options opts;

        mdf_options_init(&opts);
        opts.osc8 = osc8;
        if (traced) opts.write_trace.emit = ignored_trace_emit;
        _exit(mdf_pager_file(path, &opts, MDF_PAGER_FORMAT_AUTO) == MDF_OK ? 0 : 1);
    }
    execl(cmdf, cmdf, "-p", path, (char *)NULL);
    _exit(127);
}

static int wait_for_exit(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int wait_for_failure(pid_t pid)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) != 0 ? 0 : -1;
}

static int wait_for_signal(pid_t pid, int signal_number)
{
    int status;

    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFSIGNALED(status) && WTERMSIG(status) == signal_number ? 0 : -1;
}

static int terminal_is_cooked(int fd)
{
    struct termios state;

    if (tcgetattr(fd, &state) != 0) return 0;
    return (state.c_lflag & (ECHO | ICANON | ISIG)) == (ECHO | ICANON | ISIG);
}

static int write_fixture(char *path, size_t cap, const char *suffix, int markdown)
{
    int fd;
    int i;
    char line[64];
    char temporary[128];

    if (snprintf(temporary, sizeof(temporary), "/tmp/libmdf-pager-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (markdown) {
        if (write_all(fd, "# rendered heading\n\n", strlen("# rendered heading\n\n")) != 0) return -1;
    } else if (write_all(fd, "# raw heading\n", strlen("# raw heading\n")) != 0) {
        return -1;
    }
    if (write_all(fd, "S\303\244kerhet S\303\204KERHET \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242 \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242\n",
                  strlen("S\303\244kerhet S\303\204KERHET \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242 \346\227\245\346\234\254\350\252\236\346\244\234\347\264\242\n")) != 0) {
        return -1;
    }
    for (i = 1; i <= 60; i++) {
        int len;

        len = snprintf(line, sizeof(line), "line-%02d narrow-resize-anchor-content\n", i);
        if (len < 0 || write_all(fd, line, (size_t)len) != 0) return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.%s", temporary, suffix) >= (int)cap ||
        rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_osc8_fixture(char *path, size_t cap)
{
    int fd;
    const char *source;
    char temporary[128];

    source = "Regeringens [molnpolicy f\303\266r Sverige](https://www.regeringen.se/contentassets/112463c2b5404f84826545ba01967f28/en-molnpolicy-for-sverige--for-okad-sakerhet-effektivitet-och-innovation-i-den-offentliga-forvaltningen1.pdf) f\303\266rbjuder inte Azure och kr\303\244ver inte ett svenskt eller suver\303\244nt moln. Den betonar d\303\244remot kontroll, portabilitet, ers\303\244ttningsstrategier, *exit* och kontinuitet.\n";
    if (snprintf(temporary, sizeof(temporary), "/tmp/libmdf-pager-osc8-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, strlen(source)) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_wide_fixture(char *path, size_t cap)
{
    int fd;
    const char *source;
    char temporary[128];

    source = "\346\227\245\346\234\254\350\252\236\346\227\245\346\234\254\350\252\236\n";
    if (snprintf(temporary, sizeof(temporary), "/tmp/libmdf-pager-wide-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, source, strlen(source)) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.txt", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_status_path_fixture(char *path, size_t cap, const char *suffix)
{
    int fd;
    char temporary[128];

    if (snprintf(temporary, sizeof(temporary), "/tmp/libmdf-pager-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, "status fixture\n", strlen("status fixture\n")) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s%s", temporary, suffix) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int write_styled_search_fixture(char *path, size_t cap)
{
    int fd;
    char temporary[128];

    if (snprintf(temporary, sizeof(temporary), "/tmp/libmdf-pager-styled-XXXXXX") >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (write_all(fd, "foo `bar` baz\n", strlen("foo `bar` baz\n")) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.md", temporary) >= (int)cap || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int require_contains(const capture *out, const char *needle)
{
    return out->data != NULL && strstr(out->data, needle) != NULL ? 0 : -1;
}

static int require_full_status_bar(const capture *out, const char *path)
{
    const char *prefix;
    const char *suffix;
    char expected[128];
    size_t path_len;
    size_t shown_path_len;
    size_t position;

    prefix = "\033[7m\033[1;32m ";
    suffix = "0% \033[K\033[0m";
    path_len = strlen(path);
    shown_path_len = path_len > 36 ? 36 : path_len;
    position = 0;
    memcpy(expected + position, prefix, strlen(prefix));
    position += strlen(prefix);
    memcpy(expected + position, path, shown_path_len);
    position += shown_path_len;
    memset(expected + position, ' ', 36 - shown_path_len);
    position += 36 - shown_path_len;
    memcpy(expected + position, suffix, strlen(suffix));
    position += strlen(suffix);
    expected[position] = '\0';
    return require_contains(out, expected);
}

static int clear_capture(capture *out)
{
    out->len = 0;
    if (out->data != NULL) out->data[0] = '\0';
    return 0;
}

static size_t count_occurrences(const char *text, const char *needle)
{
    size_t count;
    size_t len;

    count = 0;
    len = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += len;
    }
    return count;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

int main(int argc, char **argv)
{
    char text_path[128];
    char markdown_path[128];
    char osc8_path[128];
    char wide_path[128];
    char control_path[128];
    char unicode_path[128];
    char styled_path[128];
    capture out;
    pid_t pid;
    int master;
    int rc;
    int i;
    struct winsize resized;
    const char *stage;

    if (argc != 2) return 2;
    memset(&out, 0, sizeof(out));
    rc = 1;
    stage = "fixtures";
    if (write_fixture(text_path, sizeof(text_path), "text.txt", 0) != 0 ||
        write_fixture(markdown_path, sizeof(markdown_path), "markdown.md", 1) != 0 ||
        write_osc8_fixture(osc8_path, sizeof(osc8_path)) != 0 ||
        write_wide_fixture(wide_path, sizeof(wide_path)) != 0 ||
        write_status_path_fixture(control_path, sizeof(control_path), "\033]52;c;INJECT\a.txt") != 0 ||
        write_status_path_fixture(unicode_path, sizeof(unicode_path),
                                  "\346\227\245\346\234\254\350\252\236\346\227\245.txt") != 0 ||
        write_styled_search_fixture(styled_path, sizeof(styled_path)) != 0) goto done;

    stage = "text startup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        wait_for_marker(master, &out, "\033[K\033[0m") != 0 ||
        require_contains(&out, "# raw heading") != 0 ||
        require_contains(&out, "\033[7m\033[1;32m ") != 0 ||
        require_full_status_bar(&out, text_path) != 0 ||
        strstr(out.data, "\033[1;32m# raw heading") != NULL) goto done;
    clear_capture(&out);
    stage = "down";
    if (write_all(master, "j", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "up";
    if (write_all(master, "k", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "page down";
    if (write_all(master, "\033[6~", 4) != 0 || wait_for_marker(master, &out, "line-10") != 0) goto done;
    clear_capture(&out);
    stage = "page up";
    if (write_all(master, "\033[5~", 4) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "half down";
    if (write_all(master, "\004", 1) != 0 || wait_for_marker(master, &out, "line-05") != 0) goto done;
    clear_capture(&out);
    stage = "half up";
    if (write_all(master, "\025", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "space page down";
    if (write_all(master, " ", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "b page up";
    if (write_all(master, "b", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "ctrl-f page down";
    if (write_all(master, "\006", 1) != 0 || wait_for_marker(master, &out, "line-08") != 0) goto done;
    clear_capture(&out);
    stage = "ctrl-b page up";
    if (write_all(master, "\002", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "d half down";
    if (write_all(master, "d", 1) != 0 || wait_for_marker(master, &out, "line-03") != 0) goto done;
    clear_capture(&out);
    stage = "u half up";
    if (write_all(master, "u", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "enter down";
    if (write_all(master, "\r", 1) != 0 || wait_for_marker(master, &out, "S\303\244kerhet") != 0) goto done;
    clear_capture(&out);
    stage = "g home";
    if (write_all(master, "g", 1) != 0 || wait_for_marker(master, &out, "# raw heading") != 0) goto done;
    clear_capture(&out);
    stage = "G end";
    if (write_all(master, "G", 1) != 0 || wait_for_marker(master, &out, "line-60") != 0) goto done;
    stage = "end";
    if (write_all(master, "\033[F", 3) != 0 || wait_for_marker(master, &out, "line-60") != 0 ||
        require_contains(&out, "line-60") != 0) goto done;
    stage = "home";
    if (write_all(master, "\033[H", 3) != 0 || wait_for_output(master, &out, 80) != 0) goto done;
    clear_capture(&out);
    stage = "live Swedish search";
    if (write_all(master, "/S\303\204K", strlen("/S\303\204K")) != 0 ||
        wait_for_marker(master, &out, "\033[7mS\303\244k\033[27m") != 0) goto done;
    clear_capture(&out);
    stage = "Swedish case-insensitive search";
    if (write_all(master, "ERHET", strlen("ERHET")) != 0 ||
        wait_for_marker(master, &out, "\033[7mS\303\244kerhet\033[27m") != 0) goto done;
    clear_capture(&out);
    stage = "confirm Swedish search";
    if (write_all(master, "\r", 1) != 0 ||
        wait_for_marker(master, &out, "/S\303\204KERHET  1/2") != 0 ||
        require_contains(&out, "\033[7mS\303\244kerhet\033[27m") != 0 ||
        require_contains(&out, "/S\303\204KERHET  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "next Swedish search hit";
    if (write_all(master, "n", 1) != 0 || wait_for_marker(master, &out, "/S\303\204KERHET  2/2") != 0 ||
        require_contains(&out, "\033[7mS\303\204KERHET\033[27m") != 0 ||
        require_contains(&out, "/S\303\204KERHET  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "previous Swedish search hit with N";
    if (write_all(master, "N", 1) != 0 || wait_for_marker(master, &out, "/S\303\204KERHET  1/2") != 0 ||
        require_contains(&out, "/S\303\204KERHET  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "previous Swedish search hit with p";
    if (write_all(master, "p", 1) != 0 || wait_for_marker(master, &out, "/S\303\204KERHET  2/2") != 0 ||
        require_contains(&out, "/S\303\204KERHET  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "exit Swedish search";
    if (write_all(master, "q", 1) != 0 || wait_for_marker(master, &out, "line-01") != 0 ||
        require_contains(&out, "line-01") != 0) goto done;
    clear_capture(&out);
    stage = "Japanese UTF-8 search";
    if (write_all(master, "/\346\227\245\346\234\254\350\252\236\r", strlen("/\346\227\245\346\234\254\350\252\236\r")) != 0 ||
        wait_for_marker(master, &out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0 ||
        require_contains(&out, "\033[7m\346\227\245\346\234\254\350\252\236\033[27m") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  1/2") != 0) goto done;
    clear_capture(&out);
    stage = "next Japanese search hit";
    if (write_all(master, "n", 1) != 0 || wait_for_marker(master, &out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0 ||
        require_contains(&out, "/\346\227\245\346\234\254\350\252\236  2/2") != 0) goto done;
    clear_capture(&out);
    stage = "narrow Japanese search status";
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 10;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "  /\346\227\245") != 0 ||
        require_contains(&out, "  /\346\227\245") != 0) goto done;
    clear_capture(&out);
    stage = "escape exits Japanese search";
    if (write_all(master, "\033", 1) != 0 || wait_for_marker(master, &out, "line-01") != 0) goto done;
    stage = "escape";
    if (write_all(master, "\033", 1) != 0 || wait_for_exit(pid) != 0 ||
        wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown direct";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "write trace rejected";
    pid = start_pager(argv[1], markdown_path, 1, 0, 1, &master);
    if (pid < 0 || wait_for_failure(pid) != 0 || wait_for_output(master, &out, 40) != 0 || out.len != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown cmdf";
    pid = start_pager(argv[1], markdown_path, 0, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "osc8 long URL";
    pid = start_pager(argv[1], osc8_path, 1, 1, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "molnpolicy") != 0 ||
        wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "\033]8;;https://www.regeringen.se/contentassets/112463c2b5404f84826545ba01967f28/en-molnpolicy-for-sverige--for-okad-sakerhet-effektivitet-och-innovation-i-den-offentliga-forvaltningen1.pdf\033\\") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "resize";
    pid = start_pager(argv[1], markdown_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        wait_for_output(master, &out, 100) != 0 ||
        write_all(master, "\033[F", 3) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-52") != 0) goto done;
    clear_capture(&out);
    if (wait_for_output(master, &out, 20) != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 20;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0) goto done;
    for (i = 0; i < 3; i++) {
        if (kill(pid, SIGWINCH) != 0) goto done;
        sleep_ms(50);
    }
    stage = "resize premature redraw";
    if (wait_for_output(master, &out, 50) != 0 || out.len != 0) goto done;
    stage = "resize settled redraw";
    if (wait_for_output(master, &out, 300) != 0 ||
        count_occurrences(out.data, "\033[H\033[2J") != 1 ||
        require_contains(&out, "line-52") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "narrow status bar";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        wait_for_output(master, &out, 80) != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 1;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out, "\033[H\033[2J") != 0 || out.len > 16384 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "wide utf8 reflow";
    pid = start_pager(argv[1], wide_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "\346\227\245\346\234\254\350\252\236\346\227\245\346\234\254\350\252\236") != 0 ||
        wait_for_output(master, &out, 80) != 0) goto done;
    clear_capture(&out);
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 4;
    resized.ws_row = 10;
    if (ioctl(master, TIOCSWINSZ, &resized) != 0 || kill(pid, SIGWINCH) != 0 ||
        wait_for_marker(master, &out,
                        "\033[K\346\227\245\346\234\254\033[0m\r\n\033[K\350\252\236\346\227\245\033[0m\r\n\033[K\346\234\254\350\252\236") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "control filename status";
    pid = start_pager(argv[1], control_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "status fixture") != 0 ||
        wait_for_marker(master, &out, "\033[7m\033[1;32m") != 0 ||
        require_contains(&out, "^[]52;c") != 0 ||
        strstr(out.data, "\033]52;c;INJECT\a") != NULL ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "unicode filename status";
    pid = start_pager(argv[1], unicode_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "status fixture") != 0 ||
        wait_for_marker(master, &out, "\033[7m\033[1;32m") != 0 ||
        require_contains(&out, "\346\227\245\346\234\254\350\252\236\346\227\245") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "styled search reset";
    pid = start_pager(argv[1], styled_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "foo ") != 0 ||
        write_all(master, "/bar baz\r", strlen("/bar baz\r")) != 0 ||
        wait_for_marker(master, &out, "bar\033[0m\033[7m baz") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_marker(master, &out, "foo ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "SIGTERM cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        kill(pid, SIGTERM) != 0 || wait_for_signal(pid, SIGTERM) != 0 ||
        wait_for_output(master, &out, 80) != 0 || require_contains(&out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    clear_capture(&out);

    stage = "ctrl-c cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        write_all(master, "\003", 1) != 0 || wait_for_signal(pid, SIGINT) != 0 ||
        wait_for_output(master, &out, 80) != 0 || require_contains(&out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    clear_capture(&out);

    stage = "SIGTSTP cleanup";
    pid = start_pager(argv[1], text_path, 1, 0, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        kill(pid, SIGTSTP) != 0 || wait_for_failure(pid) != 0 ||
        wait_for_output(master, &out, 80) != 0 || require_contains(&out, "\033[?1049l") != 0 ||
        !terminal_is_cooked(master)) goto done;
    close(master);
    rc = 0;

done:
    if (rc != 0) fprintf(stderr, "test_pager failed at %s\n", stage);
    unlink(text_path);
    unlink(markdown_path);
    unlink(osc8_path);
    unlink(wide_path);
    unlink(control_path);
    unlink(unicode_path);
    unlink(styled_path);
    free(out.data);
    return rc;
}
