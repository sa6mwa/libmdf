#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char source[] =
    "# The Outcome-Based Agile Framework\n\n"
    "alpha beta gamma delta alpha beta gamma delta alpha beta gamma delta "
    "alpha beta gamma delta alpha beta gamma delta alpha beta gamma delta "
    "alpha beta gamma delta\n\n[Link](https://example.org)\n";
static const char *executable;
static const char *program;
static char scratch[4096];
static int lua_cli;
static int last_status;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s (diagnostics in %s/stderr)\n", message, scratch);
        exit(1);
    }
}

static char *read_output(int fd)
{
    char *out;
    char chunk[4096];
    ssize_t n;
    size_t len;
    size_t i;

    out = (char *)malloc(1);
    check(out != NULL, "capture allocation");
    len = 0;
    while ((n = read(fd, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            check(errno == EIO, "capture read");
            break;
        }
        out = (char *)realloc(out, len + (size_t)n + 1);
        check(out != NULL, "capture growth");
        for (i = 0; i < (size_t)n; i++) {
            if (chunk[i] != '\r') out[len++] = chunk[i];
        }
    }
    out[len] = '\0';
    return out;
}

static char *run(const char *const *args, int terminal, int want_success)
{
    char *argv[32];
    char error_path[4096];
    char *slave_name;
    int input[2];
    int output[2];
    int master;
    int slave;
    int errors;
    int status;
    size_t i;
    size_t count;
    ssize_t n;
    pid_t child;
    char *out;

    count = 0;
    argv[count++] = (char *)executable;
    if (program != NULL) argv[count++] = (char *)program;
    for (i = 0; args[i] != NULL; i++) {
        check(count + 1 < sizeof(argv) / sizeof(argv[0]), "argument capacity");
        argv[count++] = (char *)args[i];
    }
    argv[count] = NULL;
    check(pipe(input) == 0, "input pipe");
    master = slave = -1;
    if (terminal) {
        struct winsize size;
        master = posix_openpt(O_RDWR | O_NOCTTY);
        check(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "terminal allocation");
        slave_name = ptsname(master);
        check(slave_name != NULL, "terminal name");
        slave = open(slave_name, O_RDWR | O_NOCTTY);
        check(slave >= 0, "terminal open");
        memset(&size, 0, sizeof(size));
        size.ws_row = 24;
        size.ws_col = 100;
        check(ioctl(slave, TIOCSWINSZ, &size) == 0, "terminal geometry");
    } else check(pipe(output) == 0, "output pipe");
    check(snprintf(error_path, sizeof(error_path), "%s/stderr", scratch) < (int)sizeof(error_path), "error path");
    errors = open(error_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(errors >= 0, "diagnostic file");
    child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        if (dup2(input[0], STDIN_FILENO) < 0 ||
            dup2(terminal ? slave : output[1], STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0) _exit(126);
        close(input[0]); close(input[1]); close(errors);
        if (terminal) { close(slave); close(master); }
        else { close(output[0]); close(output[1]); }
        execv(executable, argv);
        _exit(127);
    }
    close(input[0]); close(errors);
    if (terminal) close(slave);
    else close(output[1]);
    n = write(input[1], source, strlen(source));
    check(n == (ssize_t)strlen(source) || (!want_success && n < 0 && errno == EPIPE), "fixture input");
    close(input[1]);
    out = read_output(terminal ? master : output[0]);
    close(terminal ? master : output[0]);
    check(waitpid(child, &status, 0) == child && WIFEXITED(status), "child completion");
    last_status = WEXITSTATUS(status);
    check(want_success ? WEXITSTATUS(status) == 0 : WEXITSTATUS(status) != 0, "command exit status");
    return out;
}

static size_t max_line(const char *out)
{
    size_t longest;
    size_t len;
    longest = len = 0;
    while (*out != '\0') {
        if (*out++ == '\n') { if (len > longest) longest = len; len = 0; }
        else len++;
    }
    return len > longest ? len : longest;
}

static void cli_contract(void)
{
    const char *none[] = {NULL};
    const char *ascii[] = {"--ascii", "--osc8", "on", NULL};
    const char *fragment[] = {"--incremental", "--simulate-chunk", "1", "--osc8", "on", NULL};
    const char *boring[] = {"--ascii", "--boring", "--osc8", "on", NULL};
    const char *on[] = {"--ansi", "on", "--osc8", "on", NULL};
    const char *on_boring[] = {"--ansi", "on", "--boring", "--osc8", "on", NULL};
    const char *stdout_args[] = {"--output", "/dev/stdout", NULL};
    const char *width[] = {"--width", "30", NULL};
    const char *invalid[] = {"--ansi", "invalid", NULL};
    const char *mode_args[] = {"--html", "--html-disable-embedded-font", NULL, NULL};
    char named[4096];
    const char *file_args[] = {"--output", NULL, NULL, NULL, NULL, NULL, NULL};
    char *plain;
    char *actual;
    char *tty;
    char *saved;
    int fd;
    int mode;

    file_args[1] = named;
    plain = run(none, 0, 1);
    check(strchr(plain, '\033') == NULL && max_line(plain) <= 80, "pipe defaults");
    actual = run(ascii, 0, 1); check(strcmp(plain, actual) == 0, "ASCII overrides OSC8"); free(actual);
    if (lua_cli) fragment[0] = "--simulate-chunk", fragment[1] = "1", fragment[2] = "--osc8", fragment[3] = "on", fragment[4] = NULL;
    actual = run(fragment, 0, 1); check(strcmp(plain, actual) == 0, "fragmented pipe parity"); free(actual);
    actual = run(boring, 0, 1); check(strchr(actual, '\033') == NULL, "plain boring policy"); free(actual);
    actual = run(on, 0, 1); check(strchr(actual, '\033') != NULL, "explicit ANSI"); free(actual);
    actual = run(on_boring, 0, 1); check(strstr(actual, "\033]8;;") != NULL, "boring OSC8 policy"); free(actual);
    tty = run(none, 1, 1); check(strchr(tty, '\033') != NULL, "terminal defaults");
    actual = run(stdout_args, 1, 1); check(strcmp(tty, actual) == 0, "stdout alias detection"); free(actual); free(tty);
    actual = run(ascii, 1, 1); check(strchr(actual, '\033') == NULL && max_line(actual) > 80 && max_line(actual) <= 100, "terminal plain width"); free(actual);
    actual = run(width, 0, 1); check(max_line(actual) <= 30, "explicit width"); free(actual);
    check(snprintf(named, sizeof(named), "%s/output", scratch) < (int)sizeof(named), "named output path");
    actual = run(file_args, 1, 1); free(actual);
    fd = open(named, O_RDONLY); check(fd >= 0, "named output open"); saved = read_output(fd); close(fd);
    check(strcmp(saved, plain) == 0, "named output uses its own destination"); free(saved);
    file_args[2] = "--width"; file_args[3] = "30";
    actual = run(file_args, 0, 1); free(actual);
    fd = open(named, O_RDONLY); saved = read_output(fd); close(fd); check(max_line(saved) <= 30, "named explicit width"); free(saved);
    file_args[2] = "--ansi"; file_args[3] = "on";
    actual = run(file_args, 0, 1); free(actual);
    fd = open(named, O_RDONLY); saved = read_output(fd); close(fd); check(strchr(saved, '\033') != NULL, "named explicit ANSI"); free(saved);
    for (mode = 0; mode < 2; mode++) {
        fd = open(named, O_WRONLY | O_TRUNC); check(fd >= 0 && write(fd, "preserve", 8) == 8, "preservation fixture"); close(fd);
        file_args[2] = mode == 0 ? "--width" : "--theme";
        file_args[3] = mode == 0 ? "10" : "missing-theme";
        file_args[4] = mode == 0 ? "--margin-left" : NULL;
        file_args[5] = mode == 0 ? "10" : NULL;
        actual = run(file_args, 0, 0); free(actual);
        fd = open(named, O_RDONLY); saved = read_output(fd); close(fd);
        check(strcmp(saved, "preserve") == 0, "invalid options preserve existing file"); free(saved);
    }
    for (mode = 0; mode < 2; mode++) {
        mode_args[0] = mode == 0 ? "--html" : "--deck";
        mode_args[2] = NULL; saved = run(mode_args, 0, 1);
        mode_args[2] = "--ascii"; actual = run(mode_args, 0, 1);
        check(strcmp(saved, actual) == 0, "HTML and deck ignore ANSI policy"); free(saved); free(actual);
    }
    actual = run(invalid, 0, 0); free(actual);
    if (!lua_cli) check(last_status == 2, "invalid ANSI policy is a native usage error");
    check(snprintf(named, sizeof(named), "%s/stderr", scratch) < (int)sizeof(named), "usage diagnostic path");
    fd = open(named, O_RDONLY); check(fd >= 0, "usage diagnostic open");
    saved = read_output(fd); close(fd);
    check(strstr(saved, "auto|on|off") != NULL, "invalid ANSI policy lists valid choices"); free(saved);
    check(snprintf(named, sizeof(named), "%s/output", scratch) < (int)sizeof(named), "output cleanup path");
    unlink(named);
    free(plain);
}

static void lua_contract(void)
{
    static const char *const workflows[] = {"render", "stream", "document", "handle_render", "handle_stream", "feed", "tokens"};
    const char *args[] = {NULL, "auto", "known", "0", NULL};
    char *plain;
    char *actual;
    size_t i;

    for (i = 0; i < sizeof(workflows) / sizeof(workflows[0]); i++) {
        args[0] = workflows[i]; args[1] = "auto"; args[2] = "known"; args[3] = "0";
        plain = run(args, 0, 1);
        check(strchr(plain, '\033') == NULL && strstr(plain, "µ界") != NULL && max_line(plain) > 60 && max_line(plain) <= 80, "Lua pipe defaults and UTF-8");
        args[2] = "unknown"; actual = run(args, 1, 1); check(strcmp(plain, actual) == 0, "unknown Lua sink remains plain"); free(actual);
        args[2] = "known"; args[1] = "off"; actual = run(args, 0, 1); check(strcmp(plain, actual) == 0, "explicit Lua plain policy"); free(actual);
        args[1] = "on"; actual = run(args, 0, 1); check(strchr(actual, '\033') != NULL, "explicit Lua ANSI"); free(actual);
        args[1] = "auto"; actual = run(args, 1, 1); check(strchr(actual, '\033') != NULL, "Lua terminal defaults"); free(actual);
        args[1] = "off"; actual = run(args, 1, 1); check(strchr(actual, '\033') == NULL && max_line(actual) > 80 && max_line(actual) <= 100, "Lua terminal plain width"); free(actual);
        args[3] = "30"; actual = run(args, 1, 1); check(max_line(actual) <= 30, "Lua explicit terminal width"); free(actual); free(plain);
    }
}

int main(int argc, char **argv)
{
    char errors[4096];
    if (argc < 3 || argc > 4) { fprintf(stderr, "usage: test_destination_policy <executable> <scratch-root> [--lua-cli|Lua-program]\n"); return 2; }
    executable = argv[1];
    check(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "fixture pipe signal");
    if (argc == 4) {
        if (strcmp(argv[3], "--lua-cli") == 0) lua_cli = 1;
        else program = argv[3];
    }
    check(snprintf(scratch, sizeof(scratch), "%s/destination-policy.XXXXXX", argv[2]) < (int)sizeof(scratch), "scratch path");
    check(mkdtemp(scratch) != NULL, "scratch directory");
    check(setenv("COLUMNS", "200", 1) == 0 && setenv("TERM_PROGRAM", "WezTerm", 1) == 0, "test environment");
    if (program == NULL) cli_contract(); else lua_contract();
    check(snprintf(errors, sizeof(errors), "%s/stderr", scratch) < (int)sizeof(errors), "diagnostics path");
    unlink(errors); rmdir(scratch);
    return 0;
}
