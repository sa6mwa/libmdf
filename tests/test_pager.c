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

static pid_t start_pager(const char *cmdf, const char *path, int direct, int *master)
{
    struct winsize size;
    pid_t pid;

    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 10;
    pid = forkpty(master, NULL, NULL, &size);
    if (pid != 0) return pid;
    if (direct) {
        _exit(mdf_pager_file(path, NULL, MDF_PAGER_FORMAT_AUTO) == MDF_OK ? 0 : 1);
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
    for (i = 1; i <= 60; i++) {
        int len;

        len = snprintf(line, sizeof(line), "line-%02d\n", i);
        if (len < 0 || write_all(fd, line, (size_t)len) != 0) return -1;
    }
    if (close(fd) != 0 || snprintf(path, cap, "%s.%s", temporary, suffix) >= (int)cap ||
        rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int require_contains(const capture *out, const char *needle)
{
    return out->data != NULL && strstr(out->data, needle) != NULL ? 0 : -1;
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
    capture out;
    pid_t pid;
    int master;
    int rc;
    int i;
    const char *stage;

    if (argc != 2) return 2;
    memset(&out, 0, sizeof(out));
    rc = 1;
    stage = "fixtures";
    if (write_fixture(text_path, sizeof(text_path), "text.txt", 0) != 0 ||
        write_fixture(markdown_path, sizeof(markdown_path), "markdown.md", 1) != 0) goto done;

    stage = "text startup";
    pid = start_pager(argv[1], text_path, 1, &master);
    if (pid < 0 || wait_for_marker(master, &out, "# raw heading") != 0 ||
        wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "# raw heading") != 0 ||
        require_contains(&out, "\033[1;32m ") != 0 ||
        strstr(out.data, "\033[1;32m# raw heading") != NULL) goto done;
    stage = "down";
    if (write_all(master, "j", 1) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-02") != 0) goto done;
    stage = "up";
    if (write_all(master, "k", 1) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-01") != 0) goto done;
    stage = "page down";
    if (write_all(master, "\033[6~", 4) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-10") != 0) goto done;
    stage = "page up";
    if (write_all(master, "\033[5~", 4) != 0 || wait_for_output(master, &out, 80) != 0) goto done;
    stage = "half down";
    if (write_all(master, "\004", 1) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-05") != 0) goto done;
    stage = "half up";
    if (write_all(master, "\025", 1) != 0 || wait_for_output(master, &out, 80) != 0) goto done;
    stage = "end";
    if (write_all(master, "\033[F", 3) != 0 || wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "line-60") != 0) goto done;
    stage = "home";
    if (write_all(master, "\033[H", 3) != 0 || wait_for_output(master, &out, 80) != 0) goto done;
    stage = "escape";
    if (write_all(master, "\033", 1) != 0 || wait_for_exit(pid) != 0 ||
        wait_for_output(master, &out, 80) != 0 ||
        require_contains(&out, "\033[?1049l") != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown direct";
    pid = start_pager(argv[1], markdown_path, 1, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "markdown cmdf";
    pid = start_pager(argv[1], markdown_path, 0, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        require_contains(&out, "\033[1;32m# ") != 0 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    clear_capture(&out);

    stage = "resize";
    pid = start_pager(argv[1], markdown_path, 1, &master);
    if (pid < 0 || wait_for_marker(master, &out, "rendered heading") != 0 ||
        wait_for_output(master, &out, 100) != 0) goto done;
    clear_capture(&out);
    for (i = 0; i < 3; i++) {
        if (kill(pid, SIGWINCH) != 0) goto done;
        sleep_ms(50);
    }
    stage = "resize premature redraw";
    if (wait_for_output(master, &out, 50) != 0 || out.len != 0) goto done;
    stage = "resize settled redraw";
    if (wait_for_output(master, &out, 300) != 0 ||
        count_occurrences(out.data, "\033[H\033[2J") != 1 ||
        write_all(master, "q", 1) != 0 || wait_for_exit(pid) != 0) goto done;
    close(master);
    rc = 0;

done:
    if (rc != 0) fprintf(stderr, "test_pager failed at %s\n", stage);
    unlink(text_path);
    unlink(markdown_path);
    free(out.data);
    return rc;
}
