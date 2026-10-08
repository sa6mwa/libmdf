#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long integer(const char *text, long minimum)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value < minimum || value > 1000000) {
        fprintf(stderr, "invalid benchmark count: %s\n", text);
        exit(2);
    }
    return value;
}

static double number(const char *text)
{
    char *end;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' || !(value >= 0 && value <= DBL_MAX)) {
        fprintf(stderr, "invalid benchmark value: %s\n", text);
        exit(2);
    }
    return value;
}

static double now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) { perror("clock_gettime"); exit(1); }
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static double sample(char **command)
{
    double started;
    pid_t child;
    int status;
    int output;
    started = now();
    child = fork();
    if (child < 0) { perror("fork"); exit(1); }
    if (child == 0) {
        output = open("/dev/null", O_WRONLY);
        if (output < 0 || dup2(output, STDOUT_FILENO) < 0) _exit(126);
        close(output);
        execv(command[0], command);
        perror(command[0]);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) { perror("waitpid"); exit(1); } }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "benchmark command failed: %s\n", command[0]);
        exit(1);
    }
    return now() - started;
}

static int compare(const void *a, const void *b)
{
    double left;
    double right;
    left = *(const double *)a;
    right = *(const double *)b;
    return left < right ? -1 : left > right ? 1 : 0;
}

static void json_string(const char *s)
{
    putchar('"');
    while (*s != '\0') {
        unsigned char c;
        c = (unsigned char)*s++;
        if (c == '"' || c == '\\') putchar('\\');
        if (c < 32) printf("\\u%04x", (unsigned int)c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    long rounds;
    long warmups;
    long repeats;
    long i;
    double *samples;
    double total;
    double median;
    const char *label;
    const char *mode;
    int pos;
    int json;

    if (argc == 5 && strcmp(argv[1], "--compare") == 0) {
        double observed;
        double reference;
        double allowance;
        observed = number(argv[2]); reference = number(argv[3]); allowance = number(argv[4]);
        if (reference <= 0) { fprintf(stderr, "benchmark reference must be positive\n"); return 2; }
        printf("observed %.9f; reference %.9f; limit %.9f\n", observed, reference, reference * (1 + allowance));
        return observed <= reference * (1 + allowance) ? 0 : 1;
    }
    rounds = 10; warmups = 2; repeats = 1;
    label = "command"; mode = "default"; json = 0;
    for (pos = 1; pos < argc && strcmp(argv[pos], "--") != 0; pos++) {
        if (strcmp(argv[pos], "--json") == 0) { json = 1; continue; }
        if (pos + 1 >= argc) { fprintf(stderr, "missing benchmark option value\n"); return 2; }
        if (strcmp(argv[pos], "--rounds") == 0) rounds = integer(argv[++pos], 1);
        else if (strcmp(argv[pos], "--warmups") == 0) warmups = integer(argv[++pos], 0);
        else if (strcmp(argv[pos], "--repeat") == 0) repeats = integer(argv[++pos], 1);
        else if (strcmp(argv[pos], "--label") == 0) label = argv[++pos];
        else if (strcmp(argv[pos], "--mode") == 0) mode = argv[++pos];
        else { fprintf(stderr, "unknown benchmark option: %s\n", argv[pos]); return 2; }
    }
    if (++pos >= argc) { fprintf(stderr, "usage: benchmark_command [options] -- <executable> [args...]\n"); return 2; }
    samples = (double *)malloc((size_t)rounds * sizeof(*samples));
    if (samples == NULL) { perror("malloc"); return 1; }
    for (i = 0; i < warmups; i++) (void)sample(argv + pos);
    total = 0;
    for (i = 0; i < rounds; i++) { samples[i] = sample(argv + pos) / (double)repeats; total += samples[i]; }
    qsort(samples, (size_t)rounds, sizeof(*samples), compare);
    median = rounds % 2 != 0 ? samples[rounds / 2] : (samples[rounds / 2 - 1] + samples[rounds / 2]) / 2;
    if (json) {
        printf("{\"impl\":"); json_string(label); printf(",\"mode\":"); json_string(mode);
        printf(",\"rounds\":%ld,\"stats\":{\"median\":%.9f,\"mean\":%.9f,\"min\":%.9f,\"max\":%.9f}}\n",
               rounds, median, total / (double)rounds, samples[0], samples[rounds - 1]);
    } else printf("| %s | %s | %.6f | %.6f | %.6f | %.6f | %ld |\n", label, mode, median, total / (double)rounds, samples[0], samples[rounds - 1], rounds);
    free(samples);
    return 0;
}
