#include "mdf_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MDF_PAGER_RESIZE_DEBOUNCE_MS 250L

typedef struct mdf_pager_buffer {
    char *data;
    size_t len;
    size_t cap;
} mdf_pager_buffer;

typedef struct mdf_pager_view {
    mdf_pager_buffer bytes;
    size_t *lines;
    size_t line_count;
    size_t line_cap;
} mdf_pager_view;

typedef enum mdf_pager_key {
    MDF_PAGER_KEY_NONE,
    MDF_PAGER_KEY_QUIT,
    MDF_PAGER_KEY_UP,
    MDF_PAGER_KEY_DOWN,
    MDF_PAGER_KEY_PAGE_UP,
    MDF_PAGER_KEY_PAGE_DOWN,
    MDF_PAGER_KEY_HOME,
    MDF_PAGER_KEY_END,
    MDF_PAGER_KEY_HALF_UP,
    MDF_PAGER_KEY_HALF_DOWN
} mdf_pager_key;

static volatile sig_atomic_t mdf_pager_winch_count;

static void mdf_pager_on_winch(int signal_number)
{
    (void)signal_number;
    mdf_pager_winch_count++;
}

static long mdf_pager_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static void mdf_pager_buffer_destroy(mdf_pager_buffer *buffer)
{
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int mdf_pager_buffer_append(mdf_pager_buffer *buffer, const char *src, size_t len)
{
    size_t required;
    size_t next_cap;
    char *next;

    if (len > (size_t)-1 - buffer->len - 1) {
        return -1;
    }
    required = buffer->len + len + 1;
    if (required > buffer->cap) {
        next_cap = buffer->cap == 0 ? 1024 : buffer->cap;
        while (next_cap < required) {
            if (next_cap > (size_t)-1 / 2) {
                next_cap = required;
                break;
            }
            next_cap *= 2;
        }
        next = (char *)realloc(buffer->data, next_cap);
        if (next == NULL) {
            return -1;
        }
        buffer->data = next;
        buffer->cap = next_cap;
    }
    if (len > 0) {
        memcpy(buffer->data + buffer->len, src, len);
        buffer->len += len;
    }
    buffer->data[buffer->len] = '\0';
    return 0;
}

static int mdf_pager_buffer_append_byte(mdf_pager_buffer *buffer, unsigned char byte)
{
    char c;

    c = (char)byte;
    return mdf_pager_buffer_append(buffer, &c, 1);
}

static int mdf_pager_read_file(const char *path, mdf_pager_buffer *out)
{
    FILE *fp;
    char chunk[4096];
    size_t n;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (mdf_pager_buffer_append(out, chunk, n) != 0) {
            fclose(fp);
            return -1;
        }
    }
    if (ferror(fp) || fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int mdf_pager_is_markdown_path(const char *path)
{
    const char *dot;

    dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0' || dot[2] == '\0' || dot[3] != '\0') {
        return 0;
    }
    return (dot[1] == 'm' || dot[1] == 'M') && (dot[2] == 'd' || dot[2] == 'D');
}

static int mdf_pager_append_escape(mdf_pager_buffer *out, const char *src, size_t len, size_t *index)
{
    size_t i;

    i = *index;
    if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
        return -1;
    }
    i++;
    if (i < len && src[i] == '[') {
        if (mdf_pager_buffer_append_byte(out, (unsigned char)src[i]) != 0) {
            return -1;
        }
        i++;
        while (i < len) {
            unsigned char byte;

            byte = (unsigned char)src[i];
            if (mdf_pager_buffer_append_byte(out, byte) != 0) {
                return -1;
            }
            i++;
            if (byte >= 0x40 && byte <= 0x7e) {
                break;
            }
        }
    }
    *index = i;
    return 0;
}

static int mdf_pager_append_text_byte(mdf_pager_buffer *out, unsigned char byte)
{
    if (byte < 32 || byte == 127) {
        if (mdf_pager_buffer_append_byte(out, '^') != 0 ||
            mdf_pager_buffer_append_byte(out, byte == 127 ? '?' : (unsigned char)(byte + 64)) != 0) {
            return -1;
        }
        return 0;
    }
    return mdf_pager_buffer_append_byte(out, byte);
}

static int mdf_pager_reflow(mdf_pager_view *view, const char *src, size_t len, int width, int styled)
{
    size_t i;
    int col;

    if (width < 1) {
        width = 1;
    }
    i = 0;
    col = 0;
    while (i < len) {
        unsigned char byte;
        size_t unit_len;

        byte = (unsigned char)src[i];
        if (byte == '\r') {
            i++;
            continue;
        }
        if (byte == '\n') {
            if (mdf_pager_buffer_append_byte(&view->bytes, byte) != 0) return -1;
            col = 0;
            i++;
            continue;
        }
        if (styled && byte == 0x1b) {
            if (mdf_pager_append_escape(&view->bytes, src, len, &i) != 0) return -1;
            continue;
        }
        if (byte == '\t') {
            int spaces;

            spaces = 8 - (col % 8);
            while (spaces-- > 0) {
                if (col == width) {
                    if (mdf_pager_buffer_append_byte(&view->bytes, '\n') != 0) return -1;
                    col = 0;
                }
                if (mdf_pager_buffer_append_byte(&view->bytes, ' ') != 0) return -1;
                col++;
            }
            i++;
            continue;
        }
        if (col == width) {
            if (mdf_pager_buffer_append_byte(&view->bytes, '\n') != 0) return -1;
            col = 0;
        }
        unit_len = 1;
        if (byte >= 0xc2 && byte <= 0xf4) {
            size_t expected;

            expected = byte < 0xe0 ? 2 : (byte < 0xf0 ? 3 : 4);
            while (unit_len < expected && i + unit_len < len &&
                   ((unsigned char)src[i + unit_len] & 0xc0) == 0x80) {
                unit_len++;
            }
        }
        if (styled) {
            if (mdf_pager_buffer_append(&view->bytes, src + i, unit_len) != 0) return -1;
        } else {
            size_t j;

            for (j = 0; j < unit_len; j++) {
                if (mdf_pager_append_text_byte(&view->bytes, (unsigned char)src[i + j]) != 0) return -1;
            }
        }
        col++;
        i += unit_len;
    }
    return 0;
}

static void mdf_pager_view_destroy(mdf_pager_view *view)
{
    mdf_pager_buffer_destroy(&view->bytes);
    free(view->lines);
    memset(view, 0, sizeof(*view));
}

static int mdf_pager_view_add_line(mdf_pager_view *view, size_t offset)
{
    size_t next_cap;
    size_t *next;

    if (view->line_count == view->line_cap) {
        next_cap = view->line_cap == 0 ? 128 : view->line_cap * 2;
        if (next_cap < view->line_cap || next_cap > (size_t)-1 / sizeof(*next)) {
            return -1;
        }
        next = (size_t *)realloc(view->lines, next_cap * sizeof(*next));
        if (next == NULL) {
            return -1;
        }
        view->lines = next;
        view->line_cap = next_cap;
    }
    view->lines[view->line_count++] = offset;
    return 0;
}

static int mdf_pager_view_index(mdf_pager_view *view)
{
    size_t i;

    if (mdf_pager_view_add_line(view, 0) != 0) {
        return -1;
    }
    for (i = 0; i < view->bytes.len; i++) {
        if (view->bytes.data[i] == '\n' && i + 1 < view->bytes.len &&
            mdf_pager_view_add_line(view, i + 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mdf_pager_window_size(int *columns, int *rows)
{
    struct winsize size;

    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        *columns = 80;
        *rows = 24;
    } else {
        *columns = size.ws_col == 0 ? 80 : (int)size.ws_col;
        *rows = size.ws_row == 0 ? 24 : (int)size.ws_row;
    }
    if (*columns < 1) *columns = 1;
    if (*rows < 2) *rows = 2;
    return 0;
}

static int mdf_pager_write(const char *src, size_t len)
{
    size_t off;

    off = 0;
    while (off < len) {
        ssize_t n;

        n = write(STDOUT_FILENO, src + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int mdf_pager_draw(const mdf_pager_view *view, const char *path, const mdf_options *opts,
                          int columns, int rows, size_t top)
{
    const mdf_theme_style *theme;
    size_t content_rows;
    size_t row;
    size_t max_top;
    int percent;
    char status[512];
    char suffix[32];
    int status_len;
    int suffix_len;
    int path_len;

    content_rows = (size_t)(rows - 1);
    max_top = view->line_count > content_rows ? view->line_count - content_rows : 0;
    percent = max_top == 0 || top >= max_top ? 100 : (int)((top * 100) / max_top);
    if (mdf_pager_write("\033[H\033[2J", strlen("\033[H\033[2J")) != 0) return -1;
    for (row = 0; row < content_rows; row++) {
        size_t index;
        size_t start;
        size_t end;

        if (mdf_pager_write("\033[K", strlen("\033[K")) != 0) return -1;
        index = top + row;
        if (index < view->line_count) {
            start = view->lines[index];
            end = index + 1 < view->line_count ? view->lines[index + 1] : view->bytes.len;
            if (end > start && view->bytes.data[end - 1] == '\n') end--;
            if (end > start && mdf_pager_write(view->bytes.data + start, end - start) != 0) return -1;
        }
        if (mdf_pager_write("\033[0m\r\n", strlen("\033[0m\r\n")) != 0) return -1;
    }
    suffix_len = snprintf(suffix, sizeof(suffix), "  %d%%", percent);
    path_len = columns - suffix_len - 1;
    if (path_len < 0) path_len = 0;
    status_len = snprintf(status, sizeof(status), " %.*s  %d%%", path_len, path, percent);
    if (status_len < 0) return -1;
    if (status_len >= (int)sizeof(status)) status_len = (int)sizeof(status) - 1;
    theme = mdf_theme_resolve(opts == NULL ? NULL : opts->theme_name);
    if (theme == NULL) theme = mdf_theme_resolve("default");
    if (opts == NULL || !opts->boring) {
        if (mdf_pager_write(theme->heading[0], strlen(theme->heading[0])) != 0) return -1;
    }
    if (mdf_pager_write(status, (size_t)status_len) != 0 ||
        mdf_pager_write("\033[K\033[0m", strlen("\033[K\033[0m")) != 0) return -1;
    return 0;
}

static int mdf_pager_set_raw_mode(struct termios *saved)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, saved) != 0) return -1;
    raw = *saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static int mdf_pager_read_with_timeout(unsigned char *out, long milliseconds)
{
    fd_set reads;
    struct timeval timeout;
    int ready;
    ssize_t n;

    FD_ZERO(&reads);
    FD_SET(STDIN_FILENO, &reads);
    timeout.tv_sec = milliseconds / 1000L;
    timeout.tv_usec = (milliseconds % 1000L) * 1000L;
    ready = select(STDIN_FILENO + 1, &reads, NULL, NULL, &timeout);
    if (ready <= 0) return ready;
    n = read(STDIN_FILENO, out, 1);
    if (n == 1) return 1;
    return n == 0 ? 0 : -1;
}

static mdf_pager_key mdf_pager_read_key(unsigned char byte)
{
    unsigned char sequence[8];
    size_t len;
    int result;

    if (byte == 'q') return MDF_PAGER_KEY_QUIT;
    if (byte == 'j' || byte == 14) return MDF_PAGER_KEY_DOWN;
    if (byte == 'k' || byte == 16) return MDF_PAGER_KEY_UP;
    if (byte == 4) return MDF_PAGER_KEY_HALF_DOWN;
    if (byte == 21) return MDF_PAGER_KEY_HALF_UP;
    if (byte != 0x1b) return MDF_PAGER_KEY_NONE;
    result = mdf_pager_read_with_timeout(&byte, 25);
    if (result != 1) return MDF_PAGER_KEY_QUIT;
    if (byte != '[' && byte != 'O') return MDF_PAGER_KEY_NONE;
    len = 0;
    while (len < sizeof(sequence)) {
        result = mdf_pager_read_with_timeout(&byte, 10);
        if (result != 1) break;
        sequence[len++] = byte;
        if ((byte >= 'A' && byte <= 'Z') || byte == '~') break;
    }
    if (len == 0) return MDF_PAGER_KEY_NONE;
    if (sequence[len - 1] == 'A') return MDF_PAGER_KEY_UP;
    if (sequence[len - 1] == 'B') return MDF_PAGER_KEY_DOWN;
    if (sequence[len - 1] == 'H') return MDF_PAGER_KEY_HOME;
    if (sequence[len - 1] == 'F') return MDF_PAGER_KEY_END;
    if (sequence[len - 1] == '~' && len >= 2) {
        if (sequence[0] == '5') return MDF_PAGER_KEY_PAGE_UP;
        if (sequence[0] == '6') return MDF_PAGER_KEY_PAGE_DOWN;
        if (sequence[0] == '1' || sequence[0] == '7') return MDF_PAGER_KEY_HOME;
        if (sequence[0] == '4' || sequence[0] == '8') return MDF_PAGER_KEY_END;
    }
    return MDF_PAGER_KEY_NONE;
}

static mdf_status mdf_pager_make_view(mdf_pager_view *view, const mdf_pager_buffer *input,
                                      const mdf_options *opts, int markdown, int width)
{
    char *rendered;
    mdf *renderer;
    mdf_options render_opts;
    mdf_status st;

    mdf_pager_view_destroy(view);
    if (!markdown) {
        if (mdf_pager_reflow(view, input->data == NULL ? "" : input->data, input->len, width, 0) != 0 ||
            mdf_pager_view_index(view) != 0) {
            return MDF_ERROR_NOMEM;
        }
        return MDF_OK;
    }
    mdf_options_init(&render_opts);
    if (opts != NULL) render_opts = *opts;
    render_opts.width = width;
    renderer = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &render_opts, &renderer);
    if (st != MDF_OK) return st;
    rendered = NULL;
    st = renderer->render_cstr(renderer, input->data == NULL ? "" : input->data, &rendered);
    if (st == MDF_OK &&
        (mdf_pager_reflow(view, rendered == NULL ? "" : rendered,
                          rendered == NULL ? 0 : strlen(rendered), width, 1) != 0 ||
         mdf_pager_view_index(view) != 0)) {
        st = MDF_ERROR_NOMEM;
    }
    renderer->string_free(renderer, rendered);
    renderer->destroy(renderer);
    return st;
}

mdf_status mdf_pager_file(const char *path, const mdf_options *render_options, mdf_pager_format format)
{
    struct stat st;
    struct termios saved_termios;
    struct sigaction old_winch;
    struct sigaction winch_action;
    mdf_pager_buffer input;
    mdf_pager_view view;
    mdf_options opts;
    int markdown;
    int columns;
    int rows;
    size_t top;
    sig_atomic_t seen_winch;
    long resize_deadline;
    int entered_terminal;
    int handler_installed;
    int redraw;
    mdf_status result;

    if (path == NULL || path[0] == '\0' ||
        (format != MDF_PAGER_FORMAT_AUTO && format != MDF_PAGER_FORMAT_TEXT &&
         format != MDF_PAGER_FORMAT_MARKDOWN) ||
        stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return MDF_ERROR_INVALID;
    }
    memset(&input, 0, sizeof(input));
    memset(&view, 0, sizeof(view));
    if (mdf_pager_read_file(path, &input) != 0) {
        mdf_pager_buffer_destroy(&input);
        return MDF_ERROR_IO;
    }
    mdf_options_init(&opts);
    if (render_options != NULL) opts = *render_options;
    markdown = format == MDF_PAGER_FORMAT_MARKDOWN ||
        (format == MDF_PAGER_FORMAT_AUTO && mdf_pager_is_markdown_path(path));
    if (mdf_pager_set_raw_mode(&saved_termios) != 0) {
        mdf_pager_buffer_destroy(&input);
        return MDF_ERROR_IO;
    }
    entered_terminal = 0;
    handler_installed = 0;
    memset(&winch_action, 0, sizeof(winch_action));
    winch_action.sa_handler = mdf_pager_on_winch;
    sigemptyset(&winch_action.sa_mask);
    if (sigaction(SIGWINCH, &winch_action, &old_winch) != 0) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        mdf_pager_buffer_destroy(&input);
        return MDF_ERROR_IO;
    }
    handler_installed = 1;
    if (mdf_pager_write("\033[?1049h\033[?25l", strlen("\033[?1049h\033[?25l")) != 0) {
        result = MDF_ERROR_IO;
        goto done;
    }
    entered_terminal = 1;
    mdf_pager_window_size(&columns, &rows);
    result = mdf_pager_make_view(&view, &input, &opts, markdown, columns);
    if (result != MDF_OK) goto done;
    top = 0;
    seen_winch = mdf_pager_winch_count;
    resize_deadline = -1;
    redraw = 1;
    for (;;) {
        mdf_pager_key key;
        size_t content_rows;
        size_t max_top;
        long wait_ms;
        unsigned char ready_byte;
        int ready;

        if (redraw) {
            if (mdf_pager_draw(&view, path, &opts, columns, rows, top) != 0) {
                result = MDF_ERROR_IO;
                goto done;
            }
            redraw = 0;
        }
        if (mdf_pager_winch_count != seen_winch) {
            long now;

            seen_winch = mdf_pager_winch_count;
            now = mdf_pager_now_ms();
            resize_deadline = now < 0 ? 0 : now + MDF_PAGER_RESIZE_DEBOUNCE_MS;
        }
        content_rows = (size_t)(rows - 1);
        max_top = view.line_count > content_rows ? view.line_count - content_rows : 0;
        wait_ms = 1000;
        if (resize_deadline >= 0) {
            long now;

            now = mdf_pager_now_ms();
            wait_ms = now < 0 || now >= resize_deadline ? 0 : resize_deadline - now;
        }
        ready = mdf_pager_read_with_timeout(&ready_byte, wait_ms);
        if (mdf_pager_winch_count != seen_winch) {
            long now;

            seen_winch = mdf_pager_winch_count;
            now = mdf_pager_now_ms();
            resize_deadline = now < 0 ? 0 : now + MDF_PAGER_RESIZE_DEBOUNCE_MS;
            continue;
        }
        if (resize_deadline >= 0 && ready == 0) {
            mdf_pager_window_size(&columns, &rows);
            result = mdf_pager_make_view(&view, &input, &opts, markdown, columns);
            if (result != MDF_OK) goto done;
            if (top > view.line_count) top = view.line_count;
            resize_deadline = -1;
            redraw = 1;
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            result = MDF_ERROR_IO;
            goto done;
        }
        if (ready == 0) continue;
        key = mdf_pager_read_key(ready_byte);
        if (key == MDF_PAGER_KEY_QUIT) break;
        if (key == MDF_PAGER_KEY_DOWN && top < max_top) {
            top++;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_UP && top > 0) {
            top--;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_PAGE_DOWN) {
            top = top + content_rows > max_top ? max_top : top + content_rows;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_PAGE_UP) {
            top = top > content_rows ? top - content_rows : 0;
            redraw = 1;
        }
        else if (key == MDF_PAGER_KEY_HALF_DOWN) {
            size_t amount;

            amount = content_rows / 2;
            if (amount == 0) amount = 1;
            top = top + amount > max_top ? max_top : top + amount;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_HALF_UP) {
            size_t amount;

            amount = content_rows / 2;
            if (amount == 0) amount = 1;
            top = top > amount ? top - amount : 0;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_HOME) {
            top = 0;
            redraw = 1;
        } else if (key == MDF_PAGER_KEY_END) {
            top = max_top;
            redraw = 1;
        }
    }
    result = MDF_OK;

done:
    if (entered_terminal) {
        (void)mdf_pager_write("\033[0m\033[?25h\033[?1049l", strlen("\033[0m\033[?25h\033[?1049l"));
    }
    if (handler_installed) (void)sigaction(SIGWINCH, &old_winch, NULL);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    mdf_pager_view_destroy(&view);
    mdf_pager_buffer_destroy(&input);
    return result;
}
