#include "render_internal.h"

#define MDF_MIN_ANSI_CONTENT_WIDTH 3

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mdf_theme_data.h"

#define MDF_MEMORY_DEFAULT_MAX_RETAINED_BYTES (256u * 1024u)
#define MDF_MEMORY_DEFAULT_MAX_REUSABLE_BLOCK_BYTES 8192u

static void *default_alloc(void *userdata, size_t size)
{
    (void)userdata;
    return malloc(size);
}

static void *default_realloc(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    (void)userdata;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void default_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

#define MDF_ZERO_IMPL_SPAN(impl, first, last) \
    memset(&(impl)->first, \
           0, \
           offsetof(mdf_impl, last) + sizeof((impl)->last) - offsetof(mdf_impl, first))

static void mdf_impl_reset_emit_buffer(mdf_impl *impl);

static void mdf_impl_clear_auto_html_title(mdf_impl *impl)
{
    if (impl == NULL || !impl->html_title_auto) {
        return;
    }
    mdf_free_mem(&impl->allocator, impl->html_title, impl->html_title_cap);
    impl->html_title = NULL;
    impl->html_title_cap = 0;
    impl->html_title_auto = 0;
}

static void mdf_impl_reset_render_state(mdf_impl *impl)
{
    mdf_impl_clear_auto_html_title(impl);
    mdf_impl_reset_emit_buffer(impl);
    MDF_ZERO_IMPL_SPAN(impl, in_paragraph, html_footer_needs_newline);
    MDF_ZERO_IMPL_SPAN(impl, quote_open, heading_style_pending_prefix);
    MDF_ZERO_IMPL_SPAN(impl, pending_breaks, ansi_prev_char);
    impl->ansi_word_len = 0;
    impl->ansi_word_cols = 0;
    impl->ansi_flushing_word = 0;
    impl->pre_code_len = 0;
    MDF_ZERO_IMPL_SPAN(impl, pre_code_prefix_state, pre_code_wrap_indent);
    MDF_ZERO_IMPL_SPAN(impl, table_cell_mode, inline_mode);
    impl->inline_text_len = 0;
    MDF_ZERO_IMPL_SPAN(impl, inline_outer_paren_candidate, inline_outer_paren_pending);
    impl->inline_url_len = 0;
    impl->pending_fallback_url_len = 0;
    MDF_ZERO_IMPL_SPAN(impl, pending_fallback_exact, pending_wrapped_link_punct);
    impl->inline_code_len = 0;
    impl->inline_code_delim_len = 0;
    impl->inline_code_pending_ticks = 0;
    impl->ansi_pending_emit_len = 0;
    impl->ansi_pending_emit_offsets_len = 0;
    impl->ansi_pending_emit_start_col = 0;
    impl->ansi_pending_emit_valid = 0;
    impl->ansi_pending_autolink_emit_valid = 0;
    impl->ansi_pending_fallback_emit_valid = 0;
    impl->ansi_owned_inline_style[0] = '\0';
    impl->inline_emph_len = 0;
    MDF_ZERO_IMPL_SPAN(impl, inline_emph_count, inline_emph_after_word);
    impl->inline_emph_streaming = 0;
    impl->inline_emph_skip_spaces = 0;
    impl->inline_emph_nested_delim = 0;
    impl->inline_emph_nested_count = 0;
    impl->inline_emph_nested_close_count = 0;
    impl->inline_emph_nested_saw_space = 0;
    impl->inline_entity_len = 0;
    impl->inline_html_nbsp_pending = 0;
    impl->inline_html_nbsp_prev_digit = 0;
    mdf_render_reset_state(impl);
    impl->error[0] = '\0';
}

void mdf_renderer_reset_session_state(mdf_renderer *self)
{
    if (self == NULL || self->impl == NULL) {
        return;
    }
    mdf_impl_reset_render_state((mdf_impl *)self->impl);
}

void mdf_allocator_normalize(mdf_allocator *allocator)
{
    if (allocator == NULL) {
        return;
    }
    if (allocator->alloc == NULL) {
        allocator->alloc = default_alloc;
    }
    if (allocator->free == NULL) {
        allocator->free = default_free;
    }
    if (allocator->realloc == NULL && allocator->alloc == default_alloc && allocator->free == default_free) {
        allocator->realloc = default_realloc;
    }
}

void *mdf_alloc(mdf_allocator *allocator, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    mdf_allocator_normalize(allocator);
    return allocator->alloc(allocator->userdata, size);
}

void *mdf_realloc_mem(mdf_allocator *allocator, void *ptr, size_t old_size, size_t new_size)
{
    void *next;

    if (new_size == 0) {
        new_size = 1;
    }
    mdf_allocator_normalize(allocator);
    if (allocator->realloc != NULL) {
        return allocator->realloc(allocator->userdata, ptr, old_size, new_size);
    }
    next = allocator->alloc(allocator->userdata, new_size);
    if (next == NULL) {
        return NULL;
    }
    if (ptr != NULL && old_size > 0) {
        memcpy(next, ptr, old_size < new_size ? old_size : new_size);
        allocator->free(allocator->userdata, ptr, old_size);
    }
    return next;
}

void mdf_free_mem(mdf_allocator *allocator, void *ptr, size_t size)
{
    if (ptr == NULL) {
        return;
    }
    mdf_allocator_normalize(allocator);
    allocator->free(allocator->userdata, ptr, size);
}

static void *mdf_memory_alloc_cb(void *userdata, size_t size);
static void *mdf_memory_realloc_cb(void *userdata, void *ptr, size_t old_size, size_t new_size);
static void mdf_memory_free_cb(void *userdata, void *ptr, size_t size);

static size_t mdf_memory_max_retained(const mdf_memory *memory)
{
    return memory->opts.max_retained_bytes == 0 ? MDF_MEMORY_DEFAULT_MAX_RETAINED_BYTES : memory->opts.max_retained_bytes;
}

static size_t mdf_memory_max_reusable_block(const mdf_memory *memory)
{
    return memory->opts.max_reusable_block_bytes == 0 ? MDF_MEMORY_DEFAULT_MAX_REUSABLE_BLOCK_BYTES : memory->opts.max_reusable_block_bytes;
}

void mdf_memory_init(mdf_memory *memory, const mdf_allocator *backing, const mdf_memory_options *opts)
{
    memset(memory, 0, sizeof(*memory));
    if (backing != NULL) {
        memory->backing = *backing;
    }
    mdf_allocator_normalize(&memory->backing);
    if (opts != NULL) {
        memory->opts = *opts;
    }
}

void mdf_memory_destroy(mdf_memory *memory)
{
    mdf_memory_block *block;
    mdf_memory_block *next;

    if (memory == NULL) {
        return;
    }
    block = memory->free_list;
    while (block != NULL) {
        next = block->next;
        mdf_free_mem(&memory->backing, block, sizeof(*block) + block->cap);
        block = next;
    }
    memory->free_list = NULL;
    memory->retained_bytes = 0;
}

mdf_allocator mdf_memory_allocator(mdf_memory *memory)
{
    mdf_allocator allocator;

    allocator.userdata = memory;
    allocator.alloc = mdf_memory_alloc_cb;
    allocator.realloc = mdf_memory_realloc_cb;
    allocator.free = mdf_memory_free_cb;
    return allocator;
}

static void *mdf_memory_alloc_cb(void *userdata, size_t size)
{
    mdf_memory *memory;
    mdf_memory_block **prev;
    mdf_memory_block *block;

    memory = (mdf_memory *)userdata;
    if (size == 0) {
        size = 1;
    }
    prev = &memory->free_list;
    block = memory->free_list;
    while (block != NULL) {
        if (block->cap >= size) {
            *prev = block->next;
            block->next = NULL;
            memory->retained_bytes -= block->cap;
            return block + 1;
        }
        prev = &block->next;
        block = block->next;
    }
    if (size > ((size_t)-1) - sizeof(*block)) {
        return NULL;
    }
    block = (mdf_memory_block *)mdf_alloc(&memory->backing, sizeof(*block) + size);
    if (block == NULL) {
        return NULL;
    }
    block->next = NULL;
    block->cap = size;
    return block + 1;
}

static void *mdf_memory_realloc_cb(void *userdata, void *ptr, size_t old_size, size_t new_size)
{
    mdf_memory_block *block;
    void *next;
    size_t copy;

    (void)old_size;
    if (ptr == NULL) {
        return mdf_memory_alloc_cb(userdata, new_size);
    }
    if (new_size == 0) {
        new_size = 1;
    }
    block = ((mdf_memory_block *)ptr) - 1;
    if (new_size <= block->cap) {
        return ptr;
    }
    next = mdf_memory_alloc_cb(userdata, new_size);
    if (next == NULL) {
        return NULL;
    }
    copy = block->cap < new_size ? block->cap : new_size;
    memcpy(next, ptr, copy);
    mdf_memory_free_cb(userdata, ptr, block->cap);
    return next;
}

static void mdf_memory_free_cb(void *userdata, void *ptr, size_t size)
{
    mdf_memory *memory;
    mdf_memory_block *block;
    size_t max_retained;
    size_t max_block;

    (void)size;
    if (ptr == NULL) {
        return;
    }
    memory = (mdf_memory *)userdata;
    block = ((mdf_memory_block *)ptr) - 1;
    max_retained = mdf_memory_max_retained(memory);
    max_block = mdf_memory_max_reusable_block(memory);
    if (block->cap <= max_block &&
        memory->retained_bytes <= max_retained &&
        block->cap <= max_retained - memory->retained_bytes) {
        block->next = memory->free_list;
        memory->free_list = block;
        memory->retained_bytes += block->cap;
        return;
    }
    mdf_free_mem(&memory->backing, block, sizeof(*block) + block->cap);
}

typedef struct deck_string {
    mdf_allocator *allocator;
    char *buf;
    size_t len;
    size_t cap;
} deck_string;

typedef struct deck_slide {
    char *html;
    size_t html_len;
    size_t html_cap;
} deck_slide;

typedef struct deck_slice {
    const char *src;
    size_t len;
} deck_slice;

typedef struct deck_slice_source {
    const char *src;
    size_t len;
    size_t off;
} deck_slice_source;

typedef struct deck_slide_sink {
    deck_string html;
    int oom;
} deck_slide_sink;

typedef struct deck_source_reader {
    mdf_source *source;
    char buf[4096];
    size_t off;
    size_t len;
    int err;
} deck_source_reader;

typedef struct auto_title_source {
    mdf_source *source;
    deck_string prefix;
    size_t scan_off;
    size_t replay_off;
    int err;
} auto_title_source;

static int deck_string_append(deck_string *s, const char *src, size_t len)
{
    char *next;
    size_t cap;

    if (len == 0) {
        return 0;
    }
    if (s->len + len + 1 < s->len) {
        return -1;
    }
    if (s->len + len + 1 > s->cap) {
        cap = s->cap == 0 ? 1024u : s->cap;
        while (cap < s->len + len + 1) {
            if (cap > ((size_t)-1) / 2) {
                return -1;
            }
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(s->allocator, s->buf, s->cap, cap);
        if (next == NULL) {
            return -1;
        }
        s->buf = next;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, src, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return 0;
}

static void deck_string_dispose(deck_string *s)
{
    if (s == NULL) {
        return;
    }
    mdf_free_mem(s->allocator, s->buf, s->cap);
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

static size_t deck_slice_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    deck_slice_source *src;
    size_t n;

    (void)err;
    src = (deck_slice_source *)userdata;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->len - src->off;
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, src->src + src->off, n);
    src->off += n;
    return n;
}

static int deck_slide_sink_write(void *userdata, const char *src, size_t len)
{
    deck_slide_sink *sink;

    sink = (deck_slide_sink *)userdata;
    if (deck_string_append(&sink->html, src, len) != 0) {
        sink->oom = 1;
        return -1;
    }
    return 0;
}

static size_t auto_title_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    auto_title_source *src;
    size_t n;

    src = (auto_title_source *)userdata;
    if (src->replay_off < src->prefix.len) {
        n = src->prefix.len - src->replay_off;
        if (n > cap) {
            n = cap;
        }
        memcpy(dst, src->prefix.buf + src->replay_off, n);
        src->replay_off += n;
        *err = 0;
        return n;
    }
    return src->source->read(src->source->userdata, dst, cap, err);
}

static int auto_title_read_more(auto_title_source *src)
{
    char buf[256];
    size_t n;
    int err;

    err = 0;
    n = src->source->read(src->source->userdata, buf, sizeof(buf), &err);
    if (err != 0) {
        src->err = err;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (deck_string_append(&src->prefix, buf, n) != 0) {
        src->err = -1;
        return -1;
    }
    return 1;
}

static int auto_title_read_byte(auto_title_source *src, char *ch)
{
    int rc;

    while (src->scan_off >= src->prefix.len) {
        rc = auto_title_read_more(src);
        if (rc <= 0) {
            return rc;
        }
    }
    *ch = src->prefix.buf[src->scan_off++];
    return 1;
}

static int auto_title_copy_trimmed(mdf_allocator *allocator, const char *start, const char *end, char **out, size_t *cap_out)
{
    char *title;
    size_t len;

    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '#')) {
        if (end[-1] == '#') {
            const char *hash;

            hash = end;
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
    title = (char *)mdf_realloc_mem(allocator, NULL, 0, len + 1);
    if (title == NULL) {
        return -1;
    }
    memcpy(title, start, len);
    title[len] = '\0';
    *out = title;
    *cap_out = len + 1;
    return 0;
}

static int auto_title_detect_line(mdf_allocator *allocator,
                                  const char *line_start,
                                  const char *line_end,
                                  char **title_out,
                                  size_t *title_cap_out,
                                  int *decided)
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
            return auto_title_copy_trimmed(allocator, q, line_end, title_out, title_cap_out);
        }
    }
    return 0;
}

static int auto_title_detect_span(mdf_allocator *allocator,
                                  const char *src,
                                  size_t start,
                                  size_t end,
                                  char **title_out,
                                  size_t *title_cap_out)
{
    while (start < end && *title_out == NULL) {
        size_t line_start;
        size_t line_end;
        int decided;

        line_start = start;
        while (start < end && src[start] != '\n') {
            start++;
        }
        line_end = start;
        while (line_end > line_start && (src[line_end - 1] == '\n' || src[line_end - 1] == '\r')) {
            line_end--;
        }
        decided = 0;
        if (auto_title_detect_line(allocator,
                                   src + line_start,
                                   src + line_end,
                                   title_out,
                                   title_cap_out,
                                   &decided) != 0) {
            return -1;
        }
        if (start < end) {
            start++;
        }
    }
    return 0;
}

static int auto_title_front_matter_delimiter(const char *line, size_t len)
{
    while (len > 0 &&
           (line[len - 1] == '\n' ||
            line[len - 1] == '\r' ||
            line[len - 1] == ' ' ||
            line[len - 1] == '\t')) {
        len--;
    }
    return len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-';
}

static int auto_title_front_matter_metadata_line(const char *line, size_t len, int *has_key_out)
{
    size_t i;
    int saw_key_char;

    *has_key_out = 0;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i == len) {
        return 1;
    }
    if (line[i] == '#') {
        return 1;
    }
    saw_key_char = 0;
    while (i < len) {
        unsigned char ch;

        ch = (unsigned char)line[i];
        if (ch == ':') {
            if (saw_key_char) {
                *has_key_out = 1;
            }
            return saw_key_char;
        }
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' ||
            ch == '-') {
            saw_key_char = 1;
            i++;
            continue;
        }
        return 0;
    }
    return 0;
}

static int auto_title_front_matter_continuation_line(const char *line, size_t len)
{
    size_t i;

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || (line[0] != ' ' && line[0] != '\t')) {
        return 0;
    }
    i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    return i < len && line[i] != '#';
}

static int auto_title_read_line(auto_title_source *src)
{
    for (;;) {
        char ch;
        int rc;

        rc = auto_title_read_byte(src, &ch);
        if (rc <= 0) {
            return rc;
        }
        if (ch == '\n') {
            return 1;
        }
    }
}

static int auto_title_detect_from_source(mdf_impl *impl, auto_title_source *src)
{
    size_t line_start;

    for (;;) {
        char ch;
        int spaces;
        int hash_count;
        int rc;

        line_start = src->scan_off;
        spaces = 0;
        for (;;) {
            rc = auto_title_read_byte(src, &ch);
            if (rc < 0) {
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
                return 0;
            }
            if (ch == ' ') {
                spaces++;
                if (spaces >= 4) {
                    return 0;
                }
                continue;
            }
            if (ch == '#') {
                hash_count = 1;
                for (;;) {
                    rc = auto_title_read_byte(src, &ch);
                    if (rc <= 0 || ch != '#') {
                        break;
                    }
                    hash_count++;
                }
                if (rc < 0) {
                    return -1;
                }
                if (hash_count >= 1 && hash_count <= 6 &&
                    (rc == 0 || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t')) {
                    while (rc > 0 && ch != '\n') {
                        rc = auto_title_read_byte(src, &ch);
                        if (rc < 0) {
                            return -1;
                        }
                    }
                    {
                        const char *line;
                        const char *line_end;
                        int decided;

                        line = src->prefix.buf + line_start;
                        line_end = src->prefix.buf + src->scan_off;
                        while (line_end > line && (line_end[-1] == '\n' || line_end[-1] == '\r')) {
                            line_end--;
                        }
                        decided = 0;
                        if (auto_title_detect_line(&impl->allocator,
                                                   line,
                                                   line_end,
                                                   &impl->html_title,
                                                   &impl->html_title_cap,
                                                   &decided) != 0) {
                            return -1;
                        }
                        if (impl->html_title != NULL) {
                            impl->html_title_auto = 1;
                        }
                    }
                }
                return 0;
            }
            if (line_start == 0 && spaces == 0 && ch == '-') {
                int fm;
                int fm_has_key;

                while (rc > 0 && ch != '\n') {
                    rc = auto_title_read_byte(src, &ch);
                    if (rc < 0) {
                        return -1;
                    }
                }
                fm = auto_title_front_matter_delimiter(src->prefix.buf + line_start,
                                                       src->scan_off - line_start);
                fm_has_key = 0;
                while (fm) {
                    size_t fm_line_start;
                    size_t fm_line_len;
                    int line_has_key;

                    fm_line_start = src->scan_off;
                    rc = auto_title_read_line(src);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc == 0) {
                        if (auto_title_detect_span(&impl->allocator,
                                                   src->prefix.buf,
                                                   line_start,
                                                   src->scan_off,
                                                   &impl->html_title,
                                                   &impl->html_title_cap) != 0) {
                            return -1;
                        }
                        if (impl->html_title != NULL) {
                            impl->html_title_auto = 1;
                        }
                        return 0;
                    }
                    fm_line_len = src->scan_off - fm_line_start;
                    if (auto_title_front_matter_delimiter(src->prefix.buf + fm_line_start, fm_line_len)) {
                        if (!fm_has_key &&
                            auto_title_detect_span(&impl->allocator,
                                                   src->prefix.buf,
                                                   line_start,
                                                   src->scan_off,
                                                   &impl->html_title,
                                                   &impl->html_title_cap) != 0) {
                            return -1;
                        }
                        break;
                    }
                    line_has_key = 0;
                    if (!auto_title_front_matter_metadata_line(src->prefix.buf + fm_line_start,
                                                               fm_line_len,
                                                               &line_has_key) &&
                        !(fm_has_key &&
                          auto_title_front_matter_continuation_line(src->prefix.buf + fm_line_start, fm_line_len))) {
                        if (auto_title_detect_span(&impl->allocator,
                                                   src->prefix.buf,
                                                   line_start,
                                                   src->scan_off,
                                                   &impl->html_title,
                                                   &impl->html_title_cap) != 0) {
                            return -1;
                        }
                        if (impl->html_title != NULL) {
                            impl->html_title_auto = 1;
                        }
                        return 0;
                    }
                    if (line_has_key) {
                        fm_has_key = 1;
                    }
                }
                if (fm && fm_has_key) {
                    break;
                } else if (fm) {
                    if (impl->html_title != NULL) {
                        impl->html_title_auto = 1;
                        return 0;
                    }
                    break;
                }
                return 0;
            }
            return 0;
        }
        if (src->scan_off == line_start) {
            break;
        }
    }
    return 0;
}

static int deck_write_html_escaped(mdf_sink *sink, const char *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        switch (src[i]) {
        case '&':
            if (mdf_write_cstr(sink, "&amp;") != 0) return -1;
            break;
        case '<':
            if (mdf_write_cstr(sink, "&lt;") != 0) return -1;
            break;
        case '>':
            if (mdf_write_cstr(sink, "&gt;") != 0) return -1;
            break;
        case '"':
            if (mdf_write_cstr(sink, "&#34;") != 0) return -1;
            break;
        case '\'':
            if (mdf_write_cstr(sink, "&#39;") != 0) return -1;
            break;
        default:
            if (mdf_write_all(sink, src + i, 1) != 0) return -1;
            break;
        }
    }
    return 0;
}

static int deck_reader_byte(deck_source_reader *reader, char *out)
{
    if (reader->off >= reader->len) {
        reader->err = 0;
        reader->len = reader->source->read(reader->source->userdata, reader->buf, sizeof(reader->buf), &reader->err);
        reader->off = 0;
        if (reader->len == 0) {
            return reader->err == 0 ? 0 : -1;
        }
    }
    *out = reader->buf[reader->off++];
    return 1;
}

static int deck_reader_line(deck_source_reader *reader, deck_string *line)
{
    int saw;

    line->len = 0;
    if (line->buf != NULL) {
        line->buf[0] = '\0';
    }
    saw = 0;
    for (;;) {
        char ch;
        int rc;

        rc = deck_reader_byte(reader, &ch);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            return saw ? 1 : 0;
        }
        saw = 1;
        if (deck_string_append(line, &ch, 1) != 0) {
            return -2;
        }
        if (ch == '\n') {
            return 1;
        }
    }
}

static size_t deck_line_content_len(const char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    return len;
}

static int deck_line_is_fence_open(const char *line, size_t len, char *marker_out, size_t *marker_len_out)
{
    size_t i;
    size_t count;
    char marker;

    i = 0;
    while (i < len && line[i] == ' ' && i < 3) {
        i++;
    }
    if (i >= len || (line[i] != '`' && line[i] != '~')) {
        return 0;
    }
    marker = line[i];
    count = 0;
    while (i < len && line[i] == marker) {
        count++;
        i++;
    }
    if (count < 3) {
        return 0;
    }
    *marker_out = marker;
    *marker_len_out = count;
    return 1;
}

static int deck_line_is_fence_close(const char *line, size_t len, char marker, size_t marker_len)
{
    size_t i;
    size_t count;

    i = 0;
    while (i < len && line[i] == ' ' && i < 3) {
        i++;
    }
    count = 0;
    while (i < len && line[i] == marker) {
        count++;
        i++;
    }
    if (count < marker_len) {
        return 0;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        i++;
    }
    return i == len;
}

static size_t deck_line_leading_spaces(const char *line, size_t len)
{
    size_t i;

    i = 0;
    while (i < len && line[i] == ' ') {
        i++;
    }
    return i;
}

static int deck_line_opens_list_item(const char *line, size_t len, size_t *continuation_indent)
{
    size_t i;
    size_t digits;

    i = deck_line_leading_spaces(line, len);
    if (i >= 4 || i >= len) {
        return 0;
    }
    if ((line[i] == '-' || line[i] == '+' || line[i] == '*') &&
        i + 1 < len &&
        (line[i + 1] == ' ' || line[i + 1] == '\t')) {
        *continuation_indent = i + 2;
        return 1;
    }
    digits = 0;
    while (i + digits < len && line[i + digits] >= '0' && line[i + digits] <= '9') {
        digits++;
    }
    if (digits > 0 &&
        digits <= 9 &&
        i + digits + 1 < len &&
        (line[i + digits] == '.' || line[i + digits] == ')') &&
        (line[i + digits + 1] == ' ' || line[i + digits + 1] == '\t')) {
        *continuation_indent = i + digits + 2;
        return 1;
    }
    return 0;
}

static int deck_line_is_separator(const char *line, size_t len, int in_list_context, size_t list_indent)
{
    size_t i;
    size_t count;
    char marker;

    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len == 0) {
        return 0;
    }
    if (line[0] == '\t' || line[0] == '>') {
        return 0;
    }
    i = deck_line_leading_spaces(line, len);
    if (i >= 4 || i >= len) {
        return 0;
    }
    if (in_list_context && i >= list_indent) {
        return 0;
    }
    marker = line[i];
    if (marker != '-' && marker != '*' && marker != '_') {
        return 0;
    }
    count = 0;
    while (i < len) {
        if (line[i] == marker) {
            count++;
            i++;
            continue;
        }
        if (line[i] == ' ' || line[i] == '\t') {
            i++;
            continue;
        }
        return 0;
    }
    return count >= 3;
}

static int deck_front_matter_closing_line(const char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        len--;
    }
    return len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-';
}

static int deck_copy_trimmed_value(mdf_allocator *allocator, const char *start, size_t len, char **out, size_t *cap_out)
{
    char *copy;
    const char *end;

    while (len > 0 && (*start == ' ' || *start == '\t')) {
        start++;
        len--;
    }
    end = start + len;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }
    if (end - start >= 2 &&
        ((*start == '"' && end[-1] == '"') || (*start == '\'' && end[-1] == '\''))) {
        start++;
        end--;
    }
    len = (size_t)(end - start);
    copy = (char *)mdf_alloc(allocator, len + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    *out = copy;
    *cap_out = len + 1;
    return 0;
}

static int deck_line_can_be_front_matter_metadata(const char *line, size_t len, int *has_key_out)
{
    size_t i;
    int saw_key_char;

    *has_key_out = 0;
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i == len) {
        return 1;
    }
    if (line[i] == '#') {
        return 1;
    }
    saw_key_char = 0;
    while (i < len) {
        unsigned char ch;

        ch = (unsigned char)line[i];
        if (ch == ':') {
            if (saw_key_char) {
                *has_key_out = 1;
            }
            return saw_key_char;
        }
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' ||
            ch == '-') {
            saw_key_char = 1;
            i++;
            continue;
        }
        return 0;
    }
    return 0;
}

static int deck_line_is_front_matter_continuation(const char *line, size_t len)
{
    size_t i;

    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len == 0 || (line[0] != ' ' && line[0] != '\t')) {
        return 0;
    }
    i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    return i < len && line[i] != '#';
}

static int deck_extract_front_matter_options(mdf_allocator *allocator,
                                             const char *src,
                                             size_t len,
                                             const char **body,
                                             size_t *body_len,
                                             char **theme_out,
                                             size_t *theme_cap_out)
{
    size_t line_start;
    size_t pos;
    size_t front_start;
    size_t first_line_end;

    *body = src;
    *body_len = len;
    *theme_out = NULL;
    *theme_cap_out = 0;
    first_line_end = 0;
    while (first_line_end < len && src[first_line_end] != '\n') {
        first_line_end++;
    }
    if (first_line_end >= len ||
        !deck_front_matter_closing_line(src, first_line_end)) {
        return 0;
    }
    front_start = first_line_end + 1;
    pos = front_start;
    while (pos <= len) {
        size_t line_end;
        size_t next;

        line_start = pos;
        while (pos < len && src[pos] != '\n') {
            pos++;
        }
        line_end = pos;
        next = pos < len ? pos + 1 : pos;
        if (deck_front_matter_closing_line(src + line_start, line_end - line_start)) {
            size_t scan;

            scan = front_start;
            while (scan < line_start) {
                size_t fm_line_start;
                size_t fm_line_end;
                const char theme_key[] = "theme:";

                fm_line_start = scan;
                while (scan < line_start && src[scan] != '\n') {
                    scan++;
                }
                fm_line_end = scan;
                if (fm_line_end - fm_line_start >= sizeof(theme_key) - 1 &&
                    memcmp(src + fm_line_start, theme_key, sizeof(theme_key) - 1) == 0) {
                    char *next_theme;
                    size_t next_theme_cap;

                    next_theme = NULL;
                    next_theme_cap = 0;
                    if (deck_copy_trimmed_value(allocator,
                                                src + fm_line_start + sizeof(theme_key) - 1,
                                                fm_line_end - fm_line_start - (sizeof(theme_key) - 1),
                                                &next_theme,
                                                &next_theme_cap) != 0) {
                        return -1;
                    }
                    mdf_free_mem(allocator, *theme_out, *theme_cap_out);
                    *theme_out = next_theme;
                    *theme_cap_out = next_theme_cap;
                }
                if (scan < line_start) {
                    scan++;
                }
            }
            *body = src + next;
            *body_len = len - next;
            return 0;
        }
        if (pos >= len) {
            return 0;
        }
        pos = next;
    }
    return 0;
}

static int deck_render_write_slide(mdf_renderer *self,
                                   mdf **html_io,
                                   const mdf_options *opts,
                                   mdf_sink *sink,
                                   deck_string *markdown,
                                   int *shell_open,
                                   size_t *slide_index);

static int deck_replay_pending_front_matter(mdf_renderer *self,
                                            deck_string *pending,
                                            deck_string *markdown)
{
    if (pending->len > 0 &&
        deck_string_append(markdown, pending->buf, pending->len) != 0) {
        mdf_set_error(self, "out of memory");
        return -1;
    }
    pending->len = 0;
    if (pending->buf != NULL) {
        pending->buf[0] = '\0';
    }
    return 0;
}

static int deck_replay_pending_front_matter_body(mdf_renderer *self,
                                                 deck_string *pending,
                                                 deck_string *markdown)
{
    size_t off;

    off = 0;
    while (off < pending->len && pending->buf[off] != '\n') {
        off++;
    }
    if (off < pending->len) {
        off++;
    }
    if (off < pending->len &&
        deck_string_append(markdown, pending->buf + off, pending->len - off) != 0) {
        mdf_set_error(self, "out of memory");
        return -1;
    }
    pending->len = 0;
    if (pending->buf != NULL) {
        pending->buf[0] = '\0';
    }
    return 0;
}

static const char *deck_transition_name(mdf_deck_transition transition)
{
    switch (transition) {
    case MDF_DECK_TRANSITION_CROSS:
        return "cross";
    case MDF_DECK_TRANSITION_HARD:
        return "hard";
    case MDF_DECK_TRANSITION_FADE:
    default:
        return "fade";
    }
}

static int deck_write_shell_start(mdf_renderer *self,
                                  mdf_renderer *html_renderer,
                                  const mdf_options *opts,
                                  mdf_sink *sink);
static int deck_write_slide(mdf_renderer *self, const mdf_options *opts, mdf_sink *sink, const deck_slide *slide, size_t index);

static int deck_render_slide(mdf_renderer *self,
                             mdf **html_io,
                             const mdf_options *opts,
                             const deck_slice *slice,
                             deck_slide *slide)
{
    mdf *html;
    mdf_options html_opts;
    mdf_status st;
    mdf_allocator *allocator;
    deck_slice_source src_data;
    mdf_source src;
    deck_slide_sink sink_data;
    mdf_sink sink;

    allocator = &((mdf_impl *)self->impl)->allocator;
    html = *html_io;
    if (html == NULL) {
        html_opts = *opts;
        html_opts.html_dump_font = 0;
        html_opts.html_dump_font_force = 0;
        st = mdf_create(MDF_FORMAT_HTML, &html_opts, &html);
        if (st != MDF_OK) {
            mdf_set_error(self, st == MDF_ERROR_NOMEM ? "out of memory" : "deck slide HTML renderer creation failed");
            return -1;
        }
        ((mdf_impl *)html->impl)->html_links_blank = 1;
        ((mdf_impl *)html->impl)->html_fragment = 1;
        st = mdf_set_html_title(html, "mdf");
        if (st != MDF_OK) {
            html->destroy(html);
            mdf_set_error(self, st == MDF_ERROR_NOMEM ? "out of memory" : "deck slide HTML title setup failed");
            return -1;
        }
        *html_io = html;
    }
    memset(&src_data, 0, sizeof(src_data));
    src_data.src = slice->src;
    src_data.len = slice->len;
    src.userdata = &src_data;
    src.read = deck_slice_source_read;
    memset(&sink_data, 0, sizeof(sink_data));
    sink_data.html.allocator = allocator;
    sink.userdata = &sink_data;
    sink.write = deck_slide_sink_write;
    st = html->render(html, &src, &sink);
    if (st != MDF_OK) {
        const char *err;

        err = html->error(html);
        deck_string_dispose(&sink_data.html);
        if (sink_data.oom) {
            mdf_set_error(self, "out of memory");
            return -1;
        }
        mdf_set_error(self, err != NULL && err[0] != '\0' ? err : "deck slide HTML render failed");
        return -1;
    }
    slide->html = sink_data.html.buf;
    slide->html_len = sink_data.html.len;
    slide->html_cap = sink_data.html.cap;
    return 0;
}

static int deck_render_write_slide(mdf_renderer *self,
                                   mdf **html_io,
                                   const mdf_options *opts,
                                   mdf_sink *sink,
                                   deck_string *markdown,
                                   int *shell_open,
                                   size_t *slide_index)
{
    mdf_impl *impl;
    deck_slide slide;
    deck_slice slice;

    impl = (mdf_impl *)self->impl;
    memset(&slide, 0, sizeof(slide));
    slice.src = markdown->buf != NULL ? markdown->buf : "";
    slice.len = markdown->len;
    if (deck_render_slide(self, html_io, opts, &slice, &slide) != 0) {
        return -1;
    }
    if (!*shell_open) {
        if (deck_write_shell_start(self, *html_io, opts, sink) != 0) {
            mdf_free_mem(&impl->allocator, slide.html, slide.html_cap);
            mdf_set_error(self, "sink write failed");
            return -1;
        }
        *shell_open = 1;
    }
    if (deck_write_slide(self, opts, sink, &slide, *slide_index) != 0) {
        mdf_free_mem(&impl->allocator, slide.html, slide.html_cap);
        mdf_set_error(self, "sink write failed");
        return -1;
    }
    *slide_index += 1;
    mdf_free_mem(&impl->allocator, slide.html, slide.html_cap);
    markdown->len = 0;
    if (markdown->buf != NULL) {
        markdown->buf[0] = '\0';
    }
    return 0;
}

static int deck_write_number(mdf_sink *sink, size_t value, size_t width)
{
    char buf[32];
    char fmt[16];
    int n;

    snprintf(fmt, sizeof(fmt), "%%0%lulu", (unsigned long)width);
    n = snprintf(buf, sizeof(buf), fmt, (unsigned long)value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    return mdf_write_all(sink, buf, (size_t)n);
}

static int deck_write_shell_start(mdf_renderer *self,
                                  mdf_renderer *html_renderer,
                                  const mdf_options *opts,
                                  mdf_sink *sink)
{
    mdf_impl *impl;
    mdf_impl theme_impl;
    char heading_rgb_buf[32];
    const char *title;
    const char *transition;
    const char *slide_number_rgb;

    impl = (mdf_impl *)self->impl;
    theme_impl = *impl;
    theme_impl.theme = mdf_theme_resolve(opts == NULL ? NULL : opts->theme_name);
    title = impl->html_title != NULL ? impl->html_title : "mdf";
    transition = deck_transition_name(impl->opts.deck_transition);
    slide_number_rgb = html_theme_heading_rgb(&theme_impl, 3, heading_rgb_buf, sizeof(heading_rgb_buf));
    if (mdf_write_cstr(sink, "<!doctype html>\n<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>") != 0) return -1;
    if (deck_write_html_escaped(sink, title, strlen(title)) != 0) return -1;
    if (mdf_write_cstr(sink, "</title>\n<style>\n") != 0) return -1;
    if (html_write_default_css(html_renderer, sink) != 0) return -1;
    if (mdf_write_cstr(sink, "\nhtml,body{width:100%;height:100%;overflow:hidden;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "body{background:#000;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck{position:relative;width:100vw;height:100vh;overflow:hidden;background:#000;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck.mdf-cursor-hidden,.mdf-deck.mdf-cursor-hidden *{cursor:none!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide{position:absolute;inset:0;box-sizing:border-box;padding:clamp(18px,4vw,72px);display:grid;align-items:start;justify-items:stretch;opacity:0;visibility:hidden;pointer-events:none;background:inherit;color:inherit;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide{transition:opacity 140ms ease,visibility 0s linear 140ms;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide[aria-hidden=\"false\"]{opacity:1;visibility:visible;pointer-events:auto;transition:opacity 140ms ease;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-front{align-items:center;justify-items:center;text-align:left;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-front .mdf-slide-content{max-width:min(88vw,80ch);text-align:left;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-front.mdf-center-front-text .mdf-slide-content{text-align:center;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content{box-sizing:border-box;width:min(100%,92vw,var(--mdf-content-max-width,96ch));max-width:100%;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content{max-height:calc(100vh - clamp(104px,12vw,160px));overflow:auto;white-space:pre-wrap;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content{padding-bottom:clamp(48px,8vh,72px);}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-front .mdf-slide-content{width:max-content;max-width:min(88vw,80ch);max-height:none;overflow:visible;padding-bottom:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content{overflow-wrap:anywhere;tab-size:4;font-size:var(--mdf-slide-font-size,1.6rem);line-height:1.35;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered{height:calc(100vh - clamp(104px,12vw,160px));display:flex;flex-direction:column;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered .mdf-slide-header{flex:0 0 auto;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered .mdf-slide-header .mdf-heading{display:block;margin:0 0 .5em;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered .mdf-slide-body{flex:1 1 auto;min-height:0;min-width:0;display:flex;flex-direction:column;align-items:flex-start;justify-content:center;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered:has(.mdf-chart-block){width:100%;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body-inner{flex:0 0 auto;width:100%;min-width:0;max-width:100%;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content-centered.mdf-slide-scroll-fallback .mdf-slide-body{justify-content:flex-start;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body-inner>.mdf-heading+br,.mdf-slide-body-inner>.mdf-heading+br+br{display:none;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body-inner>.mdf-heading{display:block;margin:0 0 .45em;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body-inner>.mdf-heading~.mdf-heading{margin-top:.8em;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content .mdf-heading{line-height:1.12;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:32pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:32pt\"]{font-size:min(7.4vmin,7vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:22pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:22pt\"]{font-size:min(6.8vmin,6.4vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:15pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:15pt\"]{font-size:min(6.2vmin,5.8vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:5ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:5ch\"]{font-size:min(5.7vmin,5.3vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:6ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:6ch\"]{font-size:min(5.2vmin,4.8vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:7ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:7ch\"]{font-size:min(4.8vmin,4.4vw)!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:32pt\"]{font-size:1.55em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:22pt\"]{font-size:1.35em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:15pt\"]{font-size:1.2em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:5ch\"]{font-size:1.1em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:6ch\"]{font-size:1.05em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:7ch\"]{font-size:1em!important;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "@media (max-width:640px){") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:32pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:32pt\"]{font-size:clamp(1.9rem,8vw,32pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:22pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:22pt\"]{font-size:clamp(1.75rem,7.4vw,36pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"font-size:15pt\"],.mdf-slide-front .mdf-heading[style*=\"font-size:15pt\"]{font-size:clamp(1.6rem,6.8vw,32pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:5ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:5ch\"]{font-size:clamp(1.45rem,6.2vw,28pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:6ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:6ch\"]{font-size:clamp(1.32rem,5.7vw,24pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-header .mdf-heading[style*=\"--mdf-heading-indent:7ch\"],.mdf-slide-front .mdf-heading[style*=\"--mdf-heading-indent:7ch\"]{font-size:clamp(1.2rem,5.2vw,21pt)!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:32pt\"]{font-size:1.55em!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:22pt\"]{font-size:1.35em!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"font-size:15pt\"]{font-size:1.2em!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:5ch\"]{font-size:1.1em!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:6ch\"]{font-size:1.05em!important;}") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-body .mdf-heading[style*=\"--mdf-heading-indent:7ch\"]{font-size:1em!important;}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content .mdf-table{font-size:.86em;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-content .mdf-chart-block{font-size:.82em;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "@media (max-height:520px){.mdf-slide-content{max-height:calc(100vh - 112px);}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-number{position:absolute;right:clamp(16px,3vw,40px);bottom:clamp(14px,3vw,34px);}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-slide-number{font-weight:700;opacity:.72;font-size:clamp(.8rem,1.6vw,1.1rem);color:rgb(") != 0) return -1;
    if (mdf_write_cstr(sink, slide_number_rgb) != 0) return -1;
    if (mdf_write_cstr(sink, ");}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-fullscreen-button{position:absolute;left:0;bottom:0;z-index:10;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-fullscreen-button{width:clamp(48px,9vw,64px);height:clamp(48px,9vw,64px);border:0;background:transparent;color:transparent;opacity:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-fullscreen-button{padding:0;font:0/0 monospace;border-radius:0;touch-action:manipulation;cursor:pointer;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck.mdf-fallback-fullscreen{position:fixed;inset:0;z-index:2147483647;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck[data-transition=\"hard\"] .mdf-slide{transition:none;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck[data-transition=\"cross\"] .mdf-slide{transition:opacity 1600ms ease,visibility 0s linear 1600ms;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck[data-transition=\"cross\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 1600ms ease,visibility 0s linear 0s;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck[data-transition=\"fade\"] .mdf-slide{transition:opacity 520ms ease,visibility 0s linear 0s;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck[data-transition=\"fade\"] .mdf-slide[aria-hidden=\"false\"]{transition:opacity 520ms ease,visibility 0s linear 0s;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-deck.mdf-blackout .mdf-slide{opacity:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "@media (prefers-reduced-motion:reduce){.mdf-slide{transition:none!important;}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "</style>\n</head>\n<body>\n<main class=\"mdf-deck\" data-transition=\"") != 0) return -1;
    if (mdf_write_cstr(sink, transition) != 0) return -1;
    if (mdf_write_cstr(sink, "\" data-current=\"1\">\n") != 0) return -1;
    if (mdf_write_cstr(sink, "<button class=\"mdf-fullscreen-button\" type=\"button\" tabindex=\"-1\" aria-label=\"Toggle fullscreen\"></button>\n") != 0) return -1;
    return 0;
}

static int deck_find_top_heading_end(const deck_slide *slide, size_t *header_end)
{
    const char *start;
    const char *gt;
    const char *close;
    const char heading_open[] = "<span class=\"mdf-heading\"";
    size_t open_len;
    size_t off;
    size_t marker_len;

    if (slide == NULL || slide->html == NULL) {
        return 0;
    }
    off = 0;
    while (off < slide->html_len &&
           (slide->html[off] == ' ' || slide->html[off] == '\t' || slide->html[off] == '\n' || slide->html[off] == '\r')) {
        off++;
    }
    start = slide->html + off;
    open_len = sizeof(heading_open) - 1;
    if (slide->html_len - off < open_len || memcmp(start, heading_open, open_len) != 0) {
        return 0;
    }
    gt = strchr(start, '>');
    if (gt == NULL || (size_t)(gt - slide->html) + 2 >= slide->html_len) {
        return 0;
    }
    marker_len = 0;
    while ((size_t)(gt + 1 + marker_len - slide->html) < slide->html_len &&
           marker_len < 6 &&
           gt[1 + marker_len] == '#') {
        marker_len++;
    }
    if (marker_len == 0 ||
        (size_t)(gt + 1 + marker_len - slide->html) >= slide->html_len ||
        gt[1 + marker_len] != ' ') {
        return 0;
    }
    close = strstr(gt + 1, "</span>");
    if (close == NULL || close < start || (size_t)(close - slide->html) >= slide->html_len) {
        return 0;
    }
    *header_end = (size_t)(close - slide->html) + strlen("</span>");
    return 1;
}

static int deck_write_body_html(mdf_sink *sink, const char *html, size_t len)
{
    const char heading_open[] = "<span class=\"mdf-heading\"";
    const char heading_close[] = "</span>";
    const size_t heading_open_len = sizeof(heading_open) - 1;
    const size_t heading_close_len = sizeof(heading_close) - 1;
    size_t pos;

    pos = 0;
    while (pos < len) {
        if (len - pos >= heading_open_len && memcmp(html + pos, heading_open, heading_open_len) == 0) {
            const char *close;
            size_t heading_end;

            close = strstr(html + pos, heading_close);
            if (close != NULL && (size_t)(close - html) < len) {
                heading_end = (size_t)(close - html) + heading_close_len;
                if (mdf_write_all(sink, html + pos, heading_end - pos) != 0) return -1;
                pos = heading_end;
                while (pos < len &&
                       (html[pos] == ' ' ||
                        html[pos] == '\t' ||
                        html[pos] == '\n' ||
                        html[pos] == '\r')) {
                    pos++;
                }
                continue;
            }
        }
        {
            const char *found;
            size_t next;

            next = pos;
            for (;;) {
                found = (const char *)memchr(html + next, '<', len - next);
                if (found == NULL) {
                    next = len;
                    break;
                }
                next = (size_t)(found - html);
                if (len - next >= heading_open_len && memcmp(html + next, heading_open, heading_open_len) == 0) {
                    break;
                }
                next++;
            }
            if (mdf_write_all(sink, html + pos, next - pos) != 0) return -1;
            pos = next;
        }
    }
    return 0;
}

static int deck_write_slide_body_html(mdf_sink *sink, const deck_slide *slide, int center_body)
{
    size_t header_end;
    size_t body_start;
    size_t body_end;

    if (!center_body) {
        return mdf_write_all(sink, slide->html, slide->html_len);
    }
    if (deck_find_top_heading_end(slide, &header_end)) {
        body_start = header_end;
        body_end = slide->html_len;
        while (body_start < body_end &&
               (slide->html[body_start] == ' ' ||
                slide->html[body_start] == '\t' ||
                slide->html[body_start] == '\n' ||
                slide->html[body_start] == '\r')) {
            body_start++;
        }
        while (body_end > body_start &&
               (slide->html[body_end - 1] == ' ' ||
                slide->html[body_end - 1] == '\t' ||
                slide->html[body_end - 1] == '\n' ||
                slide->html[body_end - 1] == '\r')) {
            body_end--;
        }
        if (mdf_write_cstr(sink, "<div class=\"mdf-slide-header\">") != 0) return -1;
        if (mdf_write_all(sink, slide->html, header_end) != 0) return -1;
        if (mdf_write_cstr(sink, "</div><div class=\"mdf-slide-body\"><div class=\"mdf-slide-body-inner\">") != 0) return -1;
        if (deck_write_body_html(sink, slide->html + body_start, body_end - body_start) != 0) return -1;
        return mdf_write_cstr(sink, "</div></div>");
    }
    body_start = 0;
    body_end = slide->html_len;
    while (body_start < body_end &&
           (slide->html[body_start] == ' ' ||
            slide->html[body_start] == '\t' ||
            slide->html[body_start] == '\n' ||
            slide->html[body_start] == '\r')) {
        body_start++;
    }
    while (body_end > body_start &&
           (slide->html[body_end - 1] == ' ' ||
            slide->html[body_end - 1] == '\t' ||
            slide->html[body_end - 1] == '\n' ||
            slide->html[body_end - 1] == '\r')) {
        body_end--;
    }
    if (mdf_write_cstr(sink, "<div class=\"mdf-slide-body\"><div class=\"mdf-slide-body-inner\">") != 0) return -1;
    if (deck_write_body_html(sink, slide->html + body_start, body_end - body_start) != 0) return -1;
    return mdf_write_cstr(sink, "</div></div>");
}

static int deck_write_slide(mdf_renderer *self, const mdf_options *opts, mdf_sink *sink, const deck_slide *slide, size_t index)
{
    mdf_impl *impl;
    int center_body;

    impl = (mdf_impl *)self->impl;
    (void)opts;
    center_body = index > 0;
    if (mdf_write_cstr(sink, "<section class=\"mdf-slide") != 0) return -1;
    if (index == 0) {
        if (mdf_write_cstr(sink, " mdf-slide-front") != 0) return -1;
        if (impl->opts.deck_center_front_text) {
            if (mdf_write_cstr(sink, " mdf-center-front-text") != 0) return -1;
        }
    }
    if (mdf_write_cstr(sink, "\" data-slide=\"") != 0) return -1;
    if (deck_write_number(sink, index + 1, 1) != 0) return -1;
    if (mdf_write_cstr(sink, "\" aria-hidden=\"") != 0) return -1;
    if (mdf_write_cstr(sink, index == 0 ? "false" : "true") != 0) return -1;
    if (mdf_write_cstr(sink, "\"><div class=\"mdf-slide-content") != 0) return -1;
    if (center_body) {
        if (mdf_write_cstr(sink, " mdf-slide-content-centered") != 0) return -1;
    }
    if (mdf_write_cstr(sink, "\">") != 0) return -1;
    if (deck_write_slide_body_html(sink, slide, center_body) != 0) return -1;
    if (mdf_write_cstr(sink, "</div>") != 0) return -1;
    if (impl->opts.slide_numbers && index > 0) {
        if (mdf_write_cstr(sink, "<div class=\"mdf-slide-number\" aria-hidden=\"true\"></div>") != 0) return -1;
    }
    return mdf_write_cstr(sink, "</section>\n");
}

static int deck_write_shell_end(mdf_sink *sink)
{
    if (mdf_write_cstr(sink, "</main>\n<script>\n(function(){'use strict';\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var deck=document.querySelector('.mdf-deck');if(!deck)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var slides=[].slice.call(deck.querySelectorAll('.mdf-slide'));var last=0;var cur=0;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var focusable='a[href],area[href],button,input,select,textarea,iframe,object,embed,[tabindex],details>summary';\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var fs=deck.querySelector('.mdf-fullscreen-button');var reduceMotion=!!(window.matchMedia&&matchMedia('(prefers-reduced-motion: reduce)').matches);function refit(){setTimeout(function(){fit(slides[cur]);},60);}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var cursorTimer=0;function showCursor(){deck.classList.remove('mdf-cursor-hidden');clearTimeout(cursorTimer);cursorTimer=setTimeout(function(){deck.classList.add('mdf-cursor-hidden');},2500);}showCursor();\n") != 0) return -1;
    if (mdf_write_cstr(sink, "function toggleFullscreen(){var el=deck;var req=el.requestFullscreen||el.webkitRequestFullscreen;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var exit=document.exitFullscreen||document.webkitExitFullscreen;if(document.fullscreenElement||document.webkitFullscreenElement){if(exit)exit.call(document);else deck.classList.remove('mdf-fallback-fullscreen');refit();return;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(req){var p=req.call(el);if(p&&p.catch)p.catch(function(){deck.classList.toggle('mdf-fallback-fullscreen');refit();});else refit();}else{deck.classList.toggle('mdf-fallback-fullscreen');refit();}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "function fit(slide){var box=slide.querySelector('.mdf-slide-content');if(!box)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var vw=Math.max(320,innerWidth||document.documentElement.clientWidth||1024);\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var vh=Math.max(320,innerHeight||document.documentElement.clientHeight||768);\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var dense=box.textContent.length>520;var scale=Math.min(vw,vh);var base=scale*(slide.classList.contains('mdf-slide-front') ? .072 : (dense ? .038 : .047));\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(vw<640)base=scale*(slide.classList.contains('mdf-slide-front') ? .087 : (dense ? .046 : .056));\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(vh<520)base=scale*(slide.classList.contains('mdf-slide-front') ? .077 : (dense ? .041 : .051));\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var hasChart=!!box.querySelector('.mdf-chart-block');var min=vw<640?(hasChart?8:12):14;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(vh<520)min=hasChart?8:12;var body=box.querySelector('.mdf-slide-body');var inner=box.querySelector('.mdf-slide-body-inner');var size=Math.max(min,Math.round(base));\n") != 0) return -1;
    if (mdf_write_cstr(sink, "box.style.setProperty('--mdf-slide-font-size',size+'px');\n") != 0) return -1;
    if (mdf_write_cstr(sink, "for(;size>min;size-=1){box.style.setProperty('--mdf-slide-font-size',size+'px');\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(box.scrollHeight<=box.clientHeight+4&&box.scrollWidth<=box.clientWidth+4&&(!inner||(inner.scrollHeight<=body.clientHeight+4&&inner.scrollWidth<=body.clientWidth+4)))break;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "box.classList.toggle('mdf-slide-scroll-fallback',box.scrollHeight>box.clientHeight+4||box.scrollWidth>box.clientWidth+4||(inner&&(inner.scrollHeight>body.clientHeight+4||inner.scrollWidth>body.clientWidth+4)));}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "function syncSlides(){slides.forEach(function(s,i){var active=i===cur;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "s.setAttribute('aria-hidden',active?'false':'true');if('inert'in s)s.inert=!active;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "[].slice.call(s.querySelectorAll(focusable)).forEach(function(el){if(active){\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(el.hasAttribute('data-mdf-tabindex')){var v=el.getAttribute('data-mdf-tabindex');el.removeAttribute('data-mdf-tabindex');if(v==='')el.removeAttribute('tabindex');else el.setAttribute('tabindex',v);}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "else{if(!el.hasAttribute('data-mdf-tabindex'))el.setAttribute('data-mdf-tabindex',el.hasAttribute('tabindex')?el.getAttribute('tabindex'):'');el.setAttribute('tabindex','-1');}});});deck.dataset.current=String(cur+1);}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "function show(n,remember){if(n<0)n=0;if(n>=slides.length)n=slides.length-1;if(n===cur)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(remember)last=cur;var old=cur;cur=n;function apply(){syncSlides();\n") != 0) return -1;
    if (mdf_write_cstr(sink, "location.hash='slide-'+String(cur+1);fit(slides[cur]);}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(deck.dataset.transition==='fade'&&!reduceMotion&&old!==cur){deck.classList.add('mdf-blackout');\n") != 0) return -1;
    if (mdf_write_cstr(sink, "setTimeout(function(){apply();void deck.offsetWidth;deck.classList.remove('mdf-blackout');},520);}else{apply();}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "function fromHash(){var m=/^#slide-(\\d+)$/.exec(location.hash);if(m){\n") != 0) return -1;
    if (mdf_write_cstr(sink, "cur=Math.max(0,Math.min(slides.length-1,parseInt(m[1],10)-1));}}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "fromHash();syncSlides();\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var w=String(slides.length).length;if(w<2)w=2;slides.forEach(function(s,i){\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var n=s.querySelector('.mdf-slide-number');if(n)n.textContent=String(i+1).padStart(w,'0')+'/'+String(slides.length).padStart(w,'0');});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "fit(slides[cur]);addEventListener('resize',function(){fit(slides[cur]);});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "addEventListener('pointermove',showCursor,{passive:true});addEventListener('mousemove',showCursor,{passive:true});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(fs)fs.addEventListener('click',toggleFullscreen);\n") != 0) return -1;
    if (mdf_write_cstr(sink, "document.addEventListener('fullscreenchange',refit);document.addEventListener('webkitfullscreenchange',refit);\n") != 0) return -1;
    if (mdf_write_cstr(sink, "addEventListener('hashchange',function(){var before=cur;fromHash();if(before!==cur){\n") != 0) return -1;
    if (mdf_write_cstr(sink, "syncSlides();fit(slides[cur]);}});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "document.addEventListener('keydown',function(e){if(e.altKey||e.ctrlKey||e.metaKey)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var t=e.target;if(t&&(t.isContentEditable||(t.closest&&t.closest('a[href],button,summary,input,select,textarea'))))return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "switch(e.key){case'ArrowRight':case'ArrowDown':case'PageDown':case' ':case'j':\n") != 0) return -1;
    if (mdf_write_cstr(sink, "e.preventDefault();show(cur+1,true);break;case'ArrowLeft':case'ArrowUp':case'PageUp':case'k':\n") != 0) return -1;
    if (mdf_write_cstr(sink, "e.preventDefault();show(cur-1,true);break;case'Home':case'g':e.preventDefault();show(0,true);break;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "case'End':case'G':e.preventDefault();show(slides.length-1,true);break;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "case'f':e.preventDefault();toggleFullscreen();break;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "case'o':e.preventDefault();show(last,false);break;}});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var sx=0,sy=0,swiping=false;deck.addEventListener('touchstart',function(e){\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(e.touches.length!==1)return;var t=e.touches[0];sx=t.clientX;sy=t.clientY;swiping=true;},{passive:true});\n") != 0) return -1;
    if (mdf_write_cstr(sink, "deck.addEventListener('touchend',function(e){if(!swiping)return;swiping=false;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var t=e.changedTouches&&e.changedTouches[0];if(!t)return;var dx=t.clientX-sx;var dy=t.clientY-sy;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "var box=slides[cur]&&slides[cur].querySelector('.mdf-slide-content');\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(Math.abs(dx)>=48&&Math.abs(dx)>=Math.abs(dy)*1.4){if(dx<0)show(cur+1,true);else show(cur-1,true);return;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(Math.abs(dy)<48||Math.abs(dy)<Math.abs(dx)*1.4)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(box&&box.scrollHeight>box.clientHeight+4){if(dy<0&&box.scrollTop+box.clientHeight<box.scrollHeight-4)return;\n") != 0) return -1;
    if (mdf_write_cstr(sink, "if(dy>0&&box.scrollTop>1)return;}if(dy<0)show(cur+1,true);else show(cur-1,true);},{passive:true});})();\n") != 0) return -1;
    if (mdf_write_cstr(sink, "</script>\n</body>\n</html>\n") != 0) return -1;
    return 0;
}

static mdf_status deck_render(mdf_renderer *self, mdf_source *source, mdf_sink *sink)
{
    mdf_impl *impl;
    deck_source_reader reader;
    deck_string slide_markdown;
    deck_string line;
    deck_string front_matter;
    char *front_theme;
    size_t front_theme_cap;
    mdf *html_renderer;
    mdf_options html_opts;
    mdf_status status;
    int line_no;
    int pending_front_matter;
    int in_front_matter;
    int front_matter_has_key;
    int in_fence;
    int in_list_context;
    int blockquote_lazy_active;
    int shell_open;
    size_t list_indent;
    size_t slide_index;
    char fence_marker;
    size_t fence_len;

    impl = (mdf_impl *)self->impl;
    memset(&reader, 0, sizeof(reader));
    reader.source = source;
    memset(&slide_markdown, 0, sizeof(slide_markdown));
    slide_markdown.allocator = &impl->allocator;
    memset(&line, 0, sizeof(line));
    line.allocator = &impl->allocator;
    memset(&front_matter, 0, sizeof(front_matter));
    front_matter.allocator = &impl->allocator;
    front_theme = NULL;
    front_theme_cap = 0;
    html_renderer = NULL;
    status = MDF_OK;
    html_opts = impl->opts;
    /* Font URI option strings are caller-owned.  The outer renderer resolved
     * and copied the effective face URIs during mdf_create(), so pass those
     * stable copies to the lazily-created HTML slide renderer instead. */
    html_opts.html_font_uri = NULL;
    html_opts.html_font_regular_uri = impl->html_font_regular_uri;
    html_opts.html_font_italic_uri = impl->html_font_italic_uri;
    html_opts.allocator = impl->user_allocator;
    memset(&html_opts.emission_buffer, 0, sizeof(html_opts.emission_buffer));
    line_no = 0;
    pending_front_matter = 0;
    in_front_matter = 0;
    front_matter_has_key = 0;
    in_fence = 0;
    in_list_context = 0;
    blockquote_lazy_active = 0;
    shell_open = 0;
    list_indent = 0;
    slide_index = 0;
    fence_marker = '\0';
    fence_len = 0;
    for (;;) {
        int rc;
        size_t content_len;

        rc = deck_reader_line(&reader, &line);
        if (rc < 0) {
            mdf_set_error(self, rc == -2 ? "out of memory" : "source read failed");
            status = rc == -2 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
            goto done;
        }
        if (rc == 0) {
            break;
        }
        line_no++;
        content_len = deck_line_content_len(line.buf, line.len);
        if (line_no == 1 && deck_front_matter_closing_line(line.buf, content_len)) {
            pending_front_matter = 1;
            in_front_matter = 1;
            front_matter_has_key = 0;
            if (deck_string_append(&front_matter, line.buf, line.len) != 0) {
                mdf_set_error(self, "out of memory");
                status = MDF_ERROR_NOMEM;
                goto done;
            }
            continue;
        }
        if (in_front_matter) {
            if (deck_front_matter_closing_line(line.buf, content_len)) {
                const char *ignored_body;
                size_t ignored_body_len;

                if (deck_string_append(&front_matter, line.buf, line.len) != 0) {
                    mdf_set_error(self, "out of memory");
                    status = MDF_ERROR_NOMEM;
                    goto done;
                }
                if (!front_matter_has_key) {
                    if (deck_replay_pending_front_matter(self, &front_matter, &slide_markdown) != 0) {
                        status = MDF_ERROR_NOMEM;
                        goto done;
                    }
                    in_front_matter = 0;
                    pending_front_matter = 0;
                    front_matter_has_key = 0;
                    continue;
                }
                if (deck_extract_front_matter_options(&impl->allocator,
                                                      front_matter.buf,
                                                      front_matter.len,
                                                      &ignored_body,
                                                      &ignored_body_len,
                                                      &front_theme,
                                                      &front_theme_cap) != 0) {
                    mdf_set_error(self, "out of memory");
                    status = MDF_ERROR_NOMEM;
                    goto done;
                }
                (void)ignored_body;
                (void)ignored_body_len;
                if (front_theme != NULL && !impl->theme_name_explicit && mdf_theme_exists(front_theme)) {
                    html_opts.theme_name = front_theme;
                }
                in_front_matter = 0;
                pending_front_matter = 0;
                front_matter_has_key = 0;
                front_matter.len = 0;
                if (front_matter.buf != NULL) {
                    front_matter.buf[0] = '\0';
                }
                continue;
            } else {
                int line_has_key;

                line_has_key = 0;
                if (deck_line_can_be_front_matter_metadata(line.buf, content_len, &line_has_key) ||
                    (front_matter_has_key &&
                     deck_line_is_front_matter_continuation(line.buf, content_len))) {
                    if (line_has_key) {
                        front_matter_has_key = 1;
                    }
                    if (deck_string_append(&front_matter, line.buf, line.len) != 0) {
                        mdf_set_error(self, "out of memory");
                        status = MDF_ERROR_NOMEM;
                        goto done;
                    }
                    continue;
                }
            }
            if (in_front_matter) {
                if (deck_render_write_slide(self,
                                            &html_renderer,
                                            &html_opts,
                                            sink,
                                            &slide_markdown,
                                            &shell_open,
                                            &slide_index) != 0) {
                    status = strcmp(self->error(self), "out of memory") == 0 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
                    goto done;
                }
                if (deck_replay_pending_front_matter_body(self, &front_matter, &slide_markdown) != 0) {
                    status = MDF_ERROR_NOMEM;
                    goto done;
                }
                in_front_matter = 0;
                pending_front_matter = 0;
                front_matter_has_key = 0;
            }
        }
        if (in_fence) {
            if (deck_line_is_fence_close(line.buf, content_len, fence_marker, fence_len)) {
                in_fence = 0;
                fence_len = 0;
            }
        } else {
            char marker;
            size_t marker_len;
            size_t opened_list_indent;
            size_t leading_spaces;
            int line_is_blank;
            int line_starts_blockquote;

            marker = '\0';
            marker_len = 0;
            opened_list_indent = 0;
            leading_spaces = deck_line_leading_spaces(line.buf, content_len);
            line_is_blank = leading_spaces >= content_len;
            line_starts_blockquote = leading_spaces < content_len && line.buf[leading_spaces] == '>';
            if (line_is_blank) {
                blockquote_lazy_active = 0;
            }
            if (content_len > 0 &&
                in_list_context &&
                leading_spaces < list_indent &&
                !deck_line_opens_list_item(line.buf, content_len, &opened_list_indent)) {
                in_list_context = 0;
                list_indent = 0;
            }
            if (deck_line_is_fence_open(line.buf, content_len, &marker, &marker_len)) {
                in_fence = 1;
                fence_marker = marker;
                fence_len = marker_len;
            } else if (!blockquote_lazy_active &&
                       deck_line_is_separator(line.buf, content_len, in_list_context, list_indent)) {
                if (deck_render_write_slide(self,
                                            &html_renderer,
                                            &html_opts,
                                            sink,
                                            &slide_markdown,
                                            &shell_open,
                                            &slide_index) != 0) {
                    status = strcmp(self->error(self), "out of memory") == 0 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
                    goto done;
                }
                in_list_context = 0;
                list_indent = 0;
                continue;
            } else if (opened_list_indent > 0 ||
                       deck_line_opens_list_item(line.buf, content_len, &opened_list_indent)) {
                in_list_context = 1;
                list_indent = opened_list_indent;
            }
            if (!line_is_blank && (line_starts_blockquote || blockquote_lazy_active)) {
                blockquote_lazy_active = 1;
            }
        }
        if (deck_string_append(&slide_markdown, line.buf, line.len) != 0) {
            mdf_set_error(self, "out of memory");
            status = MDF_ERROR_NOMEM;
            goto done;
        }
    }
    if (pending_front_matter && front_matter.len > 0) {
        if (deck_render_write_slide(self,
                                    &html_renderer,
                                    &html_opts,
                                    sink,
                                    &slide_markdown,
                                    &shell_open,
                                    &slide_index) != 0) {
            status = strcmp(self->error(self), "out of memory") == 0 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
            goto done;
        }
        if (deck_replay_pending_front_matter_body(self, &front_matter, &slide_markdown) != 0) {
            status = MDF_ERROR_NOMEM;
            goto done;
        }
        pending_front_matter = 0;
        in_front_matter = 0;
        front_matter_has_key = 0;
    }
    if (deck_render_write_slide(self,
                                &html_renderer,
                                &html_opts,
                                sink,
                                &slide_markdown,
                                &shell_open,
                                &slide_index) != 0) {
        status = strcmp(self->error(self), "out of memory") == 0 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
        goto done;
    }
    if (slide_index == 0) {
        if (deck_render_write_slide(self,
                                    &html_renderer,
                                    &html_opts,
                                    sink,
                                    &slide_markdown,
                                    &shell_open,
                                    &slide_index) != 0) {
            status = strcmp(self->error(self), "out of memory") == 0 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
            goto done;
        }
    }
    if (deck_write_shell_end(sink) != 0) {
        mdf_set_error(self, "sink write failed");
        status = MDF_ERROR_IO;
        goto done;
    }

done:
    if (html_renderer != NULL) {
        html_renderer->destroy(html_renderer);
    }
    mdf_free_mem(&impl->allocator, front_theme, front_theme_cap);
    deck_string_dispose(&front_matter);
    deck_string_dispose(&line);
    deck_string_dispose(&slide_markdown);
    return status;
}

static mdf_status instance_render(mdf_renderer *self, mdf_source *source, mdf_sink *sink)
{
    mdf_parser *parser;
    mdf_impl *impl;
    mdf_status st;
    const char *parser_err;
    auto_title_source title_source_data;
    mdf_source title_source;

    impl = (mdf_impl *)self->impl;
    if (source == NULL || sink == NULL || source->read == NULL || sink->write == NULL) {
        mdf_set_error(self, "render requires instance, source, and sink");
        return MDF_ERROR_INVALID;
    }
    mdf_renderer_reset_session_state(self);
    memset(&title_source_data, 0, sizeof(title_source_data));
    title_source_data.source = source;
    title_source_data.prefix.allocator = &impl->allocator;
    if ((impl->format == MDF_FORMAT_HTML || impl->format == MDF_FORMAT_HTML_DECK) &&
        impl->html_title == NULL) {
        if (auto_title_detect_from_source(impl, &title_source_data) != 0) {
            deck_string_dispose(&title_source_data.prefix);
            mdf_set_error(self, title_source_data.err == -1 ? "out of memory" : "source read failed");
            return title_source_data.err == -1 ? MDF_ERROR_NOMEM : MDF_ERROR_IO;
        }
        title_source.userdata = &title_source_data;
        title_source.read = auto_title_source_read;
        source = &title_source;
    }
    if (impl->format == MDF_FORMAT_HTML_DECK) {
        st = deck_render(self, source, sink);
        deck_string_dispose(&title_source_data.prefix);
        return st;
    }
    parser = NULL;
    st = mdf_parser_create(&impl->opts, &parser);
    if (st != MDF_OK) {
        deck_string_dispose(&title_source_data.prefix);
        if (st == MDF_ERROR_NOMEM) {
            mdf_set_error(self, "out of memory");
        } else {
            mdf_set_error(self, "parser creation failed");
        }
        return st;
    }
    st = self->write_token == NULL || self->finish == NULL ? MDF_ERROR_INVALID : mdf_renderer_begin_internal(self, sink);
    if (st != MDF_OK) {
        parser->destroy(parser);
        deck_string_dispose(&title_source_data.prefix);
        return st;
    }
    st = parser->parse(parser, source, self, sink);
    if (st != MDF_OK) {
        parser_err = parser->error(parser);
        if (parser_err != NULL && parser_err[0] != '\0') {
            mdf_set_error(self, parser_err);
        }
    }
    parser->destroy(parser);
    deck_string_dispose(&title_source_data.prefix);
    return st;
}

typedef struct mem_sink {
    mdf_allocator *allocator;
    char *buf;
    size_t len;
    size_t cap;
    int oom;
} mem_sink;

static void mem_sink_init(mem_sink *sink, mdf_allocator *allocator)
{
    memset(sink, 0, sizeof(*sink));
    sink->allocator = allocator;
}

static void mem_sink_dispose(mem_sink *sink)
{
    mdf_free_mem(sink->allocator, sink->buf, sink->cap);
    sink->buf = NULL;
    sink->len = 0;
    sink->cap = 0;
    sink->oom = 0;
}

static mdf_status mem_sink_finalize_string(mdf_renderer *self, mem_sink *sink, char **out)
{
    if (sink->buf == NULL) {
        sink->buf = (char *)mdf_alloc(sink->allocator, 1);
        if (sink->buf == NULL) {
            mdf_set_error(self, "out of memory");
            return MDF_ERROR_NOMEM;
        }
        sink->buf[0] = '\0';
        sink->cap = 1;
    } else if (sink->cap != sink->len + 1) {
        char *exact;

        exact = (char *)mdf_realloc_mem(sink->allocator, sink->buf, sink->cap, sink->len + 1);
        if (exact == NULL) {
            mdf_free_mem(sink->allocator, sink->buf, sink->cap);
            sink->buf = NULL;
            sink->len = 0;
            sink->cap = 0;
            mdf_set_error(self, "out of memory");
            return MDF_ERROR_NOMEM;
        }
        sink->buf = exact;
        sink->cap = sink->len + 1;
    }
    *out = sink->buf;
    return MDF_OK;
}

static int mem_write(void *userdata, const char *src, size_t len)
{
    mem_sink *m;
    char *next;
    size_t cap;

    m = (mem_sink *)userdata;
    if (len == 0) {
        return 0;
    }
    if (m->len + len + 1 < m->len) {
        return -1;
    }
    if (m->len + len + 1 > m->cap) {
        cap = m->cap == 0 ? 256 : m->cap;
        while (cap < m->len + len + 1) {
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(m->allocator, m->buf, m->cap, cap);
        if (next == NULL) {
            m->oom = 1;
            return -1;
        }
        m->buf = next;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, src, len);
    m->len += len;
    m->buf[m->len] = '\0';
    return 0;
}

typedef struct cstr_source {
    const char *p;
    size_t len;
    size_t off;
} cstr_source;

static void cstr_source_init(cstr_source *src, const char *markdown)
{
    src->p = markdown;
    src->len = strlen(markdown);
    src->off = 0;
}

static size_t cstr_read(void *userdata, char *dst, size_t cap, int *err)
{
    cstr_source *s;
    size_t n;

    (void)err;
    s = (cstr_source *)userdata;
    if (s->off >= s->len) {
        return 0;
    }
    n = s->len - s->off;
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, s->p + s->off, n);
    s->off += n;
    return n;
}

static mdf_status instance_render_cstr(mdf_renderer *self, const char *markdown, char **out)
{
    cstr_source src_data;
    mdf_source src;
    mem_sink dst_data;
    mdf_sink dst;
    mdf_status st;

    if (out != NULL) {
        *out = NULL;
    }
    if (self == NULL || markdown == NULL || out == NULL) {
        mdf_set_error(self, "render_cstr requires instance, markdown, and out");
        return MDF_ERROR_INVALID;
    }
    cstr_source_init(&src_data, markdown);
    src.userdata = &src_data;
    src.read = cstr_read;
    mem_sink_init(&dst_data, &((mdf_impl *)self->impl)->user_allocator);
    dst.userdata = &dst_data;
    dst.write = mem_write;
    st = instance_render(self, &src, &dst);
    if (st != MDF_OK) {
        int oom;

        oom = dst_data.oom;
        mem_sink_dispose(&dst_data);
        if (oom) {
            mdf_set_error(self, "out of memory");
            return MDF_ERROR_NOMEM;
        }
        return st;
    }
    return mem_sink_finalize_string(self, &dst_data, out);
}

static mdf_status parser_parse(mdf_parser *self, mdf_source *source, mdf_renderer *renderer, mdf_sink *sink)
{
    return mdf_parse_stream(self, source, renderer, sink);
}

static const char *parser_error_method(const mdf_parser *self)
{
    return mdf_parser_error(self);
}

static void parser_destroy_method(mdf_parser *self)
{
    mdf_parser_destroy(self);
}

static mdf_status mdf_alloc_pair_zeroed(mdf_allocator *allocator,
                                        size_t outer_size,
                                        void **outer,
                                        size_t inner_size,
                                        void **inner)
{
    void *outer_ptr;
    void *inner_ptr;

    *outer = NULL;
    *inner = NULL;
    outer_ptr = mdf_alloc(allocator, outer_size);
    inner_ptr = mdf_alloc(allocator, inner_size);
    if (outer_ptr == NULL || inner_ptr == NULL) {
        mdf_free_mem(allocator, outer_ptr, outer_size);
        mdf_free_mem(allocator, inner_ptr, inner_size);
        return MDF_ERROR_NOMEM;
    }
    memset(outer_ptr, 0, outer_size);
    memset(inner_ptr, 0, inner_size);
    *outer = outer_ptr;
    *inner = inner_ptr;
    return MDF_OK;
}

static void mdf_parser_bind_methods(mdf_parser *parser)
{
    parser->parse = parser_parse;
    parser->error = parser_error_method;
    parser->destroy = parser_destroy_method;
}

static mdf_status mdf_method_render(mdf *self, mdf_source *source, mdf_sink *sink);
static mdf_status mdf_method_render_cstr(mdf *self, const char *markdown, char **out);
static const char *mdf_method_error(const mdf *self);
static void mdf_method_destroy(mdf *self);
static void mdf_method_string_free(mdf *self, char *s);

static void mdf_bind_receiver_methods(mdf *inst)
{
    inst->write_token = mdf_renderer_write_token_internal;
    inst->finish = mdf_renderer_finish_internal;
    inst->render = mdf_method_render;
    inst->render_cstr = mdf_method_render_cstr;
    inst->error = mdf_method_error;
    inst->destroy = mdf_method_destroy;
    inst->string_free = mdf_method_string_free;
}

static mdf_status mdf_impl_configure_emit_buffer(mdf_impl *impl)
{
    mdf_emission_buffer cfg;
    size_t initial_cap;
    size_t max_cap;

    cfg = impl->opts.emission_buffer;
    impl->emit_len = 0;
    max_cap = cfg.max_cap == 0 ? 8192u : cfg.max_cap;
    initial_cap = cfg.initial_cap == 0 ? 128u : cfg.initial_cap;
    if (cfg.data != NULL && cfg.cap > 0) {
        impl->emit_buf = (char *)cfg.data;
        impl->emit_cap = cfg.cap;
        impl->emit_max_cap = max_cap;
        impl->emit_fixed_mode = cfg.fixed ? 1 : 0;
        impl->emit_using_heap = cfg.take_ownership ? 1 : 0;
        impl->emit_owns_buf = cfg.take_ownership ? 1 : 0;
        impl->emit_workspace_owned = 0;
        return MDF_OK;
    }
    if (initial_cap > max_cap) {
        initial_cap = max_cap;
    }
    impl->emit_buf = (char *)mdf_alloc(&impl->allocator, initial_cap);
    if (impl->emit_buf == NULL) {
        return MDF_ERROR_NOMEM;
    }
    impl->emit_cap = initial_cap;
    impl->emit_max_cap = max_cap;
    impl->emit_fixed_mode = 0;
    impl->emit_using_heap = 1;
    impl->emit_owns_buf = 1;
    impl->emit_workspace_owned = 1;
    return MDF_OK;
}

static void mdf_impl_ensure_emit_buffer_initialized(mdf_impl *impl)
{
    if (impl->emit_buf == NULL) {
        impl->emit_buf = impl->emit_fixed;
        impl->emit_cap = sizeof(impl->emit_fixed);
        impl->emit_max_cap = sizeof(impl->emit_fixed);
        impl->emit_fixed_mode = 1;
        impl->emit_using_heap = 0;
        impl->emit_owns_buf = 0;
    }
}

static void mdf_impl_reset_emit_buffer(mdf_impl *impl)
{
    impl->emit_len = 0;
    mdf_impl_ensure_emit_buffer_initialized(impl);
}

static void mdf_impl_release_heap_state(mdf_impl *impl)
{
    mdf_allocator *allocator;

    allocator = &impl->allocator;
    mdf_render_release_state(impl);
    mdf_free_mem(allocator, impl->inline_text, impl->inline_text_cap);
    mdf_free_mem(allocator, impl->inline_url, impl->inline_url_cap);
    mdf_free_mem(allocator, impl->pending_fallback_url, impl->pending_fallback_url_cap);
    mdf_free_mem(allocator, impl->inline_code, impl->inline_code_cap);
    mdf_free_mem(allocator, impl->ansi_pending_emit, impl->ansi_pending_emit_cap);
    mdf_free_mem(allocator,
                 impl->ansi_pending_emit_offsets,
                 impl->ansi_pending_emit_offsets_cap * sizeof(impl->ansi_pending_emit_offsets[0]));
    mdf_free_mem(allocator, impl->inline_emph, impl->inline_emph_cap);
    mdf_free_mem(allocator, impl->inline_entity, impl->inline_entity_cap);
    mdf_free_mem(allocator, impl->ansi_word, impl->ansi_word_cap);
    mdf_free_mem(allocator, impl->pre_code_buf, impl->pre_code_cap);
    mdf_free_mem(allocator, impl->html_title, impl->html_title_cap);
    mdf_free_mem(allocator, impl->html_font_regular_uri, impl->html_font_regular_uri_cap);
    mdf_free_mem(allocator, impl->html_font_italic_uri, impl->html_font_italic_uri_cap);
    if (impl->emit_owns_buf && impl->emit_buf != NULL && impl->emit_workspace_owned) {
        mdf_free_mem(allocator, impl->emit_buf, impl->emit_cap);
    } else if (impl->emit_owns_buf && impl->emit_buf != NULL) {
        mdf_free_mem(&impl->user_allocator, impl->emit_buf, impl->emit_cap);
    }
}

static mdf_allocator mdf_string_allocator(const mdf *self)
{
    mdf_allocator allocator;

    if (self != NULL && self->impl != NULL) {
        return ((const mdf_impl *)self->impl)->user_allocator;
    }
    memset(&allocator, 0, sizeof(allocator));
    mdf_allocator_normalize(&allocator);
    return allocator;
}

static void mdf_options_resolve(const mdf_options *opts, mdf_options *resolved)
{
    int default_width;
    double default_html_content_width_ch;

    mdf_options_init(resolved);
    default_width = resolved->width;
    default_html_content_width_ch = resolved->html_content_width_ch;
    if (opts != NULL) {
        *resolved = *opts;
        if (resolved->allocator.realloc == default_realloc &&
            ((resolved->allocator.alloc != NULL && resolved->allocator.alloc != default_alloc) ||
             (resolved->allocator.free != NULL && resolved->allocator.free != default_free))) {
            resolved->allocator.realloc = NULL;
        }
        mdf_allocator_normalize(&resolved->allocator);
    }
    if (resolved->width <= 0) {
        resolved->width = default_width;
    }
    if (resolved->html_content_width_ch <= 0.0) {
        resolved->html_content_width_ch = default_html_content_width_ch;
    }
}

void mdf_options_init(mdf_options *opts)
{
    if (opts == NULL) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->width = 80;
    opts->theme_name = NULL;
    opts->html_content_width_ch = 96.0;
    opts->deck_transition = MDF_DECK_TRANSITION_FADE;
    opts->table_buffer_mode = MDF_TABLE_BUFFER_FULL;
    opts->table_wire_mode = MDF_TABLE_WIRE_LINE;
    mdf_allocator_normalize(&opts->allocator);
}

mdf_status mdf_parser_create(const mdf_options *opts, mdf_parser **out)
{
    mdf_parser *p;
    mdf_parser_impl *impl;
    mdf_options defaults;
    const mdf_theme_style *theme;
    mdf_status st;

    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL) {
        return MDF_ERROR_INVALID;
    }
    mdf_options_resolve(opts, &defaults);
    theme = mdf_theme_resolve(defaults.theme_name);
    if (theme == NULL) {
        return MDF_ERROR_INVALID;
    }
    defaults.theme_name = theme->name;
    st = mdf_alloc_pair_zeroed(&defaults.allocator,
                               sizeof(*p),
                               (void **)&p,
                               sizeof(*impl),
                               (void **)&impl);
    if (st != MDF_OK) {
        return st;
    }
    impl->opts = defaults;
    impl->allocator = defaults.allocator;
    impl->at_line_start = 1;
    mdf_parser_bind_methods(p);
    p->impl = impl;
    *out = p;
    return MDF_OK;
}

mdf_status mdf_parser_parse(mdf_parser *self, mdf_source *source, mdf_renderer *renderer, mdf_sink *sink)
{
    if (self == NULL || self->parse == NULL) {
        return MDF_ERROR_INVALID;
    }
    return self->parse(self, source, renderer, sink);
}

const char *mdf_parser_error(const mdf_parser *self)
{
    const mdf_parser_impl *impl;

    if (self == NULL || self->impl == NULL) {
        return "invalid parser";
    }
    impl = (const mdf_parser_impl *)self->impl;
    if (impl->error[0] == '\0') {
        return "";
    }
    return impl->error;
}

void mdf_parser_destroy(mdf_parser *self)
{
    mdf_allocator allocator;
    mdf_parser_impl *impl;

    if (self == NULL) {
        return;
    }
    if (self->impl == NULL) {
        memset(&allocator, 0, sizeof(allocator));
        mdf_allocator_normalize(&allocator);
        mdf_free_mem(&allocator, self, sizeof(*self));
        return;
    }
    impl = (mdf_parser_impl *)self->impl;
    allocator = impl->allocator;
    mdf_free_mem(&allocator, impl->chart_buf, impl->chart_cap);
    mdf_free_mem(&allocator, impl, sizeof(*impl));
    mdf_free_mem(&allocator, self, sizeof(*self));
}

mdf_status mdf_create(mdf_format format, const mdf_options *opts, mdf **out)
{
    mdf *r;
    mdf_impl *impl;
    mdf_options defaults;
    const mdf_theme_style *theme;
    mdf_status st;
    int theme_name_explicit;

    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL) {
        return MDF_ERROR_INVALID;
    }
    if (format != MDF_FORMAT_ANSI && format != MDF_FORMAT_HTML && format != MDF_FORMAT_HTML_DECK) {
        return MDF_ERROR_INVALID;
    }
    theme_name_explicit = opts != NULL && opts->theme_name != NULL;
    mdf_options_resolve(opts, &defaults);
    if (defaults.margin_left < 0 || defaults.margin_right < 0) {
        return MDF_ERROR_INVALID;
    }
    if (defaults.deck_transition != MDF_DECK_TRANSITION_FADE &&
        defaults.deck_transition != MDF_DECK_TRANSITION_CROSS &&
        defaults.deck_transition != MDF_DECK_TRANSITION_HARD) {
        return MDF_ERROR_INVALID;
    }
    if (format == MDF_FORMAT_ANSI &&
        defaults.width > 0 &&
        defaults.width - defaults.margin_left - defaults.margin_right < MDF_MIN_ANSI_CONTENT_WIDTH) {
        return MDF_ERROR_INVALID;
    }
    theme = mdf_theme_resolve(defaults.theme_name);
    if (theme == NULL) {
        return MDF_ERROR_INVALID;
    }
    defaults.theme_name = theme->name;
    st = mdf_alloc_pair_zeroed(&defaults.allocator,
                               sizeof(*r),
                               (void **)&r,
                               sizeof(*impl),
                               (void **)&impl);
    if (st != MDF_OK) {
        return st;
    }
    impl->format = format;
    impl->theme_name_explicit = theme_name_explicit;
    impl->user_allocator = defaults.allocator;
    mdf_memory_init(&impl->memory, &impl->user_allocator, &defaults.memory);
    impl->allocator = mdf_memory_allocator(&impl->memory);
    defaults.allocator = impl->allocator;
    impl->opts = defaults;
    impl->theme = theme;
    if (impl->opts.width <= 0) {
        impl->opts.width = defaults.width;
    }
    if (format == MDF_FORMAT_ANSI && impl->opts.margin_right > 0) {
        impl->opts.width -= impl->opts.margin_right;
    }
    if (impl->opts.html_content_width_ch <= 0.0) {
        impl->opts.html_content_width_ch = defaults.html_content_width_ch;
    }
    if ((format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) &&
        impl->opts.html_font.family == NULL &&
        impl->opts.html_font.regular.format == MDF_HTML_FONT_FORMAT_NONE) {
        mdf_html_jetbrains_mono_font(&impl->opts.html_font);
    }
    if (format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) {
        st = mdf_configure_html_font(impl);
        if (st != MDF_OK) {
            mdf_allocator user_allocator;

            user_allocator = impl->user_allocator;
            mdf_impl_release_heap_state(impl);
            mdf_memory_destroy(&impl->memory);
            mdf_free_mem(&user_allocator, impl, sizeof(*impl));
            mdf_free_mem(&user_allocator, r, sizeof(*r));
            return st;
        }
    }
    st = mdf_impl_configure_emit_buffer(impl);
    if (st != MDF_OK) {
        mdf_allocator user_allocator;

        user_allocator = impl->user_allocator;
        mdf_impl_release_heap_state(impl);
        mdf_memory_destroy(&impl->memory);
        mdf_free_mem(&user_allocator, impl, sizeof(*impl));
        mdf_free_mem(&user_allocator, r, sizeof(*r));
        return st;
    }
    mdf_bind_receiver_methods(r);
    r->impl = impl;
    *out = r;
    return MDF_OK;
}

mdf_status mdf_set_html_title(mdf *self, const char *title)
{
    mdf_impl *impl;
    char *next;
    size_t len;
    size_t cap;

    if (self == NULL || self->impl == NULL) {
        return MDF_ERROR_INVALID;
    }
    impl = (mdf_impl *)self->impl;
    if (impl->format != MDF_FORMAT_HTML && impl->format != MDF_FORMAT_HTML_DECK) {
        return MDF_ERROR_INVALID;
    }
    if (title == NULL) {
        mdf_free_mem(&impl->allocator, impl->html_title, impl->html_title_cap);
        impl->html_title = NULL;
        impl->html_title_cap = 0;
        impl->html_title_auto = 0;
        return MDF_OK;
    }
    len = strlen(title);
    if (len + 1 < len) {
        return MDF_ERROR_NOMEM;
    }
    cap = len + 1;
    next = (char *)mdf_realloc_mem(&impl->allocator, NULL, 0, cap);
    if (next == NULL) {
        return MDF_ERROR_NOMEM;
    }
    memcpy(next, title, len + 1);
    mdf_free_mem(&impl->allocator, impl->html_title, impl->html_title_cap);
    impl->html_title = next;
    impl->html_title_cap = cap;
    impl->html_title_auto = 0;
    return MDF_OK;
}

static mdf_status mdf_method_render(mdf *self, mdf_source *source, mdf_sink *sink)
{
    if (self == NULL) {
        return MDF_ERROR_INVALID;
    }
    return instance_render(self, source, sink);
}

static mdf_status mdf_method_render_cstr(mdf *self, const char *markdown, char **out)
{
    if (self == NULL) {
        if (out != NULL) {
            *out = NULL;
        }
        return MDF_ERROR_INVALID;
    }
    return instance_render_cstr(self, markdown, out);
}

static const char *mdf_method_error(const mdf *self)
{
    const mdf_impl *impl;

    if (self == NULL || self->impl == NULL) {
        return "invalid instance";
    }
    impl = (const mdf_impl *)self->impl;
    if (impl->error[0] == '\0') {
        return "";
    }
    return impl->error;
}

static void mdf_method_destroy(mdf *self)
{
    mdf_allocator allocator;
    mdf_impl *impl;

    if (self == NULL) {
        return;
    }
    if (self->impl == NULL) {
        return;
    }
    impl = (mdf_impl *)self->impl;
    allocator = impl->allocator;
    mdf_impl_release_heap_state(impl);
    mdf_memory_destroy(&impl->memory);
    allocator = impl->user_allocator;
    mdf_free_mem(&allocator, impl, sizeof(*impl));
    mdf_free_mem(&allocator, self, sizeof(*self));
}

static void mdf_method_string_free(mdf *self, char *s)
{
    mdf_allocator allocator;

    if (s == NULL) {
        return;
    }
    allocator = mdf_string_allocator(self);
    mdf_free_mem(&allocator, s, strlen(s) + 1);
}

const char *mdf_status_string(mdf_status status)
{
    switch (status) {
    case MDF_OK:
        return "ok";
    case MDF_ERROR_INVALID:
        return "invalid argument";
    case MDF_ERROR_NOMEM:
        return "out of memory";
    case MDF_ERROR_IO:
        return "io error";
    case MDF_ERROR_PARSE:
        return "parse error";
    }
    return "unknown status";
}

size_t mdf_theme_count(void)
{
    return sizeof(builtin_theme_styles) / sizeof(builtin_theme_styles[0]);
}

const char *mdf_theme_name(size_t index)
{
    if (index >= mdf_theme_count()) {
        return NULL;
    }
    return builtin_theme_styles[index].name;
}

static int ascii_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static int str_case_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int str_contains_case(const char *haystack, const char *needle)
{
    size_t nlen;
    size_t i;
    size_t j;

    if (haystack == NULL || needle == NULL) {
        return 0;
    }
    nlen = strlen(needle);
    if (nlen == 0) {
        return 1;
    }
    for (i = 0; haystack[i] != '\0'; i++) {
        for (j = 0; j < nlen; j++) {
            if (haystack[i + j] == '\0') {
                return 0;
            }
            if (ascii_lower((unsigned char)haystack[i + j]) != ascii_lower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

int mdf_theme_exists(const char *name)
{
    return mdf_theme_resolve(name) != NULL;
}

const mdf_theme_style *mdf_theme_resolve(const char *name)
{
    size_t i;

    if (name == NULL || *name == '\0') {
        name = "default";
    }
    for (i = 0; i < mdf_theme_count(); i++) {
        if (str_case_equal(name, builtin_theme_styles[i].name)) {
            return &builtin_theme_styles[i];
        }
    }
    return NULL;
}

int mdf_detect_osc8_support(void)
{
    const char *value;
    long vte;
    char *end;

    value = getenv("OSC8");
    if (value != NULL && strcmp(value, "0") == 0) {
        return 0;
    }
    if (getenv("DOMTERM") != NULL && getenv("DOMTERM")[0] != '\0') {
        return 1;
    }
    if (getenv("WT_SESSION") != NULL && getenv("WT_SESSION")[0] != '\0') {
        return 1;
    }
    value = getenv("TERM_PROGRAM");
    if (value != NULL && (strcmp(value, "iTerm.app") == 0 || strcmp(value, "WezTerm") == 0 || strcmp(value, "vscode") == 0)) {
        return 1;
    }
    if (str_contains_case(getenv("TERM"), "kitty")) {
        return 1;
    }
    value = getenv("VTE_VERSION");
    if (value != NULL && *value != '\0') {
        vte = strtol(value, &end, 10);
        if (end != value && *end == '\0' && vte >= 5000) {
            return 1;
        }
    }
    return 0;
}

int mdf_terminal_width(int fd, int fallback)
{
    struct winsize ws;
    const char *columns;
    long value;
    char *end;

    if (fallback <= 0) {
        fallback = 80;
    }
    if (isatty(fd)) {
        memset(&ws, 0, sizeof(ws));
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            return (int)ws.ws_col;
        }
    }
    columns = getenv("COLUMNS");
    if (columns != NULL && *columns != '\0') {
        value = strtol(columns, &end, 10);
        if (end != columns && *end == '\0' && value > 0 && value <= 10000) {
            return (int)value;
        }
    }
    return fallback;
}

static void mdf_copy_error_message(char *dst, size_t cap, const char *msg)
{
    if (msg == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, msg, cap - 1);
    dst[cap - 1] = '\0';
}

void mdf_set_error(mdf_renderer *self, const char *msg)
{
    mdf_impl *impl;

    if (self == NULL || self->impl == NULL) {
        return;
    }
    impl = (mdf_impl *)self->impl;
    mdf_copy_error_message(impl->error, sizeof(impl->error), msg);
}

void mdf_parser_set_error(mdf_parser *self, const char *msg)
{
    mdf_parser_impl *impl;

    if (self == NULL || self->impl == NULL) {
        return;
    }
    impl = (mdf_parser_impl *)self->impl;
    mdf_copy_error_message(impl->error, sizeof(impl->error), msg);
}

int mdf_write_all(mdf_sink *sink, const char *src, size_t len)
{
    if (sink == NULL || sink->write == NULL || (src == NULL && len != 0)) {
        return -1;
    }
    return sink->write(sink->userdata, src, len);
}

int mdf_write_cstr(mdf_sink *sink, const char *src)
{
    if (src == NULL) {
        return 0;
    }
    return mdf_write_all(sink, src, strlen(src));
}

int mdf_emit_all(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    char *next;
    size_t next_cap;

    if (impl == NULL) {
        return mdf_write_all(sink, src, len);
    }
    if (src == NULL && len != 0) {
        return -1;
    }
    if (len == 0) {
        return mdf_write_all(sink, src, len);
    }
    mdf_impl_ensure_emit_buffer_initialized(impl);
    if (impl->emit_max_cap > 0 && len > impl->emit_max_cap) {
        return -1;
    }
    if (len > impl->emit_cap) {
        if (impl->emit_fixed_mode || (impl->emit_buf != impl->emit_fixed && !impl->emit_owns_buf)) {
            return -1;
        }
        next_cap = impl->emit_cap == 0 ? sizeof(impl->emit_fixed) : impl->emit_cap;
        while (next_cap < len) {
            if (next_cap > ((size_t)-1) / 2) {
                next_cap = len;
                break;
            }
            next_cap *= 2;
            if (impl->emit_max_cap > 0 && next_cap > impl->emit_max_cap) {
                next_cap = impl->emit_max_cap;
            }
        }
        if (next_cap < len) {
            return -1;
        }
        if (impl->emit_using_heap && impl->emit_workspace_owned) {
            next = (char *)mdf_realloc_mem(&impl->allocator, impl->emit_buf, impl->emit_cap, next_cap);
        } else if (impl->emit_using_heap) {
            next = (char *)mdf_realloc_mem(&impl->user_allocator, impl->emit_buf, impl->emit_cap, next_cap);
        } else {
            next = (char *)mdf_alloc(&impl->allocator, next_cap);
            impl->emit_workspace_owned = 1;
        }
        if (next == NULL) {
            mdf_copy_error_message(impl->error, sizeof(impl->error), "out of memory");
            return -1;
        }
        impl->emit_buf = next;
        impl->emit_cap = next_cap;
        impl->emit_using_heap = 1;
        impl->emit_owns_buf = 1;
    }
    memmove(impl->emit_buf, src, len);
    impl->emit_len = len;
    if (mdf_write_all(sink, impl->emit_buf, len) != 0) {
        impl->emit_len = 0;
        return -1;
    }
    if (impl->opts.write_trace.emit != NULL) {
        if (impl->opts.write_trace.emit(impl->opts.write_trace.userdata, impl->format, impl->emit_buf, len) != 0) {
            impl->emit_len = 0;
            return -1;
        }
    }
    impl->emit_len = 0;
    return 0;
}

int mdf_emit_cstr(mdf_impl *impl, mdf_sink *sink, const char *src)
{
    if (src == NULL) {
        return 0;
    }
    return mdf_emit_all(impl, sink, src, strlen(src));
}

int mdf_emit_buffer_reset(mdf_impl *impl)
{
    if (impl == NULL) {
        return -1;
    }
    mdf_impl_ensure_emit_buffer_initialized(impl);
    impl->emit_len = 0;
    return 0;
}

int mdf_emit_buffer_append(mdf_impl *impl, const char *src, size_t len)
{
    size_t need;
    size_t next_cap;
    char *next;

    if (impl == NULL || (src == NULL && len != 0)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (impl->emit_buf == NULL) {
        if (mdf_emit_buffer_reset(impl) != 0) {
            return -1;
        }
    }
    if (len > ((size_t)-1) - impl->emit_len) {
        return -1;
    }
    need = impl->emit_len + len;
    if (impl->emit_max_cap > 0 && need > impl->emit_max_cap) {
        return -1;
    }
    if (need > impl->emit_cap) {
        if (impl->emit_fixed_mode || (impl->emit_buf != impl->emit_fixed && !impl->emit_owns_buf)) {
            return -1;
        }
        next_cap = impl->emit_cap == 0 ? sizeof(impl->emit_fixed) : impl->emit_cap;
        while (next_cap < need) {
            if (next_cap > ((size_t)-1) / 2) {
                next_cap = need;
                break;
            }
            next_cap *= 2;
            if (impl->emit_max_cap > 0 && next_cap > impl->emit_max_cap) {
                next_cap = impl->emit_max_cap;
            }
        }
        if (next_cap < need) {
            return -1;
        }
        if (impl->emit_using_heap && impl->emit_workspace_owned) {
            next = (char *)mdf_realloc_mem(&impl->allocator, impl->emit_buf, impl->emit_cap, next_cap);
        } else if (impl->emit_using_heap) {
            next = (char *)mdf_realloc_mem(&impl->user_allocator, impl->emit_buf, impl->emit_cap, next_cap);
        } else {
            next = (char *)mdf_alloc(&impl->allocator, next_cap);
            if (next != NULL && impl->emit_len > 0) {
                memcpy(next, impl->emit_buf, impl->emit_len);
            }
            impl->emit_workspace_owned = 1;
        }
        if (next == NULL) {
            mdf_copy_error_message(impl->error, sizeof(impl->error), "out of memory");
            return -1;
        }
        impl->emit_buf = next;
        impl->emit_cap = next_cap;
        impl->emit_using_heap = 1;
        impl->emit_owns_buf = 1;
    }
    memcpy(impl->emit_buf + impl->emit_len, src, len);
    impl->emit_len += len;
    return 0;
}

int mdf_emit_buffer_append_cstr(mdf_impl *impl, const char *src)
{
    if (src == NULL) {
        return 0;
    }
    return mdf_emit_buffer_append(impl, src, strlen(src));
}

int mdf_emit_buffer_commit(mdf_impl *impl, mdf_sink *sink)
{
    size_t len;
    int rc;

    if (impl == NULL || impl->emit_buf == NULL) {
        return -1;
    }
    len = impl->emit_len;
    if (len == 0) {
        return 0;
    }
    rc = mdf_emit_all(impl, sink, impl->emit_buf, len);
    impl->emit_len = 0;
    return rc;
}
