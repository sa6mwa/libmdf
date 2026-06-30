#include "render_internal.h"

int ansi_inline_append(mdf_impl *impl, char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    char *next;
    size_t new_cap;
    size_t need;

    if (n > ((size_t)-1) - *len - 1) {
        return -1;
    }
    need = *len + n + 1;
    if (need > *cap) {
        new_cap = *cap == 0 ? 64 : *cap;
        while (new_cap < need) {
            new_cap *= 2;
        }
        next = (char *)mdf_realloc_mem(&impl->allocator, *buf, *cap, new_cap);
        if (next == NULL) {
            mdf_impl_mark_oom(impl);
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


int ansi_flush_word(mdf_impl *impl, mdf_sink *sink);
int ansi_inline_emit_streamed_emphasis_word(mdf_impl *impl, mdf_sink *sink, int closing);
static const char *ansi_inline_emphasis_prefix(mdf_impl *impl, int continuation, char *buf, size_t buf_cap);
static int ansi_inline_emit_pending_emphasis(mdf_impl *impl, mdf_sink *sink, size_t reserve_cols);
static int ansi_inline_flush_unmatched_nested_emphasis(mdf_impl *impl, mdf_sink *sink);
static int ansi_inline_handle_streamed_emphasis_delim(mdf_impl *impl, mdf_sink *sink);
static int ansi_inline_emit_streamed_emphasis_literal_char(mdf_impl *impl, mdf_sink *sink, char c);
static const char *ansi_inline_nested_emphasis_style(mdf_impl *impl, int nested_count, char *buf, size_t buf_cap);
static int autolink_likely(const char *s, size_t len);
static int autolink_is_email(const char *s, size_t len);
static int markdown_escapable_char(char c);
static size_t visible_cols(const char *s, size_t len);
static int utf8_continuation_byte(char c);
size_t utf8_decode_codepoint(const char *s, size_t len, unsigned long *cp);
size_t utf8_display_width(unsigned long cp);
static int ansi_emit_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_write_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_ensure_left_margin(mdf_impl *impl, mdf_sink *sink);
static int ansi_write_word_byte(mdf_impl *impl, mdf_sink *sink, char c);
int ansi_emit_visible_chunk(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len);
static int ansi_write_direct_visible(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len);
static int ansi_write_code_segment(mdf_impl *impl, mdf_sink *sink, const char *segment, size_t len, int limit);
static int ansi_write_code_segment_with_prefix(mdf_impl *impl, mdf_sink *sink, const char *prefix, const char *segment, size_t len, int limit);
static int ansi_write_inline_code_segment_with_prefix(mdf_impl *impl, mdf_sink *sink, const char *prefix, const char *segment, size_t len, int limit);
static int ansi_current_prefix_width(mdf_impl *impl);
static int ansi_write_plain_code_wrapped(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len);
static int ansi_word_has_url_scheme(const char *s, size_t len);
static void ansi_pre_code_reset_line_state(mdf_impl *impl);
static int ansi_pre_code_wrap_prefix_len(mdf_impl *impl);
static int ansi_pre_code_space_starts_wrap_prefix(mdf_impl *impl);
static int ansi_pre_code_line_has_only_whitespace_prefix(mdf_impl *impl, int prefix_width);
static void ansi_pre_code_note_segment(mdf_impl *impl, const char *segment, size_t len);
static int ansi_pre_code_prepare_segment(mdf_impl *impl, mdf_sink *sink, const char *segment, size_t len, const char **prefix);

static const char *ansi_store_owned_inline_style(mdf_impl *impl, const char *style)
{
    size_t style_len;

    if (style == NULL) {
        impl->ansi_owned_inline_style[0] = '\0';
        return NULL;
    }
    style_len = strlen(style);
    if (style_len >= sizeof(impl->ansi_owned_inline_style)) {
        mdf_impl_mark_oom(impl);
        return NULL;
    }
    memcpy(impl->ansi_owned_inline_style, style, style_len + 1);
    return impl->ansi_owned_inline_style;
}

static int ansi_is_quote_char(char c)
{
    return c == '"' || c == '\'';
}

int ansi_wrap_limit(const mdf_impl *impl)
{
    if (impl == NULL || impl->opts.width <= 0) {
        return 0;
    }
    return impl->opts.width;
}

int ansi_content_limit(mdf_impl *impl)
{
    int limit;
    int prefix;

    limit = ansi_wrap_limit(impl);
    if (limit <= 0) {
        return 0;
    }
    if (impl->opts.margin_left == 0 && impl->opts.margin_right == 0) {
        return limit;
    }
    prefix = ansi_current_prefix_width(impl);
    if (prefix < impl->opts.margin_left) {
        prefix = impl->opts.margin_left;
    }
    if (prefix >= limit) {
        return 1;
    }
    return limit - prefix;
}

static int ansi_is_ordered_list_marker(const char *s, size_t len)
{
    size_t i;

    if (len < 2) {
        return 0;
    }
    for (i = 0; i + 1 < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return s[len - 1] == '.' || s[len - 1] == ')';
}

static int ansi_emphasis_suffix_trailing_only(const char *s, size_t len)
{
    char c;

    if (len == 0) {
        return 0;
    }
    c = s[0];
    switch (c) {
    case '.':
    case ',':
        return 1;
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
        return 1;
    default:
        return 0;
    }
}

static int ansi_text_contains_space(const char *s, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            return 1;
        }
    }
    return 0;
}

static int ansi_word_has_em_dash_prefix(const char *s, size_t len)
{
    return len >= 3 &&
           (unsigned char)s[0] == 0xe2 &&
           (unsigned char)s[1] == 0x80 &&
           (unsigned char)s[2] == 0x94;
}

static int ansi_is_quote_codepoint(unsigned long cp)
{
    return cp == '"' || cp == '\'' ||
           cp == 0x2018 || cp == 0x2019 ||
           cp == 0x201c || cp == 0x201d;
}

static int ansi_is_punct_boundary_char(char c, char next)
{
    if (ansi_is_quote_char(c)) {
        return 0;
    }
    if (c == '.') {
        return next >= 'A' && next <= 'Z';
    }
    if (c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
        return !ansi_is_quote_char(next);
    }
    return 0;
}

static int ansi_is_closing_punct_char(char c)
{
    return c == ')' || c == ']' || c == '}';
}

static int ansi_word_is_opening_punct_only(const char *s, size_t len)
{
    size_t i;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        switch (s[i]) {
        case '(':
        case '[':
        case '{':
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int ansi_word_has_break_before_cols(const char *s, size_t len, int limit)
{
    size_t i;
    int cols;

    if (limit <= 0) {
        return 0;
    }
    cols = 0;
    for (i = 0; i < len; i++) {
        char next;

        if (utf8_continuation_byte(s[i])) {
            continue;
        }
        cols++;
        if (cols > limit) {
            break;
        }
        next = (i + 1 < len) ? s[i + 1] : '\0';
        if (ansi_is_punct_boundary_char(s[i], next) ||
            (((s[i] == '/' && !(i == 1 && s[0] == '/')) || s[i] == '\\' || s[i] == ':' || s[i] == '?' || s[i] == '&') && i > 0)) {
            return 1;
        }
    }
    return 0;
}

static int ansi_word_contains_byte(const char *s, size_t len, char want)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (s[i] == want) {
            return 1;
        }
    }
    return 0;
}

static int ansi_word_has_url_scheme(const char *s, size_t len)
{
    size_t i;
    int has_dot;
    int has_slash;

    has_dot = 0;
    has_slash = 0;

    for (i = 0; i + 2 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            return 1;
        }
        if (s[i] == '.') {
            has_dot = 1;
        }
        if (s[i] == '/') {
            has_slash = 1;
        }
    }
    if (i < len && s[i] == '.') {
        has_dot = 1;
    }
    if (i < len && s[i] == '/') {
        has_slash = 1;
    }
    return has_dot && has_slash;
}

static int ansi_word_ends_with_open_bracket(const char *s, size_t len)
{
    if (len == 0) {
        return 0;
    }
    return s[len - 1] == '(' || s[len - 1] == '[' || s[len - 1] == '{';
}

static size_t ansi_rune_split_offset(const char *s, size_t len, int limit)
{
    size_t i;
    int cols;
    size_t split;
    size_t adv;
    unsigned long cp;
    unsigned long next_cp;
    size_t next_adv;
    size_t last_rune_start;
    unsigned long last_rune_cp;

    if (limit <= 0) {
        return 0;
    }
    cols = 0;
    split = 0;
    i = 0;
    last_rune_start = 0;
    last_rune_cp = 0;
    while (i < len) {
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        if (cols + (int)utf8_display_width(cp) > limit) {
            break;
        }
        last_rune_start = i;
        last_rune_cp = cp;
        cols += (int)utf8_display_width(cp);
        i += adv;
        split = i;
    }
    if (split < len) {
        next_adv = utf8_decode_codepoint(s + split, len - split, &next_cp);
        if (last_rune_start > 0 &&
            (ansi_is_quote_codepoint(last_rune_cp) ||
             (next_adv > 0 && split + next_adv == len && ansi_is_quote_codepoint(next_cp)))) {
            split = last_rune_start;
        }
    }
    return split;
}

static size_t ansi_word_split_offset(const char *s, size_t len, int limit)
{
    size_t i;
    int cols;
    size_t last_break;
    size_t scheme_break;
    size_t split;
    size_t adv;
    unsigned long cp;
    unsigned long next_cp;
    size_t next_adv;
    size_t last_rune_start;
    unsigned long last_rune_cp;

    if (limit <= 0) {
        return 0;
    }
    scheme_break = 0;
    for (i = 0; i + 2 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            scheme_break = i + 1;
            break;
        }
    }
    if (scheme_break > 0 && (int)scheme_break <= limit) {
        return scheme_break;
    }
    cols = 0;
    last_break = 0;
    split = 0;
    i = 0;
    last_rune_start = 0;
    last_rune_cp = 0;
    while (i < len) {
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        if (cols + (int)utf8_display_width(cp) > limit) {
            break;
        }
        last_rune_start = i;
        last_rune_cp = cp;
        cols += (int)utf8_display_width(cp);
        i += adv;
        split = i;
        if (cp == '/' || cp == '\\' || cp == ':' || cp == '?' || cp == '&') {
            last_break = i;
        }
    }
    if (last_break > 0) {
        return last_break;
    }
    if (split < len) {
        next_adv = utf8_decode_codepoint(s + split, len - split, &next_cp);
        if (last_rune_start > 0 &&
            (ansi_is_quote_codepoint(last_rune_cp) ||
             (next_adv > 0 && split + next_adv == len && ansi_is_quote_codepoint(next_cp)))) {
            split = last_rune_start;
        }
    }
    return split;
}

static int ansi_write_split_word_full_width(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t off;
    int url_like;
    int limit;

    limit = ansi_content_limit(impl);
    if (limit <= 0) {
        return ansi_write_direct_visible(impl, sink, src, len);
    }
    url_like = ansi_word_has_url_scheme(src, len) ||
               (len >= 2 && src[0] == '/' && src[1] == '/') ||
               (len >= 4 && src[0] == 'w' && src[1] == 'w' && src[2] == 'w' && src[3] == '.');
    off = 0;
    while (off < len) {
        size_t split;
        size_t rem_len;

        rem_len = len - off;
        if ((int)visible_cols(src + off, rem_len) <= limit) {
            if (url_like) {
                size_t q;

                for (q = off; q < len; q++) {
                    if (src[q] == '?' && q + 1 < len) {
                        if (ansi_emit_visible_chunk(impl, sink, src + off, q - off + 1) != 0) {
                            return -1;
                        }
                        return ansi_emit_visible_chunk(impl, sink, src + q + 1, len - q - 1);
                    }
                }
                return ansi_emit_visible_chunk(impl, sink, src + off, rem_len);
            }
            return ansi_write_direct_visible(impl, sink, src + off, rem_len);
        }
        split = url_like ? ansi_word_split_offset(src + off, rem_len, limit) : 0;
        if (split == 0) {
            split = ansi_rune_split_offset(src + off, rem_len, limit);
        }
        if (split == 0) {
            if (url_like) {
                return ansi_emit_visible_chunk(impl, sink, src + off, rem_len);
            }
            return ansi_write_direct_visible(impl, sink, src + off, rem_len);
        }
        if (url_like) {
            if (ansi_emit_visible_chunk(impl, sink, src + off, split) != 0) {
                return -1;
            }
            off += split;
            if (off < len) {
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            continue;
        }
        {
            int saved_flushing_word;

            saved_flushing_word = impl->ansi_flushing_word;
            impl->ansi_flushing_word = 1;
            if (ansi_write_direct_visible(impl, sink, src + off, split) != 0) {
                impl->ansi_flushing_word = saved_flushing_word;
                return -1;
            }
            impl->ansi_flushing_word = saved_flushing_word;
        }
        off += split;
        if (off < len) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
    }
    return 0;
}

static int ansi_write_styled_split_word_full_width(mdf_impl *impl, mdf_sink *sink, const char *style, const char *src, size_t len)
{
    size_t off;
    int need_style;
    int chunk_cols;
    int limit;

    limit = ansi_content_limit(impl);
    if (limit <= 0) {
        if (ansi_emit_styled_prefix(impl, sink, style) != 0) return -1;
        return ansi_write_direct_visible(impl, sink, src, len);
    }
    off = 0;
    need_style = style != NULL && style[0] != '\0' && !impl->opts.boring;
    chunk_cols = 0;
    while (off < len) {
        unsigned long cp;
        size_t adv;
        int cols;

        adv = utf8_decode_codepoint(src + off, len - off, &cp);
        if (adv == 0) {
            break;
        }
        cols = (int)utf8_display_width(cp);
        if (chunk_cols > 0 && chunk_cols + cols > limit) {
            if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            need_style = style != NULL && style[0] != '\0' && !impl->opts.boring;
            chunk_cols = 0;
        }
        if (need_style) {
            if (ansi_emit_styled_visible_chunk(impl, sink, style, src + off, adv) != 0) return -1;
            need_style = 0;
        } else if (ansi_emit_visible_chunk(impl, sink, src + off, adv) != 0) {
            return -1;
        }
        chunk_cols += cols;
        off += adv;
    }
    return 0;
}

static int ansi_write_split_word_preserve_prefix(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t split;
    size_t rem_len;
    int limit;

    limit = ansi_content_limit(impl);
    if (limit <= 0) {
        return ansi_write_direct_visible(impl, sink, src, len);
    }
    rem_len = len;
    if ((int)visible_cols(src, rem_len) <= limit) {
        size_t q;

        for (q = 0; q < len; q++) {
            if (src[q] == '?' && q + 1 < len) {
                if (ansi_emit_visible_chunk(impl, sink, src, q + 1) != 0) {
                    return -1;
                }
                return ansi_emit_visible_chunk(impl, sink, src + q + 1, len - q - 1);
            }
        }
        return ansi_emit_visible_chunk(impl, sink, src, rem_len);
    }
    split = ansi_rune_split_offset(src, rem_len, limit);
    if (split == 0) {
        return ansi_emit_visible_chunk(impl, sink, src, rem_len);
    }
    {
        int saved_flushing_word;

        saved_flushing_word = impl->ansi_flushing_word;
        impl->ansi_flushing_word = 1;
        if (ansi_write_direct_visible(impl, sink, src, split) != 0) {
            impl->ansi_flushing_word = saved_flushing_word;
            return -1;
        }
        impl->ansi_flushing_word = saved_flushing_word;
    }
    if (split < len) {
        if (ansi_emit_newline(impl, sink) != 0) return -1;
        return ansi_write_split_word_full_width(impl, sink, src + split, len - split);
    }
    return 0;
}

typedef enum ansi_sim_kind {
    ANSI_SIM_TEXT = 0,
    ANSI_SIM_URL = 1,
    ANSI_SIM_STRUCT = 2
} ansi_sim_kind;

typedef struct ansi_sim_input {
    const char *text;
    size_t len;
    const char *style;
    ansi_sim_kind kind;
} ansi_sim_input;

typedef struct ansi_sim_atom {
    size_t off;
    size_t len;
    const char *style;
    ansi_sim_kind kind;
} ansi_sim_atom;

typedef struct ansi_sim_word {
    char *buf;
    size_t len;
    size_t cap;
    ansi_sim_atom *atoms;
    size_t atom_count;
    size_t atom_cap;
    size_t cols;
    const char *first_style;
    ansi_sim_kind first_kind;
    int has_url;
    int has_non_url;
} ansi_sim_word;

typedef enum ansi_sim_boundary {
    ANSI_SIM_BOUNDARY_NONE = 0,
    ANSI_SIM_BOUNDARY_SPACE = 1,
    ANSI_SIM_BOUNDARY_NEWLINE = 2,
    ANSI_SIM_BOUNDARY_PUNCT = 3,
    ANSI_SIM_BOUNDARY_PUNCT_END = 4
} ansi_sim_boundary;

static void ansi_sim_word_reset(ansi_sim_word *word)
{
    word->len = 0;
    word->atom_count = 0;
    word->cols = 0;
    word->first_style = "";
    word->first_kind = ANSI_SIM_TEXT;
    word->has_url = 0;
    word->has_non_url = 0;
}

static void ansi_sim_word_destroy(mdf_allocator *allocator, ansi_sim_word *word)
{
    mdf_free_mem(allocator, word->buf, word->cap);
    mdf_free_mem(allocator, word->atoms, word->atom_cap * sizeof(*word->atoms));
    memset(word, 0, sizeof(*word));
}

static int ansi_sim_word_append(mdf_allocator *allocator, ansi_sim_word *word, const char *text, size_t len, size_t cols, const char *style, ansi_sim_kind kind)
{
    ansi_sim_atom *next_atoms;
    size_t new_cap;
    char *next_buf;
    size_t old_len;

    if (len == 0) {
        return 0;
    }
    if (word->atom_count == 0) {
        word->first_style = style == NULL ? "" : style;
        word->first_kind = kind;
    }
    if (word->len + len < word->len) {
        return -1;
    }
    if (word->len + len + 1 > word->cap) {
        new_cap = word->cap == 0 ? 64 : word->cap;
        while (new_cap < word->len + len + 1) {
            new_cap *= 2;
        }
        next_buf = (char *)mdf_realloc_mem(allocator, word->buf, word->cap, new_cap);
        if (next_buf == NULL) {
            return -1;
        }
        word->buf = next_buf;
        word->cap = new_cap;
    }
    if (word->atom_count + 1 > word->atom_cap) {
        new_cap = word->atom_cap == 0 ? 32 : word->atom_cap;
        while (new_cap < word->atom_count + 1) {
            new_cap *= 2;
        }
        next_atoms = (ansi_sim_atom *)mdf_realloc_mem(allocator, word->atoms, word->atom_cap * sizeof(*word->atoms), new_cap * sizeof(*word->atoms));
        if (next_atoms == NULL) {
            return -1;
        }
        word->atoms = next_atoms;
        word->atom_cap = new_cap;
    }
    old_len = word->len;
    memcpy(word->buf + word->len, text, len);
    word->len += len;
    word->buf[word->len] = '\0';
    word->atoms[word->atom_count].off = old_len;
    word->atoms[word->atom_count].len = len;
    word->atoms[word->atom_count].style = style == NULL ? "" : style;
    word->atoms[word->atom_count].kind = kind;
    word->atom_count++;
    word->cols += cols;
    if (kind == ANSI_SIM_URL) {
        word->has_url = 1;
    } else {
        word->has_non_url = 1;
    }
    return 0;
}

static ansi_sim_boundary ansi_sim_classify_boundary(unsigned long cp, unsigned long next_cp)
{
    if (cp == '\n') {
        return ANSI_SIM_BOUNDARY_NEWLINE;
    }
    if (cp == ' ' || cp == '\t') {
        return ANSI_SIM_BOUNDARY_SPACE;
    }
    if (ansi_is_quote_codepoint(cp)) {
        return ANSI_SIM_BOUNDARY_NONE;
    }
    if (cp == '.') {
        return next_cp >= 'A' && next_cp <= 'Z' ? ANSI_SIM_BOUNDARY_PUNCT : ANSI_SIM_BOUNDARY_NONE;
    }
    if (cp == ',' || cp == ';' || cp == ':' || cp == '!' || cp == '?') {
        return ansi_is_quote_codepoint(next_cp) ? ANSI_SIM_BOUNDARY_NONE : ANSI_SIM_BOUNDARY_PUNCT;
    }
    return ANSI_SIM_BOUNDARY_NONE;
}

static int ansi_sim_style_switch(mdf_impl *impl, mdf_sink *sink, const char **current_style, const char *style)
{
    const char *target;

    target = style == NULL ? "" : style;
    if (strcmp(*current_style, target) == 0) {
        return 0;
    }
    if ((*current_style)[0] != '\0' && mdf_emit_cstr(impl, sink, "\033[0m") != 0) {
        return -1;
    }
    if (target[0] != '\0' && mdf_emit_cstr(impl, sink, target) != 0) {
        return -1;
    }
    *current_style = target;
    return 0;
}

static int ansi_sim_emit_styled_direct(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len, const char *style, const char **active_style)
{
    size_t off;
    int url_style;

    url_style = style != NULL && strstr(style, "\033[4m") != NULL;
    if (url_style && impl->opts.width > 0) {
        off = 0;
        while (off < len) {
            unsigned long cp;
            size_t adv;

            adv = utf8_decode_codepoint(text + off, len - off, &cp);
            if (adv == 0) break;
            if (impl->ansi_col >= impl->opts.width) {
                if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            if (strcmp(*active_style, style) != 0) {
                if ((*active_style)[0] != '\0' && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
                if (ansi_emit_styled_visible_chunk(impl, sink, style, text + off, adv) != 0) return -1;
                *active_style = style;
            } else if (ansi_emit_visible_chunk(impl, sink, text + off, adv) != 0) {
                return -1;
            }
            off += adv;
        }
        return 0;
    }
    off = 0;
    while (off < len) {
        int avail;
        size_t split;
        size_t rem_len;
        size_t i;
        size_t cols;
        size_t last_rune_start;
        unsigned long last_rune_cp;
        unsigned long next_cp;
        size_t adv;
        size_t next_adv;

        if (impl->opts.width <= 0) {
            if (ansi_sim_style_switch(impl, sink, active_style, style) != 0) return -1;
            if (ansi_write_direct_visible(impl, sink, text + off, len - off) != 0) return -1;
            break;
        }
        avail = impl->opts.width - impl->ansi_col;
        if (avail <= 0) {
            if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            continue;
        }
        rem_len = len - off;
        if ((int)visible_cols(text + off, rem_len) <= avail) {
            if (ansi_sim_style_switch(impl, sink, active_style, style) != 0) return -1;
            if (ansi_write_direct_visible(impl, sink, text + off, rem_len) != 0) return -1;
            break;
        }
        split = 0;
        cols = 0;
        i = off;
        last_rune_start = off;
        last_rune_cp = 0;
        while (i < len) {
            adv = utf8_decode_codepoint(text + i, len - i, &last_rune_cp);
            if (adv == 0) {
                break;
            }
            if (cols + utf8_display_width(last_rune_cp) > (size_t)avail) {
                break;
            }
            last_rune_start = i;
            cols += utf8_display_width(last_rune_cp);
            i += adv;
            split = i;
        }
        if (split < len) {
            next_adv = utf8_decode_codepoint(text + split, len - split, &next_cp);
            if (last_rune_start > off &&
                ((ansi_is_quote_codepoint(last_rune_cp)) ||
                 (next_adv > 0 && split + next_adv == len && ansi_is_quote_codepoint(next_cp)))) {
                split = last_rune_start;
            }
        }
        if (split <= off) {
            unsigned long cp0;

            split = off + utf8_decode_codepoint(text + off, len - off, &cp0);
            if (split <= off) {
                break;
            }
        }
        if (ansi_sim_style_switch(impl, sink, active_style, style) != 0) return -1;
        if (ansi_write_direct_visible(impl, sink, text + off, split - off) != 0) return -1;
        off = split;
        if (off < len) {
            if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
    }
    return 0;
}

static int ansi_sim_emit_first_chunk_ignoring_current_col(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len, const char *style, const char **active_style, int limit, size_t *emitted_bytes)
{
    size_t i;
    size_t split;
    size_t cols;
    size_t adv;
    unsigned long cp;
    unsigned long next_cp;
    size_t next_adv;
    size_t last_rune_start;
    unsigned long last_rune_cp;

    if (limit <= 0) {
        *emitted_bytes = 0;
        return 0;
    }
    split = 0;
    cols = 0;
    i = 0;
    last_rune_start = 0;
    last_rune_cp = 0;
    while (i < len) {
        adv = utf8_decode_codepoint(text + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        if (cols + utf8_display_width(cp) > (size_t)limit) {
            break;
        }
        last_rune_start = i;
        last_rune_cp = cp;
        cols += utf8_display_width(cp);
        i += adv;
        split = i;
    }
    if (split < len) {
        next_adv = utf8_decode_codepoint(text + split, len - split, &next_cp);
        if (last_rune_start > 0 &&
            (ansi_is_quote_codepoint(last_rune_cp) ||
             (next_adv > 0 && split + next_adv == len && ansi_is_quote_codepoint(next_cp)))) {
            split = last_rune_start;
            cols = visible_cols(text, split);
        }
    }
    if (split == 0) {
        split = len;
        cols = visible_cols(text, len);
    }
    if (ansi_sim_style_switch(impl, sink, active_style, style) != 0) return -1;
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (mdf_emit_all(impl, sink, text, split) != 0) return -1;
    i = 0;
    while (i < split) {
        adv = utf8_decode_codepoint(text + i, split - i, &cp);
        if (adv == 0) {
            break;
        }
        impl->ansi_col += (int)utf8_display_width(cp);
        impl->ansi_prev_char = text[i + adv - 1];
        if (cp == ' ') {
            impl->ansi_line_has_space = 1;
        }
        i += adv;
    }
    *emitted_bytes = split;
    return 0;
}

static int ansi_sim_emit_first_chunk_runes_ignoring_current_col(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len, const char *style, const char **active_style, int limit, size_t *emitted_bytes)
{
    size_t split;
    size_t off;

    split = ansi_rune_split_offset(text, len, limit);
    if (split == 0) {
        split = len;
    }
    off = 0;
    while (off < split) {
        unsigned long cp;
        size_t adv;

        adv = utf8_decode_codepoint(text + off, split - off, &cp);
        if (adv == 0) {
            break;
        }
        if (strcmp(*active_style, style == NULL ? "" : style) != 0) {
            const char *target;

            target = style == NULL ? "" : style;
            if ((*active_style)[0] != '\0' && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            if (target[0] != '\0') {
                if (ansi_emit_styled_visible_chunk(impl, sink, target, text + off, adv) != 0) return -1;
            } else if (ansi_emit_visible_chunk(impl, sink, text + off, adv) != 0) {
                return -1;
            }
            *active_style = target;
        } else if (ansi_emit_visible_chunk(impl, sink, text + off, adv) != 0) {
            return -1;
        }
        off += adv;
    }
    *emitted_bytes = split;
    return 0;
}

static int ansi_sim_emit_atoms(mdf_impl *impl, mdf_sink *sink, const ansi_sim_word *word, const char **active_style)
{
    size_t i;
    i = 0;
    while (i < word->atom_count) {
        const char *target;
        const char *text;
        size_t start;
        size_t len;
        size_t j;

        target = word->atoms[i].style == NULL ? "" : word->atoms[i].style;
        start = word->atoms[i].off;
        len = word->atoms[i].len;
        j = i + 1;
        while (j < word->atom_count) {
            const char *next_style;

            next_style = word->atoms[j].style == NULL ? "" : word->atoms[j].style;
            if (strcmp(target, next_style) != 0 || word->atoms[j].off != start + len) {
                break;
            }
            len += word->atoms[j].len;
            j++;
        }
        text = word->buf + start;
        if (word->atoms[i].kind == ANSI_SIM_STRUCT) {
            if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
            if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
            if (mdf_emit_all(impl, sink, text, len) != 0) return -1;
            ansi_update_visible_output_state(impl, text, len);
        } else if (strstr(target, "\033[4m") != NULL &&
            (word->atoms[i].kind == ANSI_SIM_URL ||
             (strcmp(*active_style, target) == 0 &&
              (ansi_word_contains_byte(text, len, '/') ||
               ansi_word_contains_byte(text, len, '\\'))))) {
            size_t off;

            off = 0;
            while (off < len) {
                unsigned long cp;
                size_t adv;

                adv = utf8_decode_codepoint(text + off, len - off, &cp);
                if (adv == 0) break;
                (void)cp;
                if (strcmp(*active_style, target) != 0) {
                    if ((*active_style)[0] != '\0' && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
                    if (ansi_emit_styled_visible_chunk(impl, sink, target, text + off, adv) != 0) {
                        return -1;
                    }
                    *active_style = target;
                } else if (ansi_emit_visible_chunk(impl, sink, text + off, adv) != 0) {
                    return -1;
                }
                off += adv;
            }
        } else if (strcmp(*active_style, target) != 0) {
            if ((*active_style)[0] != '\0' && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            if (target[0] != '\0') {
                if (ansi_emit_styled_visible_chunk(impl, sink, target, text, len) != 0) return -1;
            } else if (ansi_emit_visible_chunk(impl, sink, text, len) != 0) {
                return -1;
            }
            *active_style = target;
        } else if (ansi_emit_visible_chunk(impl, sink, text, len) != 0) {
            return -1;
        }
        i = j;
    }
    return 0;
}

static int ansi_sim_flush_word(mdf_impl *impl, mdf_sink *sink, ansi_sim_word *word, const char **active_style, ansi_sim_boundary boundary)
{
    int preserve_prefix_line;
    int force_plain_mixed_url_exact;

    if (word->atom_count == 0) {
        return 0;
    }
    force_plain_mixed_url_exact = impl->opts.width > 0 &&
                                  (boundary == ANSI_SIM_BOUNDARY_PUNCT || boundary == ANSI_SIM_BOUNDARY_PUNCT_END) &&
                                  word->has_url &&
                                  word->has_non_url &&
                                  word->cols == (size_t)impl->opts.width;
    preserve_prefix_line = impl->opts.width > 0 &&
                           impl->ansi_col > 0 &&
                           !impl->ansi_pending_space &&
                           !impl->ansi_line_has_space &&
                           impl->ansi_col + (int)word->cols > impl->opts.width;
    if (word->has_url && !word->has_non_url) {
        preserve_prefix_line = 0;
    }
    if (impl->opts.width > 0 && impl->ansi_col > 0 &&
        impl->ansi_pending_space && impl->ansi_col + 1 + (int)word->cols > impl->opts.width) {
        if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
        if (ansi_emit_newline(impl, sink) != 0) return -1;
        impl->ansi_pending_space = 0;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 0;
    }
    if (impl->ansi_pending_space) {
        impl->ansi_pending_space = 0;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 0;
        if (ansi_write_word_byte(impl, sink, ' ') != 0) return -1;
    }
    if (impl->opts.width > 0 && impl->ansi_col > 0 &&
        impl->ansi_col + (int)word->cols > impl->opts.width) {
        if (!preserve_prefix_line) {
            if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
    }
    if (impl->opts.width > 0 &&
        (word->cols > (size_t)impl->opts.width || force_plain_mixed_url_exact)) {
        if (preserve_prefix_line) {
            size_t emitted;

            emitted = 0;
            if (word->first_style != NULL &&
                strstr(word->first_style, "\033[4m") != NULL &&
                (ansi_word_contains_byte(word->buf, word->len, '/') ||
                 ansi_word_contains_byte(word->buf, word->len, '\\'))) {
                if (ansi_sim_emit_first_chunk_runes_ignoring_current_col(impl, sink, word->buf, word->len, word->first_style, active_style, impl->opts.width, &emitted) != 0) return -1;
            } else if (ansi_sim_emit_first_chunk_ignoring_current_col(impl, sink, word->buf, word->len, word->first_style, active_style, impl->opts.width, &emitted) != 0) return -1;
            if (emitted < word->len) {
                if (ansi_sim_style_switch(impl, sink, active_style, "") != 0) return -1;
                if (ansi_emit_newline(impl, sink) != 0) return -1;
                if (ansi_sim_emit_styled_direct(impl, sink, word->buf + emitted, word->len - emitted, word->first_style, active_style) != 0) return -1;
            }
        } else {
            if (ansi_sim_emit_styled_direct(impl, sink, word->buf, word->len, word->first_style, active_style) != 0) return -1;
        }
    } else {
        if (ansi_sim_emit_atoms(impl, sink, word, active_style) != 0) return -1;
    }
    ansi_sim_word_reset(word);
    return 0;
}

static int ansi_sim_emit_inputs_active(mdf_impl *impl, mdf_sink *sink, const ansi_sim_input *inputs, size_t input_count, const char *initial_active_style)
{
    ansi_sim_word word;
    size_t input_idx;
    size_t off;
    unsigned long cp;
    unsigned long next_cp;
    size_t adv;
    size_t next_adv;
    ansi_sim_boundary boundary;
    const char *active_style;
    int punct_quote_pending;

    memset(&word, 0, sizeof(word));
    ansi_sim_word_reset(&word);
    active_style = initial_active_style == NULL ? "" : initial_active_style;
    punct_quote_pending = 0;
    for (input_idx = 0; input_idx < input_count; input_idx++) {
        off = 0;
        while (off < inputs[input_idx].len) {
            adv = utf8_decode_codepoint(inputs[input_idx].text + off, inputs[input_idx].len - off, &cp);
            if (adv == 0) {
                break;
            }
            next_cp = 0;
            if (off + adv < inputs[input_idx].len) {
                next_adv = utf8_decode_codepoint(inputs[input_idx].text + off + adv, inputs[input_idx].len - off - adv, &next_cp);
                (void)next_adv;
            } else if (input_idx + 1 < input_count && inputs[input_idx + 1].len > 0) {
                next_adv = utf8_decode_codepoint(inputs[input_idx + 1].text, inputs[input_idx + 1].len, &next_cp);
                (void)next_adv;
            }
            boundary = ansi_sim_classify_boundary(cp, next_cp);
            if (punct_quote_pending) {
                if (ansi_is_quote_codepoint(cp)) {
                    boundary = ANSI_SIM_BOUNDARY_NONE;
                    punct_quote_pending = 0;
                } else {
                    if (ansi_sim_flush_word(impl, sink, &word, &active_style, ANSI_SIM_BOUNDARY_NONE) != 0) goto fail;
                    punct_quote_pending = 0;
                }
            }
            if (boundary == ANSI_SIM_BOUNDARY_NONE) {
                if (ansi_sim_word_append(&impl->allocator, &word, inputs[input_idx].text + off, adv, utf8_display_width(cp), inputs[input_idx].style, inputs[input_idx].kind) != 0) goto nomem;
            } else if (boundary == ANSI_SIM_BOUNDARY_SPACE) {
                if (ansi_sim_flush_word(impl, sink, &word, &active_style, boundary) != 0) goto fail;
                if (impl->ansi_col == 0 || (impl->ansi_prev_char == ' ' && !impl->ansi_pending_space)) {
                    if (ansi_sim_style_switch(impl, sink, &active_style, inputs[input_idx].style) != 0) goto fail;
                    if (ansi_write_word_byte(impl, sink, ' ') != 0) goto fail;
                } else {
                    impl->ansi_pending_space = 1;
                    impl->ansi_pending_space_no_split = 0;
                    impl->ansi_pending_space_plain = 0;
                }
            } else if (boundary == ANSI_SIM_BOUNDARY_NEWLINE) {
                if (ansi_sim_flush_word(impl, sink, &word, &active_style, boundary) != 0) goto fail;
                if (ansi_sim_style_switch(impl, sink, &active_style, "") != 0) goto fail;
                if (ansi_emit_newline(impl, sink) != 0) goto fail;
            } else {
                if (inputs[input_idx].kind == ANSI_SIM_URL) {
                    if (ansi_sim_word_append(&impl->allocator, &word, inputs[input_idx].text + off, adv, utf8_display_width(cp), inputs[input_idx].style, inputs[input_idx].kind) != 0) goto nomem;
                } else {
                    if (ansi_sim_word_append(&impl->allocator, &word, inputs[input_idx].text + off, adv, utf8_display_width(cp), inputs[input_idx].style, inputs[input_idx].kind) != 0) goto nomem;
                    if (boundary == ANSI_SIM_BOUNDARY_PUNCT_END ||
                        (boundary == ANSI_SIM_BOUNDARY_PUNCT && next_cp == 0)) {
                        punct_quote_pending = 1;
                        off += adv;
                        continue;
                    }
                    if (ansi_sim_flush_word(impl, sink, &word, &active_style, boundary) != 0) goto fail;
                }
            }
            off += adv;
        }
    }
    if (ansi_sim_flush_word(impl, sink, &word, &active_style, ANSI_SIM_BOUNDARY_NONE) != 0) goto fail;
    ansi_sim_word_destroy(&impl->allocator, &word);
    return 0;

nomem:
fail:
    ansi_sim_word_destroy(&impl->allocator, &word);
    return -1;
}

static int ansi_sim_emit_inputs(mdf_impl *impl, mdf_sink *sink, const ansi_sim_input *inputs, size_t input_count)
{
    return ansi_sim_emit_inputs_active(impl, sink, inputs, input_count, "");
}

static int ansi_flush_pending_exact_fallback(mdf_impl *impl, mdf_sink *sink, char next_char)
{
    ansi_sim_input inputs[3];
    const char *url_style;
    size_t fallback_cols;
    int content_limit;

    if (!impl->pending_fallback_exact) {
        return 0;
    }
    url_style = "";
    if (!impl->opts.boring && next_char != '.') {
        url_style = mdf_theme_link_url(impl);
    }
    content_limit = ansi_content_limit(impl);
    fallback_cols = visible_cols(impl->pending_fallback_url, impl->pending_fallback_url_len) + 2;
    if (url_style[0] != '\0' &&
        content_limit > 0 &&
        fallback_cols <= (size_t)content_limit) {
        if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (mdf_emit_buffer_reset(impl) != 0 ||
            mdf_emit_buffer_append(impl, "(", 1) != 0 ||
            (url_style[0] != '\0' && mdf_emit_buffer_append(impl, url_style, strlen(url_style)) != 0) ||
            mdf_emit_buffer_append(impl, impl->pending_fallback_url, impl->pending_fallback_url_len) != 0 ||
            (url_style[0] != '\0' && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
            mdf_emit_buffer_append(impl, ")", 1) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
        ansi_update_visible_output_state(impl, "(", 1);
        ansi_update_visible_output_state(impl, impl->pending_fallback_url, impl->pending_fallback_url_len);
        ansi_update_visible_output_state(impl, ")", 1);
        impl->pending_fallback_exact = 0;
        impl->pending_fallback_url_len = 0;
        return 0;
    }
    if (next_char == '.' &&
        impl->opts.width > 0 &&
        fallback_cols >= (size_t)impl->opts.width) {
        size_t i;

        if (ansi_emit_visible_chunk(impl, sink, "(", 1) != 0) return -1;
        for (i = 0; i < impl->pending_fallback_url_len; i++) {
            if (ansi_emit_visible_chunk(impl, sink, impl->pending_fallback_url + i, 1) != 0) return -1;
        }
        if (ansi_emit_visible_chunk(impl, sink, ")", 1) != 0) return -1;
        impl->pending_fallback_exact = 0;
        impl->pending_fallback_url_len = 0;
        return 0;
    }
    inputs[0].text = "(";
    inputs[0].len = 1;
    inputs[0].style = "";
    inputs[0].kind = ANSI_SIM_TEXT;
    inputs[1].text = impl->pending_fallback_url;
    inputs[1].len = impl->pending_fallback_url_len;
    inputs[1].style = url_style;
    inputs[1].kind = ANSI_SIM_URL;
    inputs[2].text = ")";
    inputs[2].len = 1;
    inputs[2].style = "";
    inputs[2].kind = ANSI_SIM_TEXT;
    if (ansi_sim_emit_inputs(impl, sink, inputs, 3) != 0) {
        return -1;
    }
    impl->pending_fallback_exact = 0;
    impl->pending_fallback_url_len = 0;
    return 0;
}

static int ansi_set_pending_exact_fallback(mdf_impl *impl, const char *url, size_t url_len)
{
    impl->pending_fallback_url_len = 0;
    if (ansi_inline_append(impl, &impl->pending_fallback_url, &impl->pending_fallback_url_len,
            &impl->pending_fallback_url_cap, url, url_len) != 0) return -1;
    impl->pending_fallback_exact = 1;
    return 0;
}

static int ansi_emit_pending_exact_fallback_with_period(mdf_impl *impl, mdf_sink *sink, int *consumed)
{
    size_t fallback_cols;

    *consumed = 0;
    if (!impl->pending_fallback_exact || impl->opts.boring) {
        return 0;
    }
    fallback_cols = visible_cols(impl->pending_fallback_url, impl->pending_fallback_url_len) + 2;
    if (impl->opts.width <= 0 || fallback_cols + 1 > (size_t)impl->opts.width) {
        return 0;
    }
    if (mdf_emit_buffer_reset(impl) != 0 ||
        mdf_emit_buffer_append(impl, "(", 1) != 0 ||
        mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0 ||
        mdf_emit_buffer_append(impl, impl->pending_fallback_url, impl->pending_fallback_url_len) != 0 ||
        mdf_emit_buffer_append(impl, "\033[0m).", 6) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    ansi_update_visible_output_state(impl, "(", 1);
    ansi_update_visible_output_state(impl, impl->pending_fallback_url, impl->pending_fallback_url_len);
    ansi_update_visible_output_state(impl, ").", 2);
    impl->pending_fallback_exact = 0;
    impl->pending_fallback_url_len = 0;
    *consumed = 1;
    return 0;
}

static int ansi_emit_pending_exact_fallback_with_close(mdf_impl *impl, mdf_sink *sink, int *consumed)
{
    size_t fallback_cols;

    *consumed = 0;
    if (!impl->pending_fallback_exact || impl->opts.boring) {
        return 0;
    }
    fallback_cols = visible_cols(impl->pending_fallback_url, impl->pending_fallback_url_len) + 2;
    if (impl->opts.width <= 0 || fallback_cols + 1 > (size_t)impl->opts.width) {
        return 0;
    }
    if (mdf_emit_buffer_reset(impl) != 0 ||
        mdf_emit_buffer_append(impl, "(", 1) != 0 ||
        mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0 ||
        mdf_emit_buffer_append(impl, impl->pending_fallback_url, impl->pending_fallback_url_len) != 0 ||
        mdf_emit_buffer_append(impl, "\033[0m))", 6) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    ansi_update_visible_output_state(impl, "(", 1);
    ansi_update_visible_output_state(impl, impl->pending_fallback_url, impl->pending_fallback_url_len);
    ansi_update_visible_output_state(impl, "))", 2);
    impl->pending_fallback_exact = 0;
    impl->pending_fallback_url_len = 0;
    *consumed = 1;
    return 0;
}

static int ansi_write_split_word_bytes(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t off;

    if (impl->opts.width <= 0) {
        return ansi_write_direct_visible(impl, sink, src, len);
    }
    off = 0;
    while (off < len) {
        int avail;
        size_t split;
        size_t rem_len;

        avail = impl->opts.width - impl->ansi_col;
        rem_len = len - off;
        if (avail <= 0) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            continue;
        }
        if ((int)visible_cols(src + off, rem_len) <= avail) {
            return ansi_emit_visible_chunk(impl, sink, src + off, rem_len);
        }
        split = ansi_word_split_offset(src + off, rem_len, avail);
        if (split == 0) {
            return ansi_emit_visible_chunk(impl, sink, src + off, rem_len);
        }
        if (ansi_emit_visible_chunk(impl, sink, src + off, split) != 0) return -1;
        off += split;
        if (off < len) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
    }
    return 0;
}

static int utf8_continuation_byte(char c)
{
    return ((unsigned char)c & 0xc0) == 0x80;
}

size_t utf8_decode_codepoint(const char *s, size_t len, unsigned long *cp)
{
    unsigned char c0;

    if (len == 0) {
        *cp = 0;
        return 0;
    }
    c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    }
    if ((c0 & 0xe0) == 0xc0 && len >= 2 && utf8_continuation_byte(s[1])) {
        *cp = ((unsigned long)(c0 & 0x1f) << 6) |
              (unsigned long)((unsigned char)s[1] & 0x3f);
        if (*cp >= 0x80) {
            return 2;
        }
    }
    if ((c0 & 0xf0) == 0xe0 && len >= 3 &&
        utf8_continuation_byte(s[1]) && utf8_continuation_byte(s[2])) {
        *cp = ((unsigned long)(c0 & 0x0f) << 12) |
              ((unsigned long)((unsigned char)s[1] & 0x3f) << 6) |
              (unsigned long)((unsigned char)s[2] & 0x3f);
        if (*cp >= 0x800) {
            return 3;
        }
    }
    if ((c0 & 0xf8) == 0xf0 && len >= 4 &&
        utf8_continuation_byte(s[1]) && utf8_continuation_byte(s[2]) &&
        utf8_continuation_byte(s[3])) {
        *cp = ((unsigned long)(c0 & 0x07) << 18) |
              ((unsigned long)((unsigned char)s[1] & 0x3f) << 12) |
              ((unsigned long)((unsigned char)s[2] & 0x3f) << 6) |
              (unsigned long)((unsigned char)s[3] & 0x3f);
        if (*cp >= 0x10000 && *cp <= 0x10ffff) {
            return 4;
        }
    }
    *cp = c0;
    return 1;
}

static int unicode_is_combining(unsigned long cp)
{
    return (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05bd) ||
           cp == 0x05bf ||
           (cp >= 0x05c1 && cp <= 0x05c2) ||
           (cp >= 0x05c4 && cp <= 0x05c5) ||
           cp == 0x05c7 ||
           (cp >= 0x0610 && cp <= 0x061a) ||
           (cp >= 0x064b && cp <= 0x065f) ||
           cp == 0x0670 ||
           (cp >= 0x06d6 && cp <= 0x06dc) ||
           (cp >= 0x06df && cp <= 0x06e4) ||
           (cp >= 0x06e7 && cp <= 0x06e8) ||
           (cp >= 0x06ea && cp <= 0x06ed) ||
           (cp >= 0x0711 && cp <= 0x0711) ||
           (cp >= 0x0730 && cp <= 0x074a) ||
           (cp >= 0x07a6 && cp <= 0x07b0) ||
           (cp >= 0x07eb && cp <= 0x07f3) ||
           (cp >= 0x0816 && cp <= 0x0819) ||
           (cp >= 0x081b && cp <= 0x0823) ||
           (cp >= 0x0825 && cp <= 0x0827) ||
           (cp >= 0x0829 && cp <= 0x082d) ||
           (cp >= 0x0859 && cp <= 0x085b) ||
           (cp >= 0x08d3 && cp <= 0x0902) ||
           cp == 0x093a ||
           cp == 0x093c ||
           (cp >= 0x0941 && cp <= 0x0948) ||
           cp == 0x094d ||
           (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x0962 && cp <= 0x0963) ||
           cp == 0x0981 ||
           cp == 0x09bc ||
           cp == 0x09c1 ||
           cp == 0x09c2 ||
           cp == 0x09cd ||
           (cp >= 0x09e2 && cp <= 0x09e3) ||
           cp == 0x0a01 ||
           cp == 0x0a02 ||
           cp == 0x0a3c ||
           cp == 0x0a41 ||
           cp == 0x0a42 ||
           cp == 0x0a47 ||
           cp == 0x0a48 ||
           cp == 0x0a4b ||
           cp == 0x0a4c ||
           cp == 0x0a4d ||
           (cp >= 0x0a51 && cp <= 0x0a51) ||
           cp == 0x0a70 ||
           cp == 0x0a71 ||
           cp == 0x0a75 ||
           cp == 0x0a81 ||
           cp == 0x0abc ||
           (cp >= 0x0ac1 && cp <= 0x0ac5) ||
           (cp >= 0x0ac7 && cp <= 0x0ac8) ||
           cp == 0x0acd ||
           (cp >= 0x0ae2 && cp <= 0x0ae3) ||
           cp == 0x0b01 ||
           cp == 0x0b3c ||
           cp == 0x0b3f ||
           (cp >= 0x0b41 && cp <= 0x0b44) ||
           cp == 0x0b4d ||
           cp == 0x0b56 ||
           (cp >= 0x0b62 && cp <= 0x0b63) ||
           cp == 0x0b82 ||
           cp == 0x0bc0 ||
           cp == 0x0bcd ||
           cp == 0x0c00 ||
           cp == 0x0c04 ||
           (cp >= 0x0c3e && cp <= 0x0c40) ||
           (cp >= 0x0c46 && cp <= 0x0c48) ||
           (cp >= 0x0c4a && cp <= 0x0c4d) ||
           (cp >= 0x0c55 && cp <= 0x0c56) ||
           (cp >= 0x0c62 && cp <= 0x0c63) ||
           cp == 0x0c81 ||
           cp == 0x0cbc ||
           cp == 0x0cbf ||
           cp == 0x0cc6 ||
           cp == 0x0ccc ||
           cp == 0x0ccd ||
           (cp >= 0x0ce2 && cp <= 0x0ce3) ||
           cp == 0x0d00 ||
           cp == 0x0d01 ||
           (cp >= 0x0d3b && cp <= 0x0d3c) ||
           cp == 0x0d41 ||
           cp == 0x0d42 ||
           cp == 0x0d4d ||
           (cp >= 0x0d62 && cp <= 0x0d63) ||
           cp == 0x0dca ||
           (cp >= 0x0dd2 && cp <= 0x0dd4) ||
           cp == 0x0dd6 ||
           cp == 0x0e31 ||
           (cp >= 0x0e34 && cp <= 0x0e3a) ||
           (cp >= 0x0e47 && cp <= 0x0e4e) ||
           cp == 0x0eb1 ||
           (cp >= 0x0eb4 && cp <= 0x0ebc) ||
           (cp >= 0x0ec8 && cp <= 0x0ecd) ||
           cp == 0x0f18 ||
           cp == 0x0f19 ||
           cp == 0x0f35 ||
           cp == 0x0f37 ||
           cp == 0x0f39 ||
           (cp >= 0x0f71 && cp <= 0x0f7e) ||
           (cp >= 0x0f80 && cp <= 0x0f84) ||
           (cp >= 0x0f86 && cp <= 0x0f87) ||
           (cp >= 0x0f8d && cp <= 0x0f97) ||
           (cp >= 0x0f99 && cp <= 0x0fbc) ||
           cp == 0x0fc6 ||
           (cp >= 0x102d && cp <= 0x1030) ||
           (cp >= 0x1032 && cp <= 0x1037) ||
           (cp >= 0x1039 && cp <= 0x103a) ||
           (cp >= 0x103d && cp <= 0x103e) ||
           (cp >= 0x1058 && cp <= 0x1059) ||
           (cp >= 0x105e && cp <= 0x1060) ||
           (cp >= 0x1071 && cp <= 0x1074) ||
           cp == 0x1082 ||
           (cp >= 0x1085 && cp <= 0x1086) ||
           cp == 0x108d ||
           cp == 0x109d ||
           (cp >= 0x135d && cp <= 0x135f) ||
           (cp >= 0x1712 && cp <= 0x1714) ||
           (cp >= 0x1732 && cp <= 0x1734) ||
           (cp >= 0x1752 && cp <= 0x1753) ||
           (cp >= 0x1772 && cp <= 0x1773) ||
           (cp >= 0x17b4 && cp <= 0x17b5) ||
           (cp >= 0x17b7 && cp <= 0x17bd) ||
           cp == 0x17c6 ||
           (cp >= 0x17c9 && cp <= 0x17d3) ||
           cp == 0x17dd ||
           (cp >= 0x180b && cp <= 0x180f) ||
           cp == 0x1885 ||
           cp == 0x1886 ||
           (cp >= 0x18a9 && cp <= 0x18a9) ||
           (cp >= 0x1920 && cp <= 0x1922) ||
           (cp >= 0x1927 && cp <= 0x1928) ||
           cp == 0x1932 ||
           (cp >= 0x1939 && cp <= 0x193b) ||
           (cp >= 0x1a17 && cp <= 0x1a18) ||
           cp == 0x1a1b ||
           cp == 0x1a56 ||
           (cp >= 0x1a58 && cp <= 0x1a5e) ||
           cp == 0x1a60 ||
           cp == 0x1a62 ||
           (cp >= 0x1a65 && cp <= 0x1a6c) ||
           (cp >= 0x1a73 && cp <= 0x1a7c) ||
           cp == 0x1a7f ||
           (cp >= 0x1ab0 && cp <= 0x1ace) ||
           (cp >= 0x1b00 && cp <= 0x1b03) ||
           cp == 0x1b34 ||
           (cp >= 0x1b36 && cp <= 0x1b3a) ||
           cp == 0x1b3c ||
           cp == 0x1b42 ||
           (cp >= 0x1b6b && cp <= 0x1b73) ||
           (cp >= 0x1b80 && cp <= 0x1b81) ||
           (cp >= 0x1ba2 && cp <= 0x1ba5) ||
           (cp >= 0x1ba8 && cp <= 0x1ba9) ||
           (cp >= 0x1bab && cp <= 0x1bad) ||
           cp == 0x1be6 ||
           (cp >= 0x1be8 && cp <= 0x1be9) ||
           cp == 0x1bed ||
           (cp >= 0x1bef && cp <= 0x1bf1) ||
           (cp >= 0x1c2c && cp <= 0x1c33) ||
           (cp >= 0x1c36 && cp <= 0x1c37) ||
           (cp >= 0x1cd0 && cp <= 0x1cd2) ||
           (cp >= 0x1cd4 && cp <= 0x1ce0) ||
           (cp >= 0x1ce2 && cp <= 0x1ce8) ||
           cp == 0x1ced ||
           cp == 0x1cf4 ||
           cp == 0x1cf8 ||
           cp == 0x1cf9 ||
           (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f);
}

static int unicode_is_wide(unsigned long cp)
{
    return cp >= 0x1100 &&
           (cp <= 0x115f ||
            cp == 0x2329 || cp == 0x232a ||
            (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
            (cp >= 0xac00 && cp <= 0xd7a3) ||
            (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0xfe10 && cp <= 0xfe19) ||
            (cp >= 0xfe30 && cp <= 0xfe6f) ||
            (cp >= 0xff00 && cp <= 0xff60) ||
            (cp >= 0xffe0 && cp <= 0xffe6) ||
            (cp >= 0x1f300 && cp <= 0x1f64f) ||
            (cp >= 0x1f900 && cp <= 0x1f9ff) ||
            (cp >= 0x20000 && cp <= 0x3fffd));
}

size_t utf8_display_width(unsigned long cp)
{
    if (cp == 0 || cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || unicode_is_combining(cp)) {
        return 0;
    }
    if (unicode_is_wide(cp)) {
        return 2;
    }
    return 1;
}

static const char *ansi_heading_style(mdf_impl *impl, int level)
{
    return mdf_theme_heading(impl, level);
}

static const char *ansi_join_styles(const char *prefix, const char *suffix, char *buf, size_t buf_cap);

static const char *ansi_heading_inline_style(mdf_impl *impl, int level, const char *style, char *buf, size_t buf_cap)
{
    if (level <= 0 || level > 6) {
        level = 1;
    }
    if (strcmp(style, mdf_theme_emphasis(impl)) == 0) {
        return ansi_join_styles(ansi_heading_style(impl, level), mdf_theme_emphasis(impl), buf, buf_cap);
    }
    if (strcmp(style, mdf_theme_strong(impl)) == 0) {
        return ansi_join_styles(ansi_heading_style(impl, level), mdf_theme_strong(impl), buf, buf_cap);
    }
    if (strcmp(style, mdf_theme_emphasis_strong(impl)) == 0) {
        return ansi_join_styles(ansi_heading_style(impl, level), mdf_theme_emphasis_strong(impl), buf, buf_cap);
    }
    return style;
}

static const char *ansi_heading_inline_suffix(mdf_impl *impl, const char *style)
{
    if (strcmp(style, mdf_theme_emphasis(impl)) == 0) {
        return mdf_theme_emphasis(impl);
    }
    if (strcmp(style, mdf_theme_strong(impl)) == 0) {
        return mdf_theme_strong(impl);
    }
    if (strcmp(style, mdf_theme_emphasis_strong(impl)) == 0) {
        return mdf_theme_emphasis_strong(impl);
    }
    return style;
}

void ansi_reset_line_output_state(mdf_impl *impl)
{
    impl->ansi_col = 0;
    impl->ansi_writing_left_margin = 0;
    impl->ansi_pending_left_margin = 1;
    impl->ansi_pending_space = 0;
    impl->ansi_pending_space_no_split = 0;
    impl->ansi_pending_space_plain = 0;
    impl->ansi_line_has_space = 0;
    impl->ansi_prev_char = 0;
}

static int ansi_emit_raw_newline(mdf_impl *impl, mdf_sink *sink)
{
    if (mdf_emit_cstr(impl, sink, "\n") != 0) {
        return -1;
    }
    ansi_reset_line_output_state(impl);
    return 0;
}

int ansi_ensure_left_margin(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->format != MDF_FORMAT_ANSI ||
        impl->opts.margin_left <= 0 ||
        (!impl->ansi_pending_left_margin && impl->ansi_col != 0) ||
        impl->ansi_writing_left_margin) {
        return 0;
    }
    impl->ansi_writing_left_margin = 1;
    if (ansi_write_spaces(impl, sink, (size_t)impl->opts.margin_left) != 0) {
        impl->ansi_writing_left_margin = 0;
        return -1;
    }
    impl->ansi_writing_left_margin = 0;
    impl->ansi_pending_left_margin = 0;
    return 0;
}

int ansi_emit_pending_style_reset(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->ansi_pending_style_reset) {
        return 0;
    }
    impl->ansi_pending_style_reset = 0;
    impl->quote_text_open = 0;
    if (impl->opts.boring) {
        return 0;
    }
    return mdf_emit_cstr(impl, sink, "\033[0m");
}

static int ansi_quote_text_active(const mdf_impl *impl)
{
    const char *style;

    if (impl->opts.boring || impl->in_pre || impl->heading_open) {
        return 0;
    }
    if (!(impl->quote_open || impl->quote_wrap_active || impl->quote_depth > 0)) {
        return 0;
    }
    style = mdf_theme_quote_text(impl);
    return style != NULL && style[0] != '\0';
}

static int ansi_close_quote_text(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->quote_text_open) {
        return 0;
    }
    impl->quote_text_open = 0;
    if (impl->opts.boring) {
        return 0;
    }
    return mdf_emit_cstr(impl, sink, "\033[0m");
}

int ansi_emit_quote_marker(mdf_impl *impl, mdf_sink *sink, int trailing_space)
{
    const char *style;

    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (ansi_close_quote_text(impl, sink) != 0) return -1;
    if (impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, trailing_space ? "> " : ">") != 0) return -1;
        return 0;
    }
    style = mdf_theme_quote(impl);
    if (mdf_emit_buffer_reset(impl) != 0 ||
        mdf_emit_buffer_append_cstr(impl, style) != 0 ||
        mdf_emit_buffer_append(impl, ">", 1) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    if (trailing_space && mdf_emit_cstr(impl, sink, " ") != 0) return -1;
    return 0;
}

static int ansi_emit_newline(mdf_impl *impl, mdf_sink *sink)
{
    int i;
    int quote_depth;
    size_t quote_wrap_extra;

    if (ansi_close_quote_text(impl, sink) != 0) return -1;
    ansi_reset_line_output_state(impl);
    if (impl->ansi_active_inline_style != NULL && !impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        impl->ansi_pending_inline_style = impl->ansi_active_inline_style;
    }
    if (ansi_emit_pending_style_reset(impl, sink) != 0) return -1;
    if (impl->heading_open && !impl->heading_style_suspended && !impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    }
    if (ansi_emit_raw_newline(impl, sink) != 0) {
        return -1;
    }
    quote_depth = impl->quote_depth > 0 ? impl->quote_depth : (impl->quote_wrap_active ? 1 : 0);
    quote_wrap_extra = 0;
    if (!impl->in_pre && impl->ansi_wrap_indent > 0 &&
        quote_depth > 0 && impl->ansi_wrap_indent_in_quote) {
        quote_wrap_extra = (size_t)impl->ansi_wrap_indent;
    }
    if (quote_depth > 0 && !impl->in_pre) {
        if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (quote_depth == 1) {
            if (ansi_emit_quote_prefix(impl, sink, (size_t)impl->quote_prefix_indent, quote_wrap_extra) != 0) return -1;
        } else {
            size_t unit_len;
            const char *unit;
            char buf[256];
            size_t off;

            unit = impl->opts.boring ? "> " : "\033[90m>\033[0m ";
            unit_len = impl->opts.boring ? 2 : 11;
            off = 0;
            if (!impl->opts.boring && strcmp(mdf_theme_quote(impl), "\033[90m") != 0) {
                if (impl->quote_prefix_indent > 0 &&
                    ansi_write_spaces(impl, sink, (size_t)impl->quote_prefix_indent) != 0) return -1;
                for (i = 0; i < quote_depth; i++) {
                    if (ansi_emit_quote_prefix(impl, sink, 0, 0) != 0) return -1;
                }
                if (quote_wrap_extra > 0 && ansi_write_spaces(impl, sink, quote_wrap_extra) != 0) return -1;
            } else if (impl->quote_prefix_indent + quote_wrap_extra + ((size_t)quote_depth * unit_len) <= sizeof(buf)) {
                for (i = 0; i < impl->quote_prefix_indent; i++) {
                    buf[off++] = ' ';
                }
                for (i = 0; i < quote_depth; i++) {
                    memcpy(buf + off, unit, unit_len);
                    off += unit_len;
                }
                for (i = 0; i < (int)quote_wrap_extra; i++) {
                    buf[off++] = ' ';
                }
                if (mdf_emit_all(impl, sink, buf, off) != 0) return -1;
                impl->ansi_col += impl->quote_prefix_indent + (quote_depth * 2) + (int)quote_wrap_extra;
                impl->ansi_prev_char = ' ';
                impl->ansi_line_has_space = 1;
            } else {
                if (impl->quote_prefix_indent > 0) {
                    if (ansi_write_spaces(impl, sink, (size_t)impl->quote_prefix_indent) != 0) return -1;
                }
                for (i = 0; i < quote_depth; i++) {
                    if (ansi_emit_quote_prefix(impl, sink, 0, 0) != 0) return -1;
                }
            }
        }
    }
    if (!impl->in_pre && impl->ansi_wrap_indent > 0 &&
        (!impl->quote_wrap_active || impl->ansi_wrap_indent_in_quote)) {
        if (quote_wrap_extra == 0) {
            if (ansi_write_spaces(impl, sink, (size_t)impl->ansi_wrap_indent) != 0) return -1;
        }
    }
    if (impl->heading_open && !impl->in_pre) {
        int heading_indent;

        heading_indent = impl->quote_wrap_active ? 0 : impl->heading_level + 1;
        if (heading_indent > 0) {
            if (ansi_write_spaces(impl, sink, (size_t)heading_indent) != 0) return -1;
        }
        impl->heading_style_pending_prefix = impl->opts.boring ? 0 : 1;
        impl->heading_style_suspended = 0;
    }
    return 0;
}

int ansi_emit_plain_newline(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_close_quote_text(impl, sink) != 0) return -1;
    ansi_reset_line_output_state(impl);
    if (impl->ansi_active_inline_style != NULL && !impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        impl->ansi_pending_inline_style = impl->ansi_active_inline_style;
    }
    if (ansi_emit_pending_style_reset(impl, sink) != 0) return -1;
    return ansi_emit_raw_newline(impl, sink);
}

int ansi_emit_quote_newline(mdf_impl *impl, mdf_sink *sink)
{
    int quote_depth;
    size_t quote_wrap_extra;
    int i;

    if (ansi_close_quote_text(impl, sink) != 0) return -1;
    ansi_reset_line_output_state(impl);
    if (impl->ansi_active_inline_style != NULL && !impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        impl->ansi_pending_inline_style = impl->ansi_active_inline_style;
    }
    if (ansi_emit_pending_style_reset(impl, sink) != 0) return -1;
    if (ansi_emit_raw_newline(impl, sink) != 0) {
        return -1;
    }
    quote_depth = impl->quote_depth > 0 ? impl->quote_depth : 1;
    quote_wrap_extra = 0;
    if (!impl->in_pre && impl->ansi_wrap_indent > 0 && impl->ansi_wrap_indent_in_quote) {
        quote_wrap_extra = (size_t)impl->ansi_wrap_indent;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (quote_depth == 1) {
        if (ansi_emit_quote_prefix_split(impl, sink, (size_t)impl->quote_prefix_indent, quote_wrap_extra) != 0) return -1;
    } else {
        size_t unit_len;
        const char *unit;
        char buf[256];
        size_t off;
        int quote_prefix_emitted;

        unit = impl->opts.boring ? "> " : "\033[90m>\033[0m ";
        unit_len = impl->opts.boring ? 2 : 11;
        off = 0;
        quote_prefix_emitted = 0;
        if (!impl->opts.boring && strcmp(mdf_theme_quote(impl), "\033[90m") != 0) {
            if (impl->quote_prefix_indent > 0 &&
                ansi_write_spaces(impl, sink, (size_t)impl->quote_prefix_indent) != 0) return -1;
            for (i = 0; i < quote_depth; i++) {
                if (ansi_emit_quote_prefix(impl, sink, 0, 0) != 0) return -1;
            }
            if (quote_wrap_extra > 0 && ansi_write_spaces(impl, sink, quote_wrap_extra) != 0) return -1;
            quote_prefix_emitted = 1;
        } else if (impl->quote_prefix_indent + quote_wrap_extra + ((size_t)quote_depth * unit_len) <= sizeof(buf)) {
            for (i = 0; i < impl->quote_prefix_indent; i++) {
                buf[off++] = ' ';
            }
            for (i = 0; i < quote_depth; i++) {
                memcpy(buf + off, unit, unit_len);
                off += unit_len;
            }
            for (i = 0; i < (int)quote_wrap_extra; i++) {
                buf[off++] = ' ';
            }
            if (mdf_emit_all(impl, sink, buf, off) != 0) return -1;
        } else {
            if (impl->quote_prefix_indent > 0) {
                if (ansi_write_spaces(impl, sink, (size_t)impl->quote_prefix_indent) != 0) return -1;
            }
            for (i = 0; i < quote_depth; i++) {
                if (mdf_emit_all(impl, sink, unit, unit_len) != 0) return -1;
            }
            if (quote_wrap_extra > 0) {
                if (ansi_write_spaces(impl, sink, quote_wrap_extra) != 0) return -1;
            }
        }
        if (!quote_prefix_emitted) {
            impl->ansi_col += impl->quote_prefix_indent + (quote_depth * 2) + (int)quote_wrap_extra;
        }
    }
    impl->ansi_prev_char = ' ';
    impl->ansi_line_has_space = 1;
    impl->pending_quote_reopen_prefix = 1;
    return 0;
}

int ansi_emit_quote_blank_newline(mdf_impl *impl, mdf_sink *sink)
{
    int i;

    if (ansi_close_quote_text(impl, sink) != 0) return -1;
    ansi_reset_line_output_state(impl);
    if (ansi_emit_pending_style_reset(impl, sink) != 0) return -1;
    if (ansi_emit_raw_newline(impl, sink) != 0) {
        return -1;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (impl->quote_prefix_indent > 0) {
        for (i = 0; i < impl->quote_prefix_indent; i++) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        }
    }
    for (i = 0; i < (impl->quote_depth > 0 ? impl->quote_depth : 1); i++) {
        if (ansi_emit_quote_marker(impl, sink, 0) != 0) return -1;
        impl->ansi_col++;
        if (i + 1 < (impl->quote_depth > 0 ? impl->quote_depth : 1)) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            impl->ansi_col++;
        }
    }
    impl->ansi_prev_char = '>';
    return 0;
}

static int ansi_emit_list_quote_blank(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (impl->pending_list_quote_blank_indent > 0) {
        size_t i;
        for (i = 0; i < (size_t)impl->pending_list_quote_blank_indent; i++) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        }
    }
    if (ansi_emit_pending_style_reset(impl, sink) != 0) return -1;
    if (ansi_emit_quote_marker(impl, sink, 0) != 0) return -1;
    impl->pending_list_quote_blank_indent = 0;
    ansi_reset_line_output_state(impl);
    return ansi_emit_raw_newline(impl, sink);
}

int ansi_write_newline(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_word(impl, sink) != 0) {
        return -1;
    }
    return ansi_emit_plain_newline(impl, sink);
}

void ansi_update_visible_output_state(mdf_impl *impl, const char *src, size_t len)
{
    size_t i;

    i = 0;
    while (i < len) {
        unsigned long cp;
        size_t adv;

        adv = utf8_decode_codepoint(src + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        impl->ansi_col += (int)utf8_display_width(cp);
        if (cp == ' ') {
            impl->ansi_line_has_space = 1;
        }
        impl->ansi_prev_char = src[i + adv - 1];
        i += adv;
    }
}

int ansi_emit_visible_chunk(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    const char *reset;
    const char *quote_text;
    size_t reset_len;
    size_t quote_text_len;

    if (len == 0) {
        return 0;
    }
    if (src[0] != '\n' && ansi_ensure_left_margin(impl, sink) != 0) {
        return -1;
    }
    if (impl->opts.boring) {
        impl->heading_style_pending_prefix = 0;
        impl->ansi_pending_inline_style = NULL;
        impl->ansi_pending_style_reset = 0;
        impl->ansi_pending_attached_style = NULL;
    }
    reset = "";
    reset_len = 0;
    if (impl->ansi_pending_style_reset && !impl->opts.boring) {
        reset = "\033[0m";
        reset_len = 4;
        impl->quote_text_open = 0;
    }
    quote_text = "";
    quote_text_len = 0;
    if (ansi_quote_text_active(impl) &&
        (impl->ansi_col == ansi_current_prefix_width(impl) ||
         impl->ansi_col + 1 == ansi_current_prefix_width(impl)) &&
        len > 0 &&
        src[0] == ' ') {
        if (mdf_emit_all(impl, sink, " ", 1) != 0) {
            return -1;
        }
        ansi_update_visible_output_state(impl, " ", 1);
        return ansi_emit_visible_chunk(impl, sink, src + 1, len - 1);
    }
    if (!impl->quote_text_open &&
        impl->heading_style_pending_prefix == 0 &&
        impl->ansi_pending_inline_style == NULL &&
        (impl->ansi_active_inline_style == NULL ||
         impl->ansi_pending_style_reset ||
         (len > 0 && (src[0] == ':' || src[0] == ';' || src[0] == ',' ||
                      src[0] == '.' || src[0] == '!' || src[0] == '?'))) &&
        ansi_quote_text_active(impl)) {
        quote_text = mdf_theme_quote_text(impl);
        quote_text_len = strlen(quote_text);
    }
    if ((impl->heading_style_pending_prefix && !impl->opts.boring) ||
        (impl->ansi_pending_inline_style != NULL && !impl->opts.boring) ||
        reset_len > 0 ||
        quote_text_len > 0) {
        const char *style;
        const char *inline_style;
        size_t style_len;
        size_t inline_style_len;

        style = (impl->heading_style_pending_prefix && !impl->opts.boring) ? ansi_heading_style(impl, impl->heading_level) : "";
        inline_style = (impl->ansi_pending_inline_style != NULL && !impl->opts.boring) ? impl->ansi_pending_inline_style : "";
        style_len = strlen(style);
        inline_style_len = strlen(inline_style);
        if (mdf_emit_buffer_reset(impl) != 0 ||
            mdf_emit_buffer_append(impl, reset, reset_len) != 0 ||
            mdf_emit_buffer_append(impl, style, style_len) != 0 ||
            mdf_emit_buffer_append(impl, inline_style, inline_style_len) != 0 ||
            mdf_emit_buffer_append(impl, quote_text, quote_text_len) != 0 ||
            mdf_emit_buffer_append(impl, src, len) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->heading_style_pending_prefix = 0;
        impl->ansi_pending_inline_style = NULL;
        if (inline_style_len > 0) {
            impl->ansi_active_inline_style = inline_style;
        }
        impl->ansi_pending_style_reset = 0;
        impl->ansi_pending_attached_style = NULL;
        impl->quote_text_open = quote_text_len > 0;
        if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    } else {
        if (mdf_emit_all(impl, sink, src, len) != 0) {
            return -1;
        }
    }
    ansi_update_visible_output_state(impl, src, len);
    return 0;
}

int ansi_emit_styled_visible_chunk(mdf_impl *impl, mdf_sink *sink, const char *style, const char *src, size_t len)
{
    size_t style_len;
    const char *reset;
    const char *quote_text;
    size_t reset_len;
    size_t quote_text_len;

    if (len > 0 && src[0] != '\n' && ansi_ensure_left_margin(impl, sink) != 0) {
        return -1;
    }
    if ((style == NULL || style[0] == '\0' || impl->opts.boring) &&
        !impl->ansi_pending_style_reset &&
        !impl->quote_text_open) {
        return ansi_emit_visible_chunk(impl, sink, src, len);
    }
    reset = "";
    reset_len = 0;
    if ((impl->ansi_pending_style_reset || impl->quote_text_open) && !impl->opts.boring) {
        reset = "\033[0m";
        reset_len = 4;
    }
    if (impl->ansi_pending_inline_style != NULL && style == NULL) {
        style = impl->ansi_pending_inline_style;
    }
    style_len = (style == NULL || impl->opts.boring) ? 0 : strlen(style);
    if (style_len == 0 && reset_len == 0) {
        return ansi_emit_visible_chunk(impl, sink, src, len);
    }
    quote_text = "";
    quote_text_len = 0;
    if (reset_len > 0 && style_len == 0 && len > 0 && src[0] != ' ' && ansi_quote_text_active(impl)) {
        quote_text = mdf_theme_quote_text(impl);
        quote_text_len = strlen(quote_text);
    }
    if (style_len > 0) {
        impl->heading_style_pending_prefix = 0;
    }
    {
        if (mdf_emit_buffer_reset(impl) != 0 ||
            mdf_emit_buffer_append(impl, reset, reset_len) != 0 ||
            mdf_emit_buffer_append(impl, style, style_len) != 0 ||
            mdf_emit_buffer_append(impl, quote_text, quote_text_len) != 0 ||
            mdf_emit_buffer_append(impl, src, len) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->ansi_pending_style_reset = 0;
        if (style == impl->ansi_pending_inline_style) {
            impl->ansi_pending_inline_style = NULL;
        }
        if (reset_len > 0) {
            impl->ansi_pending_attached_style = NULL;
        }
        impl->quote_text_open = quote_text_len > 0;
        if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    }
    ansi_update_visible_output_state(impl, src, len);
    return 0;
}

int ansi_emit_styled_prefix(mdf_impl *impl, mdf_sink *sink, const char *style)
{
    if (ansi_ensure_left_margin(impl, sink) != 0) {
        return -1;
    }
    if (ansi_emit_pending_style_reset(impl, sink) != 0) {
        return -1;
    }
    if (ansi_close_quote_text(impl, sink) != 0) {
        return -1;
    }
    if (style == NULL || style[0] == '\0' || impl->opts.boring) {
        return 0;
    }
    impl->heading_style_pending_prefix = 0;
    return mdf_emit_cstr(impl, sink, style);
}

static int ansi_emit_styled_word_reset_suffix(mdf_impl *impl, mdf_sink *sink, const char *style,
                                              const char *text, size_t text_len,
                                              const char *suffix, size_t suffix_len)
{
    size_t style_len;
    size_t prefix_reset_len;
    size_t reset_len;
    const char *quote_text;
    size_t quote_text_len;

    style_len = (style == NULL || impl->opts.boring) ? 0 : strlen(style);
    prefix_reset_len = (!impl->opts.boring && style_len > 0 && impl->quote_text_open) ? 4 : 0;
    reset_len = impl->opts.boring ? 0 : 4;
    quote_text = "";
    quote_text_len = 0;
    if (suffix_len > 0 && suffix[0] != ' ' && reset_len > 0 && ansi_quote_text_active(impl)) {
        quote_text = mdf_theme_quote_text(impl);
        quote_text_len = strlen(quote_text);
    }
    if (text_len > 0 && ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (mdf_emit_buffer_reset(impl) != 0) return -1;
    if (prefix_reset_len > 0) {
        if (mdf_emit_buffer_append(impl, "\033[0m", prefix_reset_len) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->quote_text_open = 0;
    }
    if (style_len > 0) {
        if (mdf_emit_buffer_append(impl, style, style_len) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->heading_style_pending_prefix = 0;
    }
    if (mdf_emit_buffer_append(impl, text, text_len) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (reset_len > 0) {
        if (mdf_emit_buffer_append(impl, "\033[0m", reset_len) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
    }
    if (mdf_emit_buffer_append(impl, quote_text, quote_text_len) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (mdf_emit_buffer_append(impl, suffix, suffix_len) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    impl->quote_text_open = quote_text_len > 0;
    if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
    ansi_update_visible_output_state(impl, text, text_len);
    ansi_update_visible_output_state(impl, suffix, suffix_len);
    return 0;
}

static int ansi_write_word_byte(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (!impl->ansi_flushing_word && !impl->in_pre && impl->opts.width > 0 && !utf8_continuation_byte(c) && impl->ansi_col > impl->opts.width) {
        if (ansi_emit_newline(impl, sink) != 0) return -1;
    }
    return ansi_emit_visible_chunk(impl, sink, &c, 1);
}

int ansi_write_spaces(mdf_impl *impl, mdf_sink *sink, size_t count)
{
    static const char spaces[] =
        "                                                                ";

    if (count > 0 && ansi_ensure_left_margin(impl, sink) != 0) {
        return -1;
    }
    while (count > 0) {
        size_t chunk;

        chunk = count;
        if (chunk > sizeof(spaces) - 1) {
            chunk = sizeof(spaces) - 1;
        }
        if (mdf_emit_all(impl, sink, spaces, chunk) != 0) {
            return -1;
        }
        impl->ansi_col += (int)chunk;
        impl->ansi_prev_char = ' ';
        impl->ansi_line_has_space = 1;
        count -= chunk;
    }
    return 0;
}

typedef struct ansi_pending_emit_sink {
    mdf_impl *impl;
    int oom;
} ansi_pending_emit_sink;

static int ansi_pending_emit_append(mdf_impl *impl, const char *src, size_t len)
{
    char *next;
    size_t *next_offsets;
    size_t need;
    size_t cap;

    if (len == 0) {
        return 0;
    }
    if (len > ((size_t)-1) - impl->ansi_pending_emit_len) {
        return -1;
    }
    need = impl->ansi_pending_emit_len + len;
    if (need > impl->ansi_pending_emit_cap) {
        cap = impl->ansi_pending_emit_cap == 0 ? 32 : impl->ansi_pending_emit_cap;
        while (cap < need) {
            if (cap > ((size_t)-1) / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(&impl->allocator,
                                       impl->ansi_pending_emit,
                                       impl->ansi_pending_emit_cap,
                                       cap);
        if (next == NULL) {
            return -1;
        }
        impl->ansi_pending_emit = next;
        impl->ansi_pending_emit_cap = cap;
    }
    memcpy(impl->ansi_pending_emit + impl->ansi_pending_emit_len, src, len);
    impl->ansi_pending_emit_len += len;
    if (impl->ansi_pending_emit_offsets_len == impl->ansi_pending_emit_offsets_cap) {
        cap = impl->ansi_pending_emit_offsets_cap == 0 ? 8 : impl->ansi_pending_emit_offsets_cap * 2;
        next_offsets = (size_t *)mdf_realloc_mem(&impl->allocator,
                                                 impl->ansi_pending_emit_offsets,
                                                 impl->ansi_pending_emit_offsets_cap * sizeof(impl->ansi_pending_emit_offsets[0]),
                                                 cap * sizeof(impl->ansi_pending_emit_offsets[0]));
        if (next_offsets == NULL) {
            return -1;
        }
        impl->ansi_pending_emit_offsets = next_offsets;
        impl->ansi_pending_emit_offsets_cap = cap;
    }
    impl->ansi_pending_emit_offsets[impl->ansi_pending_emit_offsets_len++] = impl->ansi_pending_emit_len;
    impl->ansi_pending_emit_valid = 1;
    return 0;
}

static int ansi_pending_emit_write(void *userdata, const char *src, size_t len)
{
    ansi_pending_emit_sink *pending;

    pending = (ansi_pending_emit_sink *)userdata;
    if (ansi_pending_emit_append(pending->impl, src, len) != 0) {
        pending->oom = 1;
        return -1;
    }
    return 0;
}

static int ansi_pending_emit_line_cols(mdf_impl *impl)
{
    size_t i;
    int cols;
    int esc;

    cols = impl->ansi_pending_emit_start_col;
    esc = 0;
    i = 0;
    while (i < impl->ansi_pending_emit_len) {
        unsigned long cp;
        size_t adv;
        unsigned char ch;

        ch = (unsigned char)impl->ansi_pending_emit[i];
        if (esc) {
            if ((ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                ch == '\\') {
                esc = 0;
            }
            i++;
            continue;
        }
        if (ch == 0x1b) {
            esc = 1;
            i++;
            continue;
        }
        if (ch == '\n') {
            cols = 0;
            i++;
            continue;
        }
        adv = utf8_decode_codepoint(impl->ansi_pending_emit + i,
                                    impl->ansi_pending_emit_len - i,
                                    &cp);
        if (adv == 0) {
            break;
        }
        cols += (int)utf8_display_width(cp);
        i += adv;
    }
    return cols;
}

static void ansi_pending_emit_clear(mdf_impl *impl)
{
    impl->ansi_pending_emit_valid = 0;
    impl->ansi_pending_emit_len = 0;
    impl->ansi_pending_emit_offsets_len = 0;
    impl->ansi_pending_emit_start_col = 0;
    impl->ansi_pending_emit_leading_space = 0;
}

static int ansi_flush_pending_code_emit(mdf_impl *impl, mdf_sink *sink)
{
    size_t i;
    size_t start;

    if (!impl->ansi_pending_emit_valid || impl->ansi_pending_emit_len == 0) {
        ansi_pending_emit_clear(impl);
        return 0;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) {
        ansi_pending_emit_clear(impl);
        return -1;
    }
    start = 0;
    for (i = 0; i < impl->ansi_pending_emit_offsets_len; i++) {
        size_t end;

        end = impl->ansi_pending_emit_offsets[i];
        if (end > impl->ansi_pending_emit_len || end < start) {
            ansi_pending_emit_clear(impl);
            return -1;
        }
        if (end > start &&
            mdf_emit_all(impl, sink, impl->ansi_pending_emit + start, end - start) != 0) {
            ansi_pending_emit_clear(impl);
            return -1;
        }
        start = end;
    }
    if (start < impl->ansi_pending_emit_len &&
        mdf_emit_all(impl, sink, impl->ansi_pending_emit + start, impl->ansi_pending_emit_len - start) != 0) {
        ansi_pending_emit_clear(impl);
        return -1;
    }
    ansi_pending_emit_clear(impl);
    return 0;
}

static int ansi_pending_code_can_move_to_continuation(mdf_impl *impl, int code_cols)
{
    size_t text_start;
    int continuation_col;

    text_start = impl->ansi_pending_emit_leading_space ? 1 : 0;
    if (impl->opts.width <= 0 ||
        impl->ansi_pending_emit_len == 0 ||
        code_cols < 8 ||
        memchr(impl->ansi_pending_emit + text_start, ' ', impl->ansi_pending_emit_len - text_start) != NULL ||
        memchr(impl->ansi_pending_emit, '\n', impl->ansi_pending_emit_len) != NULL) {
        return 0;
    }
    continuation_col = ansi_current_prefix_width(impl);
    if (impl->ansi_wrap_indent > continuation_col) {
        continuation_col = impl->ansi_wrap_indent;
    }
    return continuation_col + code_cols + 1 <= impl->opts.width ||
           code_cols + 1 <= impl->opts.width;
}

static int ansi_emit_pending_code_with_punct_on_continuation(mdf_impl *impl, mdf_sink *sink, char c, int code_cols)
{
    size_t start;
    int saved_reset;

    start = impl->ansi_pending_emit_leading_space ? 1 : 0;
    saved_reset = impl->ansi_pending_style_reset;
    impl->ansi_pending_style_reset = 0;
    if (ansi_emit_newline(impl, sink) != 0) {
        impl->ansi_pending_style_reset = saved_reset;
        return -1;
    }
    if (saved_reset && !impl->opts.boring) {
        if (ansi_pending_emit_append(impl, "\033[0m", 4) != 0) {
            mdf_impl_mark_oom(impl);
            ansi_pending_emit_clear(impl);
            return -1;
        }
    }
    if (ansi_pending_emit_append(impl, &c, 1) != 0) {
        mdf_impl_mark_oom(impl);
        ansi_pending_emit_clear(impl);
        return -1;
    }
    if (mdf_emit_all(impl, sink, impl->ansi_pending_emit + start, impl->ansi_pending_emit_len - start) != 0) {
        ansi_pending_emit_clear(impl);
        return -1;
    }
    ansi_pending_emit_clear(impl);
    impl->ansi_col += code_cols + 1;
    impl->ansi_prev_char = c;
    impl->ansi_line_has_space = 1;
    return 0;
}

static int ansi_emit_pending_code_with_punct(mdf_impl *impl, mdf_sink *sink, char c)
{
    int replay_chunks;
    int line_cols;
    int code_cols;
    int code_cols_without_leading_space;

    if (!impl->ansi_pending_emit_valid || impl->ansi_pending_emit_len == 0) {
        return 0;
    }
    line_cols = ansi_pending_emit_line_cols(impl);
    code_cols = line_cols - impl->ansi_pending_emit_start_col;
    if (code_cols < 0) {
        code_cols = line_cols;
    }
    code_cols_without_leading_space = code_cols;
    if (impl->ansi_pending_emit_leading_space && code_cols_without_leading_space > 0) {
        code_cols_without_leading_space--;
    }
    if (impl->opts.width > 0 &&
        (line_cols + 1 > impl->opts.width ||
         (impl->ansi_wrap_indent > 0 &&
          impl->ansi_wrap_indent + code_cols_without_leading_space + 1 > impl->opts.width) ||
         code_cols_without_leading_space + 1 > impl->opts.width - 2)) {
        if (ansi_pending_code_can_move_to_continuation(impl, code_cols_without_leading_space)) {
            return ansi_emit_pending_code_with_punct_on_continuation(impl, sink, c, code_cols_without_leading_space);
        }
        if (ansi_flush_pending_code_emit(impl, sink) != 0) return -1;
        if (ansi_emit_newline(impl, sink) != 0) return -1;
        return ansi_emit_visible_chunk(impl, sink, &c, 1);
    }
    replay_chunks = code_cols != 1 ||
                    impl->ansi_pending_emit_offsets_len > 1 ||
                    memchr(impl->ansi_pending_emit, '\n', impl->ansi_pending_emit_len) != NULL;
    if (replay_chunks) {
        if (ansi_flush_pending_code_emit(impl, sink) != 0) return -1;
        if (impl->ansi_pending_style_reset && !impl->opts.boring) {
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, "\033[0m", 4) != 0 ||
                mdf_emit_buffer_append(impl, &c, 1) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            impl->ansi_pending_style_reset = 0;
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
        } else if (mdf_emit_all(impl, sink, &c, 1) != 0) {
            return -1;
        }
        impl->ansi_col++;
        impl->ansi_prev_char = c;
        impl->ansi_line_has_space = 1;
        return 0;
    }
    if (impl->ansi_pending_style_reset && !impl->opts.boring) {
        if (ansi_pending_emit_append(impl, "\033[0m", 4) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->ansi_pending_style_reset = 0;
    }
    if (ansi_pending_emit_append(impl, &c, 1) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    impl->ansi_col++;
    impl->ansi_prev_char = c;
    impl->ansi_line_has_space = 1;
    if (mdf_emit_all(impl, sink, impl->ansi_pending_emit, impl->ansi_pending_emit_len) != 0) {
        ansi_pending_emit_clear(impl);
        return -1;
    }
    ansi_pending_emit_clear(impl);
    return 0;
}

static int ansi_flush_pending_autolink_emit(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->ansi_pending_autolink_emit_valid) {
        return 0;
    }
    if (impl->ansi_pending_emit_len > 0 &&
        ansi_ensure_left_margin(impl, sink) != 0) {
        impl->ansi_pending_autolink_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    if (impl->ansi_pending_emit_len > 0 &&
        mdf_emit_all(impl, sink, impl->ansi_pending_emit, impl->ansi_pending_emit_len) != 0) {
        impl->ansi_pending_autolink_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    impl->ansi_pending_autolink_emit_valid = 0;
    ansi_pending_emit_clear(impl);
    return 0;
}

static int ansi_emit_pending_autolink_with_punct(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (!impl->ansi_pending_autolink_emit_valid) {
        return 0;
    }
    if (impl->ansi_pending_style_reset && !impl->opts.boring) {
        if (ansi_pending_emit_append(impl, "\033[0m", 4) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->ansi_pending_style_reset = 0;
    }
    if (ansi_pending_emit_append(impl, &c, 1) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) {
        impl->ansi_pending_autolink_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    impl->ansi_col++;
    impl->ansi_prev_char = c;
    impl->ansi_line_has_space = 1;
    if (mdf_emit_all(impl, sink, impl->ansi_pending_emit, impl->ansi_pending_emit_len) != 0) {
        impl->ansi_pending_autolink_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    impl->ansi_pending_autolink_emit_valid = 0;
    ansi_pending_emit_clear(impl);
    return 0;
}

static int ansi_flush_pending_fallback_emit(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->ansi_pending_fallback_emit_valid) {
        return 0;
    }
    if (impl->ansi_pending_emit_len > 0 &&
        ansi_ensure_left_margin(impl, sink) != 0) {
        impl->ansi_pending_fallback_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    if (impl->ansi_pending_emit_len > 0 &&
        mdf_emit_all(impl, sink, impl->ansi_pending_emit, impl->ansi_pending_emit_len) != 0) {
        impl->ansi_pending_fallback_emit_valid = 0;
        ansi_pending_emit_clear(impl);
        return -1;
    }
    impl->ansi_pending_fallback_emit_valid = 0;
    ansi_pending_emit_clear(impl);
    return 0;
}

static int ansi_emit_pending_fallback_with_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (!impl->ansi_pending_fallback_emit_valid) {
        return 0;
    }
    if (ansi_pending_emit_append(impl, &c, 1) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    impl->ansi_col++;
    impl->ansi_prev_char = c;
    impl->ansi_line_has_space = 1;
    return ansi_flush_pending_fallback_emit(impl, sink);
}

int ansi_emit_quote_prefix(mdf_impl *impl, mdf_sink *sink, size_t prefix_spaces, size_t extra_spaces)
{
    char buf[128];
    size_t off;
    size_t i;

    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    off = 0;
    if (prefix_spaces + extra_spaces + 16 > sizeof(buf)) {
        if (prefix_spaces > 0 && ansi_write_spaces(impl, sink, prefix_spaces) != 0) {
            return -1;
        }
        if (ansi_emit_quote_marker(impl, sink, 1) != 0) return -1;
        if (extra_spaces > 0 && ansi_write_spaces(impl, sink, extra_spaces) != 0) {
            return -1;
        }
        impl->ansi_col += 2;
        impl->ansi_prev_char = ' ';
        impl->ansi_line_has_space = 1;
        return 0;
    }
    for (i = 0; i < prefix_spaces; i++) {
        buf[off++] = ' ';
    }
    if (impl->opts.boring) {
        memcpy(buf + off, "> ", 2);
        off += 2;
    } else if (strcmp(mdf_theme_quote(impl), "\033[90m") == 0) {
        memcpy(buf + off, "\033[90m>\033[0m ", 11);
        off += 11;
    } else {
        if (off > 0 && mdf_emit_all(impl, sink, buf, off) != 0) return -1;
        if (ansi_emit_quote_marker(impl, sink, 1) != 0) return -1;
        off = 0;
    }
    for (i = 0; i < extra_spaces; i++) {
        buf[off++] = ' ';
    }
    if (mdf_emit_all(impl, sink, buf, off) != 0) {
        return -1;
    }
    impl->ansi_col += (int)(prefix_spaces + 2 + extra_spaces);
    impl->ansi_prev_char = ' ';
    impl->ansi_line_has_space = 1;
    return 0;
}

int ansi_emit_quote_prefix_split(mdf_impl *impl, mdf_sink *sink, size_t prefix_spaces, size_t extra_spaces)
{
    size_t i;

    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    for (i = 0; i < prefix_spaces; i++) {
        if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        impl->ansi_col++;
    }
    if (impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, ">") != 0) return -1;
    } else if (ansi_emit_quote_marker(impl, sink, 0) != 0) return -1;
    impl->ansi_col++;
    impl->ansi_prev_char = '>';
    if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
    impl->ansi_col++;
    for (i = 0; i < extra_spaces; i++) {
        if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        impl->ansi_col++;
    }
    impl->ansi_prev_char = ' ';
    impl->ansi_line_has_space = 1;
    return 0;
}

static int ansi_flush_word_reserved(mdf_impl *impl, mdf_sink *sink, size_t trailing_reserve)
{
    size_t i;
    int has_utf8;
    int prefix_only_line;
    int preserve_prefix_line;
    const char *final_emph_style;

    if (impl->ansi_word_len == 0) {
        if (ansi_flush_pending_code_emit(impl, sink) != 0) {
            return -1;
        }
        return 0;
    }
    if (ansi_flush_pending_code_emit(impl, sink) != 0) {
        return -1;
    }
    impl->ansi_punct_quote_pending = 0;
    final_emph_style = impl->ansi_pending_final_emph_style;
    if (final_emph_style != NULL && !impl->ansi_pending_final_emph_suffix) {
        impl->ansi_pending_final_emph_style = NULL;
        impl->ansi_pending_final_emph_suffix = 0;
        impl->ansi_pending_final_emph_word_suffix = 0;
        impl->ansi_pending_final_emph_base_len = 0;
        impl->ansi_pending_final_emph_base_cols = 0;
        if (!impl->opts.boring &&
            impl->ansi_active_inline_style != final_emph_style &&
            impl->ansi_pending_inline_style != final_emph_style) {
            impl->ansi_pending_attached_style = final_emph_style;
        }
    }
    prefix_only_line = impl->opts.width > 0 &&
                       impl->ansi_col > 0 &&
                       !impl->ansi_pending_space &&
                       impl->ansi_col == ansi_current_prefix_width(impl);
    if (impl->ansi_pending_space) {
        int can_split_after_space;
        int no_split_after_space;
        int plain_space;

        no_split_after_space = impl->ansi_pending_space_no_split;
        plain_space = impl->ansi_pending_space_plain;
        impl->ansi_pending_space = 0;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 0;
        can_split_after_space = 0;
        if (impl->opts.width > 0 && impl->ansi_col > 0 &&
            impl->ansi_col + 1 + (int)(impl->ansi_word_cols + trailing_reserve) > impl->opts.width &&
            !no_split_after_space &&
            ansi_word_has_url_scheme(impl->ansi_word, impl->ansi_word_len) &&
            !ansi_word_contains_byte(impl->ansi_word, impl->ansi_word_len, '<') &&
            ansi_word_has_break_before_cols(impl->ansi_word, impl->ansi_word_len, impl->opts.width - impl->ansi_col - 1)) {
            can_split_after_space = 1;
        }
        if (impl->opts.width > 0 && impl->ansi_col > 0 &&
            impl->ansi_col + 1 + (int)(impl->ansi_word_cols + trailing_reserve) > impl->opts.width &&
            !can_split_after_space) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        } else {
            if (impl->heading_open && impl->heading_style_suspended && !impl->opts.boring) {
                impl->ansi_pending_style_reset = 1;
                impl->heading_style_pending_prefix = 1;
                impl->heading_style_suspended = 0;
            }
            if (ansi_quote_text_active(impl) &&
                impl->ansi_col == ansi_current_prefix_width(impl)) {
                if (mdf_emit_all(impl, sink, " ", 1) != 0) return -1;
                impl->ansi_col++;
                impl->ansi_prev_char = ' ';
                impl->ansi_line_has_space = 1;
            } else if (plain_space) {
                if (mdf_emit_all(impl, sink, " ", 1) != 0) return -1;
                impl->ansi_col++;
                impl->ansi_prev_char = ' ';
                impl->ansi_line_has_space = 1;
            } else if (impl->ansi_pending_style_reset ||
                (!impl->opts.boring && impl->ansi_pending_inline_style != NULL)) {
                if (ansi_emit_visible_chunk(impl, sink, " ", 1) != 0) return -1;
            } else if (ansi_write_word_byte(impl, sink, ' ') != 0) return -1;
        }
    } else if (impl->opts.width > 0 && impl->ansi_col > 0 &&
               impl->ansi_col + (int)(impl->ansi_word_cols + trailing_reserve) > impl->opts.width &&
               !prefix_only_line) {
        if (ansi_emit_newline(impl, sink) != 0) return -1;
    }
    if (impl->heading_open && impl->heading_style_suspended && !impl->opts.boring) {
        impl->ansi_pending_style_reset = 1;
        impl->heading_style_pending_prefix = 1;
        impl->heading_style_suspended = 0;
    }
    has_utf8 = 0;
    for (i = 0; i < impl->ansi_word_len; i++) {
        if (((unsigned char)impl->ansi_word[i]) >= 0x80) {
            has_utf8 = 1;
            break;
        }
    }
    impl->ansi_flushing_word = has_utf8;
    preserve_prefix_line = impl->opts.width > 0 &&
                           impl->ansi_col > 0 &&
                           !impl->ansi_pending_space &&
                           impl->ansi_col == ansi_current_prefix_width(impl);
    if (final_emph_style != NULL && impl->ansi_pending_final_emph_suffix) {
        const char *emit_style;
        size_t total_word_cols;

        if (impl->ansi_pending_final_emph_base_len > impl->ansi_word_len) {
            impl->ansi_pending_final_emph_base_len = impl->ansi_word_len;
            impl->ansi_pending_final_emph_base_cols = impl->ansi_word_cols;
        }
        emit_style = (!impl->opts.boring &&
                      impl->ansi_active_inline_style == final_emph_style &&
                      impl->ansi_pending_inline_style != final_emph_style) ? "" : final_emph_style;
        total_word_cols = visible_cols(impl->ansi_word, impl->ansi_word_len);
        if (impl->opts.width > 0 && total_word_cols > (size_t)impl->opts.width) {
            if (ansi_write_styled_split_word_full_width(impl, sink, emit_style,
                    impl->ansi_word, impl->ansi_word_len) != 0) {
                impl->ansi_flushing_word = 0;
                return -1;
            }
            impl->ansi_pending_style_reset = 1;
        } else if (ansi_emit_styled_word_reset_suffix(impl, sink, emit_style,
                impl->ansi_word,
                impl->ansi_pending_final_emph_base_len,
                impl->ansi_word + impl->ansi_pending_final_emph_base_len,
                impl->ansi_word_len - impl->ansi_pending_final_emph_base_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
        impl->ansi_pending_final_emph_style = NULL;
        impl->ansi_pending_final_emph_suffix = 0;
        impl->ansi_pending_final_emph_word_suffix = 0;
        impl->ansi_pending_final_emph_base_len = 0;
        impl->ansi_pending_final_emph_base_cols = 0;
        impl->ansi_active_inline_style = NULL;
        impl->ansi_pending_inline_style = NULL;
        if (impl->heading_open) {
            impl->heading_style_suspended = 1;
        }
        final_emph_style = NULL;
    } else if (impl->ansi_pending_attached_style != NULL && !impl->opts.boring) {
        const char *style;

        style = impl->ansi_pending_attached_style;
        impl->ansi_pending_attached_style = NULL;
        impl->ansi_pending_style_reset = 0;
        if (impl->opts.width > 0 && impl->ansi_word_cols > (size_t)impl->opts.width) {
            if (ansi_emit_styled_prefix(impl, sink, style) != 0) {
                impl->ansi_flushing_word = 0;
                return -1;
            }
            if (ansi_write_split_word_full_width(impl, sink, impl->ansi_word, impl->ansi_word_len) != 0) {
                impl->ansi_flushing_word = 0;
                return -1;
            }
        } else if (ansi_emit_styled_visible_chunk(impl, sink, style, impl->ansi_word, impl->ansi_word_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
        impl->ansi_pending_style_reset = 1;
    } else if (preserve_prefix_line && impl->opts.width > 0 &&
        impl->ansi_col > 0 &&
        impl->ansi_col + (int)impl->ansi_word_cols > impl->opts.width) {
        if (ansi_write_split_word_preserve_prefix(impl, sink, impl->ansi_word, impl->ansi_word_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
    } else if (impl->opts.width > 0 && impl->ansi_word_cols > (size_t)impl->opts.width) {
        if (ansi_write_split_word_full_width(impl, sink, impl->ansi_word, impl->ansi_word_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
    } else if (!has_utf8 && impl->opts.width > 0 &&
        impl->ansi_col > 0 &&
        impl->ansi_col + (int)impl->ansi_word_cols > impl->opts.width &&
        ansi_word_split_offset(impl->ansi_word, impl->ansi_word_len, impl->opts.width - impl->ansi_col) > 0) {
        if (ansi_write_split_word_bytes(impl, sink, impl->ansi_word, impl->ansi_word_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
    } else {
        size_t split;

        split = 0;
        if (impl->ansi_word_len >= 3) {
            size_t k;

            for (k = 1; k + 1 < impl->ansi_word_len; k++) {
                if (impl->ansi_word[k] == ':' &&
                    ascii_is_digit_char(impl->ansi_word[k - 1]) &&
                    ascii_is_digit_char(impl->ansi_word[k + 1])) {
                    split = k + 1;
                    break;
                }
            }
        }
        if (split > 0) {
            if (ansi_emit_visible_chunk(impl, sink, impl->ansi_word, split) != 0) {
                impl->ansi_flushing_word = 0;
                return -1;
            }
            if (ansi_emit_visible_chunk(impl, sink, impl->ansi_word + split, impl->ansi_word_len - split) != 0) {
                impl->ansi_flushing_word = 0;
                return -1;
            }
        } else if ((ansi_word_has_url_scheme(impl->ansi_word, impl->ansi_word_len) ||
                    (impl->ansi_word_len >= 2 && impl->ansi_word[0] == '/' && impl->ansi_word[1] == '/')) &&
                   ansi_word_contains_byte(impl->ansi_word, impl->ansi_word_len, '?')) {
            size_t q;

            for (q = 0; q < impl->ansi_word_len; q++) {
                if (impl->ansi_word[q] == '?' && q + 1 < impl->ansi_word_len) {
                    if (ansi_emit_visible_chunk(impl, sink, impl->ansi_word, q + 1) != 0) {
                        impl->ansi_flushing_word = 0;
                        return -1;
                    }
                    if (ansi_emit_visible_chunk(impl, sink, impl->ansi_word + q + 1, impl->ansi_word_len - q - 1) != 0) {
                        impl->ansi_flushing_word = 0;
                        return -1;
                    }
                    break;
                }
            }
        } else if (ansi_emit_visible_chunk(impl, sink, impl->ansi_word, impl->ansi_word_len) != 0) {
            impl->ansi_flushing_word = 0;
            return -1;
        }
    }
    impl->ansi_flushing_word = 0;
    impl->ansi_word_len = 0;
    impl->ansi_word_cols = 0;
    if (impl->ansi_pending_style_reset_after_word) {
        impl->ansi_pending_style_reset_after_word = 0;
        impl->ansi_active_inline_style = NULL;
        impl->ansi_pending_inline_style = NULL;
        impl->ansi_pending_style_reset = 1;
    }
    if (final_emph_style != NULL) {
        impl->ansi_active_inline_style = NULL;
        impl->ansi_pending_inline_style = NULL;
        impl->ansi_pending_style_reset = 1;
    }
    return 0;
}

int ansi_flush_word(mdf_impl *impl, mdf_sink *sink)
{
    return ansi_flush_word_reserved(impl, sink, 0);
}

static int ansi_flush_pending_space_for(mdf_impl *impl, mdf_sink *sink, size_t next_len)
{
    int plain_space;

    if (ansi_flush_word(impl, sink) != 0) {
        return -1;
    }
    if (!impl->ansi_pending_space) {
        return 0;
    }
    plain_space = impl->ansi_pending_space_plain;
    impl->ansi_pending_space = 0;
    impl->ansi_pending_space_no_split = 0;
    impl->ansi_pending_space_plain = 0;
    if (impl->in_pre) {
        impl->ansi_col++;
        impl->ansi_prev_char = ' ';
        return mdf_emit_cstr(impl, sink, " ");
    }
    if (impl->opts.width > 0 && impl->ansi_col > 0 && impl->ansi_col + 1 + (int)next_len > impl->opts.width) {
        return ansi_emit_newline(impl, sink);
    }
    if (plain_space) {
        if (mdf_emit_all(impl, sink, " ", 1) != 0) return -1;
        impl->ansi_col++;
        impl->ansi_prev_char = ' ';
        impl->ansi_line_has_space = 1;
        return 0;
    }
    return ansi_emit_visible_chunk(impl, sink, " ", 1);
}

static int ansi_flush_pending_space_only_for(mdf_impl *impl, mdf_sink *sink, size_t next_len)
{
    int list_prefix_only_line;
    int plain_space;

    if (!impl->ansi_pending_space) {
        return 0;
    }
    plain_space = impl->ansi_pending_space_plain;
    list_prefix_only_line = impl->list_item_open &&
                            impl->ansi_wrap_indent > 0 &&
                            impl->ansi_col + 1 == impl->ansi_wrap_indent;
    impl->ansi_pending_space = 0;
    impl->ansi_pending_space_no_split = 0;
    impl->ansi_pending_space_plain = 0;
    if (impl->in_pre) {
        impl->ansi_col++;
        impl->ansi_prev_char = ' ';
        return mdf_emit_cstr(impl, sink, " ");
    }
    if (impl->opts.width > 0 && impl->ansi_col > 0 && impl->ansi_col + 1 + (int)next_len > impl->opts.width) {
        if (list_prefix_only_line) {
            impl->ansi_col++;
            impl->ansi_prev_char = ' ';
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        }
        return ansi_emit_newline(impl, sink);
    }
    if (plain_space) {
        if (mdf_emit_all(impl, sink, " ", 1) != 0) return -1;
        impl->ansi_col++;
        impl->ansi_prev_char = ' ';
        impl->ansi_line_has_space = 1;
        return 0;
    }
    return ansi_emit_visible_chunk(impl, sink, " ", 1);
}

int ansi_flush_pending_space(mdf_impl *impl, mdf_sink *sink)
{
    return ansi_flush_pending_space_for(impl, sink, 1);
}

static int ansi_handle_visible_space(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_word(impl, sink) != 0) {
        return -1;
    }
    if (impl->opts.boring && impl->ansi_pending_style_reset) {
        impl->ansi_pending_style_reset = 0;
        impl->ansi_pending_attached_style = NULL;
    }
    if (impl->ansi_pending_style_reset) {
        if (impl->heading_open && impl->heading_style_suspended && !impl->opts.boring) {
            impl->ansi_pending_space = 1;
            impl->ansi_pending_space_no_split = 0;
            impl->ansi_pending_space_plain = 0;
            return 0;
        }
        impl->ansi_pending_space = 1;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 0;
        return 0;
    }
    if (impl->ansi_col == 0 || (impl->ansi_prev_char == ' ' && !impl->ansi_pending_space)) {
        return ansi_write_word_byte(impl, sink, ' ');
    }
    impl->ansi_pending_space = 1;
    impl->ansi_pending_space_no_split = 0;
    impl->ansi_pending_space_plain = 0;
    return 0;
}

static int ansi_flush_pending_final_emph_word(mdf_impl *impl, mdf_sink *sink, char next, int *consumed)
{
    const char *style;
    unsigned char u;
    size_t total_cols;

    *consumed = 0;
    if (impl->ansi_pending_final_emph_style == NULL) {
        return 0;
    }
    style = impl->ansi_pending_final_emph_style;
    if (impl->ansi_pending_final_emph_suffix &&
        next != '\0' && next != ' ' && next != '\n') {
        if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &next, 1) != 0) return -1;
        if (!utf8_continuation_byte(next)) {
            impl->ansi_word_cols++;
        }
        *consumed = 1;
        return 0;
    }
    if (next != '\0' && next != ' ' && next != '\n') {
        u = (unsigned char)next;
        if (!isalnum(u) && !ansi_emphasis_suffix_trailing_only(&next, 1)) {
            impl->ansi_pending_final_emph_suffix = 1;
            impl->ansi_pending_final_emph_base_len = impl->ansi_word_len;
            impl->ansi_pending_final_emph_base_cols = impl->ansi_word_cols;
            if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &next, 1) != 0) return -1;
            if (!utf8_continuation_byte(next)) {
                impl->ansi_word_cols++;
            }
            *consumed = 1;
            return 0;
        }
        if (!isalnum(u)) {
            const char *emit_style;

            total_cols = impl->ansi_word_cols + (utf8_continuation_byte(next) ? 0 : 1);
            if (impl->ansi_pending_space) {
                if (ansi_flush_pending_space_only_for(impl, sink, total_cols) != 0) return -1;
            } else if (impl->opts.width > 0 &&
                       impl->ansi_col > 0 &&
                       impl->ansi_col + (int)total_cols > impl->opts.width) {
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            emit_style = (!impl->opts.boring &&
                          impl->ansi_active_inline_style == style &&
                          impl->ansi_pending_inline_style != style) ? "" : style;
            if (ansi_emit_styled_word_reset_suffix(impl, sink, emit_style,
                    impl->ansi_word, impl->ansi_word_len, &next, 1) != 0) return -1;
            impl->ansi_word_len = 0;
            impl->ansi_word_cols = 0;
            impl->ansi_pending_final_emph_style = NULL;
            impl->ansi_pending_final_emph_suffix = 0;
            impl->ansi_pending_final_emph_word_suffix = 0;
            impl->ansi_pending_final_emph_base_len = 0;
            impl->ansi_pending_final_emph_base_cols = 0;
            impl->ansi_active_inline_style = NULL;
            impl->ansi_pending_inline_style = NULL;
            *consumed = 1;
            return 0;
        }
    }
    if (impl->ansi_pending_final_emph_suffix) {
        const char *emit_style;

        if (impl->ansi_pending_final_emph_base_len > impl->ansi_word_len) {
            impl->ansi_pending_final_emph_base_len = impl->ansi_word_len;
            impl->ansi_pending_final_emph_base_cols = impl->ansi_word_cols;
        }
        total_cols = impl->ansi_word_cols;
        if (impl->ansi_pending_space) {
            if (ansi_flush_pending_space_only_for(impl, sink, total_cols) != 0) return -1;
        } else if (impl->opts.width > 0 &&
                   impl->ansi_col > 0 &&
                   impl->ansi_col + (int)total_cols > impl->opts.width) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
        emit_style = (!impl->opts.boring &&
                      impl->ansi_active_inline_style == style &&
                      impl->ansi_pending_inline_style != style) ? "" : style;
        if (impl->opts.width > 0 && total_cols > (size_t)impl->opts.width) {
            if (ansi_write_styled_split_word_full_width(impl, sink, emit_style,
                    impl->ansi_word, impl->ansi_word_len) != 0) return -1;
            impl->ansi_pending_style_reset = 1;
        } else if (ansi_emit_styled_word_reset_suffix(impl, sink, emit_style,
                impl->ansi_word,
                impl->ansi_pending_final_emph_base_len,
                impl->ansi_word + impl->ansi_pending_final_emph_base_len,
                impl->ansi_word_len - impl->ansi_pending_final_emph_base_len) != 0) return -1;
        impl->ansi_word_len = 0;
        impl->ansi_word_cols = 0;
        impl->ansi_pending_final_emph_style = NULL;
        impl->ansi_pending_final_emph_suffix = 0;
        impl->ansi_pending_final_emph_word_suffix = 0;
        impl->ansi_pending_final_emph_base_len = 0;
        impl->ansi_pending_final_emph_base_cols = 0;
        impl->ansi_active_inline_style = NULL;
        impl->ansi_pending_inline_style = NULL;
        return 0;
    }
    impl->ansi_pending_final_emph_style = NULL;
    impl->ansi_pending_final_emph_suffix = 0;
    impl->ansi_pending_final_emph_word_suffix = 0;
    impl->ansi_pending_final_emph_base_len = 0;
    impl->ansi_pending_final_emph_base_cols = 0;
    if (!impl->opts.boring &&
        impl->ansi_active_inline_style != style &&
        impl->ansi_pending_inline_style != style) {
        impl->ansi_pending_attached_style = style;
    }
    if (ansi_flush_word(impl, sink) != 0) return -1;
    impl->ansi_active_inline_style = NULL;
    impl->ansi_pending_inline_style = NULL;
    impl->ansi_pending_style_reset = 1;
    return 0;
}

int ansi_write_visible_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    int consumed_final;

    if (ansi_flush_pending_final_emph_word(impl, sink, c, &consumed_final) != 0) return -1;
    if (consumed_final) {
        return 0;
    }
    if (impl->ansi_pending_style_reset_after_word &&
        c != ' ' &&
        c != '\n' &&
        !isalnum((unsigned char)c)) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
    }
    if (impl->inline_emph_pending) {
        if (!impl->inline_emph_after_word && c != ' ' && c != '\n') {
            if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &c, 1) != 0) return -1;
            if (!utf8_continuation_byte(c)) {
                impl->ansi_word_cols++;
            }
            impl->ansi_prev_char = c;
            return 0;
        }
        if (ansi_inline_emit_pending_emphasis(impl, sink, impl->inline_emph_after_word ? 0 : impl->ansi_word_cols) != 0) return -1;
        if (impl->inline_mode == 0 && c != ' ' && c != '\n') {
            impl->ansi_active_inline_style = NULL;
        }
    }
    if (impl->in_pre) {
        if (c == '\n') {
            return ansi_emit_newline(impl, sink);
        }
        return ansi_write_word_byte(impl, sink, c);
    }
    if (c == '\n') {
        return ansi_write_newline(impl, sink);
    }
    if (c == ' ') {
        return ansi_handle_visible_space(impl, sink);
    }
    if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &c, 1) != 0) return -1;
    if (!utf8_continuation_byte(c)) {
        impl->ansi_word_cols++;
    }
    impl->ansi_prev_char = c;
    return 0;
}

int ansi_write_standalone_visible_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (ansi_flush_word(impl, sink) != 0) return -1;
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (mdf_emit_all(impl, sink, &c, 1) != 0) return -1;
    if (!utf8_continuation_byte(c)) {
        impl->ansi_col++;
    }
    impl->ansi_prev_char = c;
    return 0;
}

static int ansi_write_visible_nbsp(mdf_impl *impl, mdf_sink *sink)
{
    char c;

    (void)sink;
    c = ' ';
    if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &c, 1) != 0) return -1;
    impl->ansi_word_cols++;
    impl->ansi_prev_char = c;
    return 0;
}

static int ascii_is_alnum_char(char c)
{
    return isalnum((unsigned char)c) != 0;
}

int ascii_is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

int ansi_write_visible(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len);
int ansi_write_visible_cstr(mdf_impl *impl, mdf_sink *sink, const char *src);

static int ansi_emit_pending_html_nbsp(mdf_impl *impl, mdf_sink *sink, char next_char)
{
    if (!impl->inline_html_nbsp_pending) {
        return 0;
    }
    if (impl->inline_html_nbsp_prev_digit && ascii_is_digit_char(next_char)) {
        if (ansi_write_visible_cstr(impl, sink, "&") != 0) return -1;
        if (impl->inline_entity_len > 0 && ansi_write_visible(impl, sink, impl->inline_entity, impl->inline_entity_len) != 0) return -1;
        if (ansi_write_visible_cstr(impl, sink, ";") != 0) return -1;
    } else {
        if (ansi_write_visible_char(impl, sink, ' ') != 0) return -1;
    }
    impl->inline_html_nbsp_pending = 0;
    impl->inline_html_nbsp_prev_digit = 0;
    impl->inline_entity_len = 0;
    return 0;
}

int ansi_write_visible(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (ansi_write_visible_char(impl, sink, src[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int ansi_write_visible_cstr(mdf_impl *impl, mdf_sink *sink, const char *src)
{
    return ansi_write_visible(impl, sink, src, strlen(src));
}

static int ansi_write_backticks(mdf_impl *impl, mdf_sink *sink, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (ansi_write_visible_cstr(impl, sink, "`") != 0) {
            return -1;
        }
    }
    return 0;
}

static int ansi_pre_prefix_width(mdf_impl *impl)
{
    int width;
    int quote_depth;

    width = 0;
    quote_depth = impl->quote_depth > 0 ? impl->quote_depth : (impl->quote_wrap_active ? 1 : 0);
    if (quote_depth > 0) {
        width += impl->quote_prefix_indent;
        width += quote_depth * 2;
    } else if (impl->list_item_open && impl->ansi_wrap_indent > 0) {
        width += impl->ansi_wrap_indent;
    }
    return width;
}

static int ansi_current_prefix_width(mdf_impl *impl)
{
    int width;

    width = ansi_pre_prefix_width(impl);
    if (!impl->in_pre && impl->heading_open) {
        if (!impl->quote_wrap_active) {
            width += impl->heading_level + 1;
        }
    }
    return width;
}

static void ansi_pre_code_reset_line_state(mdf_impl *impl)
{
    impl->pre_code_prefix_state = 0;
    impl->pre_code_leading_cols = 0;
    impl->pre_code_marker_len = 0;
    impl->pre_code_wrap_indent = 0;
}

static int ansi_pre_code_prefix_all_hashes(mdf_impl *impl)
{
    size_t i;

    if (impl->pre_code_marker_len == 0) {
        return 0;
    }
    for (i = 0; i < impl->pre_code_marker_len; i++) {
        if (impl->pre_code_marker[i] != '#') {
            return 0;
        }
    }
    return 1;
}

static int ansi_pre_code_prefix_is_ordered(mdf_impl *impl)
{
    size_t i;

    if (impl->pre_code_marker_len < 2) {
        return 0;
    }
    for (i = 0; i + 1 < impl->pre_code_marker_len; i++) {
        if (impl->pre_code_marker[i] < '0' || impl->pre_code_marker[i] > '9') {
            return 0;
        }
    }
    return impl->pre_code_marker[impl->pre_code_marker_len - 1] == '.' ||
           impl->pre_code_marker[impl->pre_code_marker_len - 1] == ')';
}

static int ansi_pre_code_wrap_prefix_len(mdf_impl *impl)
{
    int prefix_width;

    prefix_width = ansi_pre_prefix_width(impl);
    if (impl->pre_code_marker_len == 1) {
        if (impl->pre_code_marker[0] == '>' ||
            impl->pre_code_marker[0] == '-' ||
            impl->pre_code_marker[0] == '*' ||
            impl->pre_code_marker[0] == '+') {
            return prefix_width + impl->pre_code_leading_cols + 2;
        }
    }
    if (ansi_pre_code_prefix_all_hashes(impl) || ansi_pre_code_prefix_is_ordered(impl)) {
        return prefix_width + impl->pre_code_leading_cols + (int)impl->pre_code_marker_len + 1;
    }
    return 0;
}

static int ansi_pre_code_space_starts_wrap_prefix(mdf_impl *impl)
{
    return impl->pre_code_prefix_state == 1 && ansi_pre_code_wrap_prefix_len(impl) > 0;
}

static int ansi_pre_code_line_has_only_whitespace_prefix(mdf_impl *impl, int prefix_width)
{
    return impl->pre_code_prefix_state == 0 && impl->ansi_col > prefix_width;
}

static const char ansi_pre_code_reopen_prefix_marker[] = "";

static void ansi_pre_code_note_segment(mdf_impl *impl, const char *segment, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c;

        c = segment[i];
        if (impl->pre_code_prefix_state == 2) {
            continue;
        }
        if (impl->pre_code_prefix_state == 0) {
            if (c == ' ' || c == '\t') {
                impl->pre_code_leading_cols++;
                continue;
            }
            impl->pre_code_prefix_state = 1;
            impl->pre_code_marker_len = 0;
        }
        if (impl->pre_code_prefix_state == 1) {
            if (c == ' ' || c == '\t') {
                impl->pre_code_wrap_indent = ansi_pre_code_wrap_prefix_len(impl);
                impl->pre_code_prefix_state = 2;
                continue;
            }
            if (impl->pre_code_marker_len + 1 < sizeof(impl->pre_code_marker)) {
                impl->pre_code_marker[impl->pre_code_marker_len++] = c;
            }
        }
    }
}

static int ansi_pre_code_prepare_segment(mdf_impl *impl, mdf_sink *sink, const char *segment, size_t len, const char **prefix)
{
    (void)sink;
    *prefix = NULL;
    if (len > 0 && segment[0] == ' ' &&
        (ansi_pre_code_space_starts_wrap_prefix(impl) ||
         impl->ansi_prev_char == '-' ||
         impl->ansi_prev_char == '#')) {
        if (impl->code_style_open && !impl->opts.boring) {
            *prefix = ansi_pre_code_reopen_prefix_marker;
            return 0;
        }
    }
    if (!impl->opts.boring && !impl->code_style_open) {
        *prefix = mdf_theme_code_block(impl);
        impl->code_style_open = 1;
    }
    return 0;
}

static int ansi_emit_pre_wrap_newline(mdf_impl *impl, mdf_sink *sink)
{
    ansi_pre_wrap_state state;

    if (impl->code_style_open && !impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        impl->code_style_open = 0;
    }
    ansi_pre_wrap_state_capture(&state, impl);
    ansi_pre_wrap_state_suspend(impl);
    if (ansi_emit_newline(impl, sink) != 0) {
        ansi_pre_wrap_state_restore(impl, &state);
        return -1;
    }
    ansi_pre_wrap_state_restore(impl, &state);
    if (impl->opts.margin_left > 0) {
        ansi_reset_line_output_state(impl);
    }
    if (impl->pre_code_wrap_indent > 0) {
        if (ansi_write_spaces(impl, sink, (size_t)impl->pre_code_wrap_indent) != 0) return -1;
        impl->ansi_prev_char = ' ';
        impl->pre_code_prefix_state = 2;
        impl->pre_code_leading_cols = 0;
        impl->pre_code_marker_len = 0;
    }
    impl->code_style_open = 0;
    return 0;
}

static int ansi_emit_inline_code_wrap_newline(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    }
    if (ansi_emit_newline(impl, sink) != 0) {
        return -1;
    }
    return 0;
}

static const char *ansi_inline_code_style_prefix(mdf_impl *impl, int *style_open)
{
    if (!*style_open && !impl->opts.boring) {
        *style_open = 1;
        return mdf_theme_code_inline(impl);
    }
    return NULL;
}

static int ansi_inline_code_delim(unsigned long cp)
{
    return cp == '(' || cp == ')' || cp == '{' || cp == '}' ||
           cp == '[' || cp == ']' || cp == '<' || cp == '>' ||
           cp == '.' || cp == ',' || cp == ';' || cp == ':' ||
           cp == '/' || cp == '\\';
}

static int ansi_active_inline_style_is_code(mdf_impl *impl)
{
    return impl->ansi_active_inline_style != NULL &&
           strcmp(impl->ansi_active_inline_style, mdf_theme_code_inline(impl)) == 0;
}

static int ansi_line_has_only_list_prefix(mdf_impl *impl)
{
    return impl->list_item_open &&
           impl->ansi_wrap_indent > 0 &&
           impl->ansi_col + (impl->ansi_pending_space ? 1 : 0) == impl->ansi_wrap_indent;
}

static int ansi_write_inline_code_wrapped(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len)
{
    size_t seg_start;
    size_t i;
    int limit;
    int prefix_width;
    int allow_prefix_overflow;
    int preserve_prefix_line;
    int style_open;

    limit = impl->opts.width > 0 ? impl->opts.width : 0;
    prefix_width = ansi_pre_prefix_width(impl);
    allow_prefix_overflow = limit > 0 && impl->ansi_col > 0 && impl->ansi_col == prefix_width;
    preserve_prefix_line = allow_prefix_overflow && prefix_width > 2 &&
                           len > 0 && (text[0] == '(' || text[0] == '[' || text[0] == '{');
    style_open = 0;
    if (limit <= 0 || impl->ansi_col == 0 || preserve_prefix_line ||
        impl->ansi_col + (int)visible_cols(text, len) <= limit) {
        if (ansi_write_code_segment_with_prefix(impl,
                                                sink,
                                                ansi_inline_code_style_prefix(impl, &style_open),
                                                text,
                                                len,
                                                limit) != 0) return -1;
        if (style_open && !impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
        }
        return 0;
    }
    seg_start = 0;
    for (i = 0; i < len; ) {
        unsigned long cp;
        size_t adv;
        size_t end;
        size_t seg_cols;

        adv = utf8_decode_codepoint(text + i, len - i, &cp);
        if (adv == 0) break;
        if (!ansi_inline_code_delim(cp)) {
            i += adv;
            continue;
        }
        end = i + adv;
        seg_cols = visible_cols(text + seg_start, end - seg_start);
        if (limit > 0 && impl->ansi_col > 0 && impl->ansi_col + (int)seg_cols > limit && !allow_prefix_overflow && !preserve_prefix_line) {
            if (style_open) {
                if (ansi_emit_inline_code_wrap_newline(impl, sink) != 0) return -1;
                style_open = 0;
            } else if (ansi_emit_newline(impl, sink) != 0) {
                return -1;
            }
        }
        if (ansi_write_inline_code_segment_with_prefix(impl,
                                                       sink,
                                                       ansi_inline_code_style_prefix(impl, &style_open),
                                                       text + seg_start,
                                                       end - seg_start,
                                                       limit) != 0) return -1;
        allow_prefix_overflow = 0;
        seg_start = end;
        i = end;
    }
    if (seg_start < len) {
        size_t seg_cols;

        seg_cols = visible_cols(text + seg_start, len - seg_start);
        if (limit > 0 && impl->ansi_col > 0 && impl->ansi_col + (int)seg_cols > limit && !allow_prefix_overflow && !preserve_prefix_line) {
            if (style_open) {
                if (ansi_emit_inline_code_wrap_newline(impl, sink) != 0) return -1;
                style_open = 0;
            } else if (ansi_emit_newline(impl, sink) != 0) {
                return -1;
            }
        }
        if (ansi_write_inline_code_segment_with_prefix(impl,
                                                       sink,
                                                       ansi_inline_code_style_prefix(impl, &style_open),
                                                       text + seg_start,
                                                       len - seg_start,
                                                       limit) != 0) return -1;
    }
    if (style_open && !impl->opts.boring) {
        impl->ansi_pending_style_reset = 1;
    }
    return 0;
}

static int ansi_write_plain_code_wrapped(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len)
{
    size_t seg_start;
    size_t i;
    int limit;

    limit = impl->opts.width > 0 ? impl->opts.width : 0;
    if (limit > 0 && impl->ansi_col > 0 && impl->ansi_col + (int)visible_cols(text, len) > limit) {
        for (i = 0; i < len; ) {
            unsigned long cp;
            size_t adv;
            size_t cols;

            adv = utf8_decode_codepoint(text + i, len - i, &cp);
            if (adv == 0) break;
            cols = utf8_display_width(cp);
            if (cp == ' ' && i + adv < len && ascii_is_alnum_char(text[i + adv])) {
                size_t j;
                size_t tail_cols;

                j = i + adv;
                while (j < len && ascii_is_alnum_char(text[j])) {
                    j++;
                }
                tail_cols = visible_cols(text + i + adv, j - (i + adv));
                if (impl->ansi_col > 0 && impl->ansi_col + 1 + (int)tail_cols > limit) {
                    if (ansi_emit_newline(impl, sink) != 0) return -1;
                }
            }
            if (impl->ansi_col > limit ||
                (impl->ansi_col + (int)cols >= limit && isalnum((unsigned char)text[i]))) {
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            if (ansi_emit_visible_chunk(impl, sink, text + i, adv) != 0) return -1;
            i += adv;
        }
        return 0;
    }
    seg_start = 0;
    for (i = 0; i < len; ) {
        unsigned long cp;
        size_t adv;
        size_t end;
        size_t seg_cols;

        adv = utf8_decode_codepoint(text + i, len - i, &cp);
        if (adv == 0) break;
        if (!ansi_inline_code_delim(cp)) {
            i += adv;
            continue;
        }
        end = i + adv;
        seg_cols = visible_cols(text + seg_start, end - seg_start);
        if (limit > 0 && impl->ansi_col > 0 && impl->ansi_col + (int)seg_cols > limit) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
        if (ansi_write_code_segment(impl, sink, text + seg_start, end - seg_start, limit) != 0) return -1;
        seg_start = end;
        i = end;
    }
    if (seg_start < len) {
        size_t seg_cols;

        seg_cols = visible_cols(text + seg_start, len - seg_start);
        if (limit > 0 && impl->ansi_col > 0 && impl->ansi_col + (int)seg_cols > limit) {
            if (ansi_emit_newline(impl, sink) != 0) return -1;
        }
        if (ansi_write_code_segment(impl, sink, text + seg_start, len - seg_start, limit) != 0) return -1;
    }
    return 0;
}

static int ansi_write_code_rune_utf8(mdf_impl *impl, mdf_sink *sink, unsigned long cp)
{
    char buf[4];
    size_t n;

    if (cp < 0x80UL) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800UL) {
        buf[0] = (char)(0xc0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000UL) {
        buf[0] = (char)(0xe0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        buf[0] = (char)(0xf0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (mdf_emit_all(impl, sink, buf, n) != 0) return -1;
    impl->ansi_col += (int)utf8_display_width(cp);
    impl->ansi_prev_char = buf[n - 1];
    return 0;
}

static int ansi_write_code_segment(mdf_impl *impl, mdf_sink *sink, const char *segment, size_t len, int limit)
{
    return ansi_write_code_segment_with_prefix(impl, sink, NULL, segment, len, limit);
}

static int ansi_code_prefix_pending(const char *prefix)
{
    return prefix != NULL && (prefix[0] != '\0' || prefix == ansi_pre_code_reopen_prefix_marker);
}

static int ansi_emit_buffer_append_code_prefix(mdf_impl *impl, const char *prefix)
{
    if (prefix == NULL) {
        return 0;
    }
    if (impl->quote_text_open) {
        if (mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) {
            return -1;
        }
        impl->quote_text_open = 0;
    }
    if (prefix == ansi_pre_code_reopen_prefix_marker) {
        return mdf_emit_buffer_append(impl, "\033[0m", 4) == 0 &&
               mdf_emit_buffer_append_cstr(impl, mdf_theme_code_block(impl)) == 0 ? 0 : -1;
    }
    return mdf_emit_buffer_append_cstr(impl, prefix);
}

static int ansi_emit_code_prefix(mdf_impl *impl, mdf_sink *sink, const char *prefix)
{
    if (!ansi_code_prefix_pending(prefix)) {
        return 0;
    }
    if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
    if (mdf_emit_buffer_reset(impl) != 0 ||
        ansi_emit_buffer_append_code_prefix(impl, prefix) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    return mdf_emit_buffer_commit(impl, sink);
}

static int ansi_code_effective_segment_limit(mdf_impl *impl, int limit)
{
    int effective_limit;

    effective_limit = limit;
    if (limit > 0 && (impl->opts.margin_left > 0 || impl->opts.margin_right > 0)) {
        if (impl->ansi_col > 0 && impl->ansi_col < effective_limit) {
            effective_limit -= impl->ansi_col;
        } else if (impl->ansi_col == 0 && impl->opts.margin_left > 0) {
            effective_limit -= impl->opts.margin_left;
        }
        if (effective_limit < 1) {
            effective_limit = 1;
        }
    }
    return effective_limit;
}

static int ansi_preformatted_segment_limit(mdf_impl *impl, int limit, int overlong_line)
{
    int reserved;

    if (!overlong_line || limit <= 0 ||
        (impl->opts.margin_left <= 0 && impl->opts.margin_right <= 0)) {
        return limit;
    }
    reserved = impl->opts.margin_left;
    if (limit > reserved) {
        return limit - reserved;
    }
    return 1;
}

static int ansi_write_inline_code_segment_with_prefix(mdf_impl *impl, mdf_sink *sink, const char *prefix, const char *segment, size_t len, int limit)
{
    size_t width;
    size_t off;
    int emitted;
    int prefix_pending;
    int effective_limit;

    width = visible_cols(segment, len);
    effective_limit = ansi_code_effective_segment_limit(impl, limit);
    prefix_pending = ansi_code_prefix_pending(prefix);
    if (effective_limit == 1 && width > (size_t)effective_limit) {
        if (ansi_emit_code_prefix(impl, sink, prefix) != 0) return -1;
        return ansi_write_code_rune_utf8(impl, sink, 0x2026UL);
    }
    off = 0;
    emitted = 0;
    while (off < len) {
        unsigned long cp;
        size_t adv;

        if (effective_limit > 0 && width > (size_t)effective_limit && emitted >= effective_limit - 1) {
            return ansi_write_code_rune_utf8(impl, sink, 0x2026UL);
        }
        adv = utf8_decode_codepoint(segment + off, len - off, &cp);
        if (adv == 0) break;
        if (prefix_pending) {
            if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
            if (mdf_emit_buffer_reset(impl) != 0 ||
                ansi_emit_buffer_append_code_prefix(impl, prefix) != 0 ||
                mdf_emit_buffer_append(impl, segment + off, adv) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            ansi_update_visible_output_state(impl, segment + off, adv);
            prefix_pending = 0;
        } else if (ansi_write_code_rune_utf8(impl, sink, cp) != 0) {
            return -1;
        }
        emitted += (int)utf8_display_width(cp);
        off += adv;
    }
    return 0;
}

static int ansi_write_code_segment_with_prefix(mdf_impl *impl, mdf_sink *sink, const char *prefix, const char *segment, size_t len, int limit)
{
    size_t width;
    size_t off;
    int emitted;
    int effective_limit;

    width = visible_cols(segment, len);
    effective_limit = ansi_code_effective_segment_limit(impl, limit);
    if (effective_limit <= 0 || width <= (size_t)effective_limit) {
        int has_prefix;

        has_prefix = ansi_code_prefix_pending(prefix);
        if (has_prefix || len > 0) {
            if (len > 0 && ansi_ensure_left_margin(impl, sink) != 0) return -1;
            if (mdf_emit_buffer_reset(impl) != 0 ||
                ansi_emit_buffer_append_code_prefix(impl, prefix) != 0 ||
                mdf_emit_buffer_append(impl, segment, len) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            ansi_update_visible_output_state(impl, segment, len);
        }
        return 0;
    }
    if (ansi_code_prefix_pending(prefix)) {
        if (ansi_emit_code_prefix(impl, sink, prefix) != 0) return -1;
    }
    if (effective_limit == 1) {
        return ansi_write_code_rune_utf8(impl, sink, 0x2026UL);
    }
    off = 0;
    emitted = 0;
    while (off < len && emitted < effective_limit - 1) {
        unsigned long cp;
        size_t adv;

        adv = utf8_decode_codepoint(segment + off, len - off, &cp);
        if (adv == 0) break;
        if (ansi_write_code_rune_utf8(impl, sink, cp) != 0) return -1;
        emitted += (int)utf8_display_width(cp);
        off += adv;
    }
    return ansi_write_code_rune_utf8(impl, sink, 0x2026UL);
}

static int ansi_write_preformatted_code(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len)
{
    size_t seg_start;
    size_t i;
    int limit;
    int prefix_width;
    int overlong_line;

    prefix_width = ansi_pre_prefix_width(impl);
    limit = impl->opts.width > 0 ? impl->opts.width : 0;
    overlong_line = len > 0 && limit > 0 && visible_cols(text, len) > (size_t)limit;
    if (len > 0 && (limit <= 0 || impl->ansi_col + (int)visible_cols(text, len) <= limit)) {
        const char *prefix;
        size_t marker;

        if (ansi_pre_code_prepare_segment(impl, sink, text, len, &prefix) != 0) return -1;
        marker = 0;
        while (marker < len && text[marker] == ' ') {
            marker++;
        }
        if (!impl->opts.boring &&
            marker + 1 < len &&
            (text[marker] == '-' || text[marker] == '#') &&
            text[marker + 1] == ' ') {
            size_t prefix_len;

            prefix_len = prefix == NULL ? 0 : strlen(prefix);
            if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, prefix, prefix_len) != 0 ||
                mdf_emit_buffer_append(impl, text, marker + 1) != 0 ||
                mdf_emit_buffer_append(impl, "\033[0m", 4) != 0 ||
                mdf_emit_buffer_append_cstr(impl, mdf_theme_code_block(impl)) != 0 ||
                mdf_emit_buffer_append(impl, text + marker + 1, len - marker - 1) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            ansi_update_visible_output_state(impl, text, len);
            ansi_pre_code_note_segment(impl, text, len);
            return 0;
        }
        if (ansi_write_code_segment_with_prefix(impl, sink, prefix, text, len, limit) != 0) return -1;
        ansi_pre_code_note_segment(impl, text, len);
        return 0;
    }
    seg_start = 0;
    for (i = 0; i < len; ) {
        unsigned long cp;
        size_t adv;
        int is_delim;
        int segment_limit;
        int overflow;
        int keep_prefix_segment;
        int whitespace_prefix_line;
        const char *prefix;
        size_t end;

        adv = utf8_decode_codepoint(text + i, len - i, &cp);
        if (adv == 0) break;
        is_delim = cp == '(' || cp == ')' || cp == '{' || cp == '}' ||
                   cp == '[' || cp == ']' || cp == '<' || cp == '>' ||
                   cp == '.' || cp == ',' || cp == ';' || cp == ':' ||
                   cp == '-' || cp == '#' ||
                   cp == '/' || cp == '\\';
        if (!is_delim) {
            i += adv;
            continue;
        }
        end = i + adv;
        segment_limit = ansi_preformatted_segment_limit(impl, limit, overlong_line);
        overflow = impl->opts.width > 0 && impl->ansi_col > prefix_width &&
                   impl->ansi_col + (int)visible_cols(text + seg_start, end - seg_start) > impl->opts.width;
        keep_prefix_segment = text[seg_start] == ' ' && ansi_pre_code_space_starts_wrap_prefix(impl);
        whitespace_prefix_line = ansi_pre_code_line_has_only_whitespace_prefix(impl, prefix_width);
        if (overflow && !keep_prefix_segment && !whitespace_prefix_line) {
            if (ansi_emit_pre_wrap_newline(impl, sink) != 0) return -1;
        } else if (overflow && keep_prefix_segment && !whitespace_prefix_line) {
            segment_limit = impl->opts.width - (impl->ansi_col - prefix_width);
            if (segment_limit < 1) {
                segment_limit = 1;
            }
        }
        if (ansi_pre_code_prepare_segment(impl, sink, text + seg_start, end - seg_start, &prefix) != 0) return -1;
        if (overlong_line) {
            if (ansi_write_inline_code_segment_with_prefix(impl, sink, prefix, text + seg_start, end - seg_start, segment_limit) != 0) return -1;
        } else if (ansi_write_code_segment_with_prefix(impl, sink, prefix, text + seg_start, end - seg_start, segment_limit) != 0) return -1;
        ansi_pre_code_note_segment(impl, text + seg_start, end - seg_start);
        seg_start = end;
        i = end;
    }
    if (seg_start < len) {
        int segment_limit;
        int overflow;
        int keep_prefix_segment;
        int whitespace_prefix_line;
        const char *prefix;

        segment_limit = ansi_preformatted_segment_limit(impl, limit, overlong_line);
        overflow = impl->opts.width > 0 && impl->ansi_col > prefix_width &&
                   impl->ansi_col + (int)visible_cols(text + seg_start, len - seg_start) > impl->opts.width;
        keep_prefix_segment = text[seg_start] == ' ' && ansi_pre_code_space_starts_wrap_prefix(impl);
        whitespace_prefix_line = ansi_pre_code_line_has_only_whitespace_prefix(impl, prefix_width);
        if (overflow && !keep_prefix_segment && !whitespace_prefix_line) {
            if (ansi_emit_pre_wrap_newline(impl, sink) != 0) return -1;
        } else if (overflow && keep_prefix_segment && !whitespace_prefix_line) {
            segment_limit = impl->opts.width - (impl->ansi_col - prefix_width);
            if (segment_limit < 1) {
                segment_limit = 1;
            }
        }
        if (ansi_pre_code_prepare_segment(impl, sink, text + seg_start, len - seg_start, &prefix) != 0) return -1;
        if (overlong_line) {
            if (ansi_write_inline_code_segment_with_prefix(impl, sink, prefix, text + seg_start, len - seg_start, segment_limit) != 0) return -1;
        } else if (ansi_write_code_segment_with_prefix(impl, sink, prefix, text + seg_start, len - seg_start, segment_limit) != 0) return -1;
        ansi_pre_code_note_segment(impl, text + seg_start, len - seg_start);
    }
    return 0;
}

int ansi_flush_pre_code_buffer(mdf_impl *impl, mdf_sink *sink)
{
    int i;

    if (impl->pre_code_len == 0) {
        return 0;
    }
    if (ansi_flush_pending_space(impl, sink) != 0) return -1;
    if (impl->list_item_open && impl->ansi_col == 0 && impl->ansi_wrap_indent > 0) {
        for (i = 0; i < impl->ansi_wrap_indent; i++) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            impl->ansi_col++;
        }
    }
    if (ansi_write_preformatted_code(impl, sink, impl->pre_code_buf, impl->pre_code_len) != 0) {
        return -1;
    }
    impl->pre_code_len = 0;
    return 0;
}

int ansi_inline_flush_literal(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_pending_exact_fallback(impl, sink, '\0') != 0) return -1;
    if (ansi_emit_pending_html_nbsp(impl, sink, '\0') != 0) return -1;
    if (impl->inline_emph_pending) {
        if (ansi_inline_emit_pending_emphasis(impl, sink, impl->inline_emph_after_word ? 0 : impl->ansi_word_cols) != 0) return -1;
    }
    if (impl->inline_outer_paren_candidate) {
        if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
        impl->inline_outer_paren_candidate = 0;
    }
    if (impl->inline_mode == 0) {
        return 0;
    }
        if (impl->inline_mode == 4) {
            if (ansi_write_backticks(impl, sink, impl->inline_code_delim_len) != 0) return -1;
            if (impl->inline_code_len > 0 && ansi_write_visible(impl, sink, impl->inline_code, impl->inline_code_len) != 0) return -1;
            impl->inline_mode = 0;
            impl->inline_code_len = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (impl->inline_mode == 8) {
        if (ansi_write_backticks(impl, sink, impl->inline_code_delim_len) != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (impl->inline_mode == 9) {
        if (ansi_write_backticks(impl, sink, impl->inline_code_delim_len) != 0) return -1;
        if (impl->inline_code_len > 0 && ansi_write_visible(impl, sink, impl->inline_code, impl->inline_code_len) != 0) return -1;
        if (ansi_write_backticks(impl, sink, impl->inline_code_pending_ticks) != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_code_len = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (impl->inline_mode == 5) {
        int i;

        if (impl->inline_emph_streaming) {
            if (ansi_inline_flush_unmatched_nested_emphasis(impl, sink) != 0) return -1;
            if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 1) != 0) return -1;
            if (!impl->opts.boring) {
                impl->ansi_pending_style_reset = 1;
            }
            ansi_inline_clear_emphasis_state(impl);
            return 0;
        }
        for (i = 0; i < impl->inline_emph_count; i++) {
            if (ansi_write_visible_char(impl, sink, impl->inline_emph_delim) != 0) return -1;
        }
        if (impl->inline_emph_len > 0 && ansi_write_visible(impl, sink, impl->inline_emph, impl->inline_emph_len) != 0) return -1;
        for (i = 0; i < impl->inline_emph_close_count; i++) {
            if (ansi_write_visible_char(impl, sink, impl->inline_emph_delim) != 0) return -1;
        }
        impl->inline_mode = 0;
        impl->inline_emph_len = 0;
        impl->inline_emph_count = 0;
        impl->inline_emph_close_count = 0;
        impl->inline_emph_pending = 0;
        impl->inline_emph_streaming = 0;
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_count = 0;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        return 0;
    }
    if (impl->inline_mode == 10 || impl->inline_mode == 11 || impl->inline_mode == 12) {
        if (ansi_write_visible_char(impl, sink, '[') != 0) return -1;
        if (impl->inline_text_len > 0 && ansi_write_visible(impl, sink, impl->inline_text, impl->inline_text_len) != 0) return -1;
        if (impl->inline_mode >= 11 && ansi_write_visible_char(impl, sink, ']') != 0) return -1;
        if (impl->inline_mode == 12) {
            if (ansi_write_visible_char(impl, sink, '(') != 0) return -1;
            if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
        }
        impl->inline_mode = 5;
        impl->inline_text_len = 0;
        impl->inline_url_len = 0;
        return 0;
    }
    if (impl->inline_mode == 13) {
        if (ansi_write_visible_char(impl, sink, '<') != 0) return -1;
        if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
        impl->inline_mode = 5;
        impl->inline_url_len = 0;
        return 0;
    }
    if (impl->inline_mode == 6) {
        if (ansi_write_visible_cstr(impl, sink, "&") != 0) return -1;
        if (impl->inline_entity_len > 0 && ansi_write_visible(impl, sink, impl->inline_entity, impl->inline_entity_len) != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_entity_len = 0;
        return 0;
    }
    if (impl->inline_mode == 7) {
        if (ansi_write_visible_cstr(impl, sink, "<") != 0) return -1;
        if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_url_len = 0;
        return 0;
    }
    if (impl->inline_outer_paren_pending) {
        if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
        impl->inline_outer_paren_pending = 0;
    }
    if (ansi_write_visible_cstr(impl, sink, "[") != 0) return -1;
    if (impl->inline_text_len > 0 && ansi_write_visible(impl, sink, impl->inline_text, impl->inline_text_len) != 0) return -1;
    if (impl->inline_mode >= 2) {
        if (ansi_write_visible_cstr(impl, sink, "]") != 0) return -1;
    }
    if (impl->inline_mode >= 3) {
        if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
        if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
    }
    impl->inline_mode = 0;
    impl->inline_text_len = 0;
    impl->inline_url_len = 0;
    impl->inline_code_len = 0;
    impl->inline_code_delim_len = 0;
    impl->inline_code_pending_ticks = 0;
    impl->inline_emph_len = 0;
    impl->inline_emph_count = 0;
    impl->inline_emph_close_count = 0;
    impl->inline_emph_pending = 0;
    impl->inline_emph_after_word = 0;
    impl->inline_emph_streaming = 0;
    impl->inline_emph_nested_delim = 0;
    impl->inline_emph_nested_count = 0;
    impl->inline_emph_nested_close_count = 0;
    impl->inline_emph_nested_saw_space = 0;
    impl->inline_entity_len = 0;
    return 0;
}

static size_t visible_first_word_cols(const char *s, size_t len)
{
    size_t i;
    size_t cols;
    size_t adv;
    unsigned long cp;

    cols = 0;
    i = 0;
    while (i < len) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            break;
        }
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        cols += utf8_display_width(cp);
        i += adv;
    }
    return cols;
}

static size_t visible_cols(const char *s, size_t len)
{
    size_t i;
    size_t cols;
    size_t adv;
    unsigned long cp;

    cols = 0;
    i = 0;
    while (i < len) {
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        cols += utf8_display_width(cp);
        i += adv;
    }
    return cols;
}

static int ansi_write_direct_visible(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len)
{
    size_t i;

    i = 0;
    while (i < len) {
        unsigned long cp;
        size_t adv;

        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        if (adv == 1 && cp < 0x80UL) {
            if (ansi_write_word_byte(impl, sink, s[i]) != 0) return -1;
        } else {
            if (ansi_emit_visible_chunk(impl, sink, s + i, adv) != 0) return -1;
        }
        i += adv;
    }
    return 0;
}

static size_t url_visible_fit_start(const char *s, size_t len, int limit)
{
    size_t i;

    if (limit <= 0) {
        return 0;
    }
    for (i = 0; i + 2 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            if ((int)visible_cols(s + i + 3, len - i - 3) <= limit) {
                return i + 3;
            }
            break;
        }
    }
    return 0;
}

static int ansi_write_url_fit(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, int limit)
{
    size_t start;
    size_t i;
    int cols;

    if (limit <= 0) {
        return 0;
    }
    start = url_visible_fit_start(s, len, limit);
    if (start > 0) {
        size_t q;

        for (q = start; q < len; q++) {
            if (s[q] == '?') {
                if (ansi_emit_visible_chunk(impl, sink, s + start, q - start + 1) != 0) return -1;
                if (q + 1 < len) {
                    return ansi_emit_visible_chunk(impl, sink, s + q + 1, len - q - 1);
                }
                return 0;
            }
        }
        return ansi_write_visible(impl, sink, s + start, len - start);
    }
    if ((int)visible_cols(s, len) <= limit) {
        return ansi_write_visible(impl, sink, s, len);
    }
    if (limit == 1) {
        return ansi_write_visible_cstr(impl, sink, "\342\200\246");
    }
    cols = 0;
    for (i = 0; i < len && cols < limit - 1; i++) {
        if (ansi_write_visible_char(impl, sink, s[i]) != 0) return -1;
        if (!utf8_continuation_byte(s[i])) {
            cols++;
        }
    }
    return ansi_write_visible_cstr(impl, sink, "\342\200\246");
}

int ansi_write_styled_words_inner(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, const char *style, int open_style, size_t trailing_reserve)
{
    size_t i;
    size_t start;
    size_t cols;
    size_t reserved_cols;
    int wrote_word;
    const char *pending_style;

    wrote_word = 0;
    pending_style = open_style ? style : "";
    i = 0;
    while (i < len) {
        while (i < len && s[i] == ' ') {
            i++;
        }
        if (i >= len) {
            break;
        }
        start = i;
        while (i < len && s[i] != ' ') {
            i++;
        }
        cols = visible_cols(s + start, i - start);
        reserved_cols = cols;
        if (i >= len) {
            reserved_cols += trailing_reserve;
        }
        if (wrote_word && impl->opts.width > 0 && impl->ansi_col > 0 && impl->ansi_col + 1 + (int)reserved_cols > impl->opts.width) {
            if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            pending_style = style;
        } else if (wrote_word) {
            if (ansi_write_word_byte(impl, sink, ' ') != 0) return -1;
        }
        if (i >= len &&
            trailing_reserve > 0 &&
            impl->ansi_word_len > 0 &&
            ansi_emphasis_suffix_trailing_only(impl->ansi_word, impl->ansi_word_len) &&
            !impl->opts.boring &&
            cols + impl->ansi_word_cols <= (size_t)(impl->opts.width > 0 ? impl->opts.width : (int)(cols + impl->ansi_word_cols))) {
            if (ansi_emit_styled_word_reset_suffix(impl, sink, pending_style,
                    s + start, i - start, impl->ansi_word, impl->ansi_word_len) != 0) return -1;
            impl->ansi_word_len = 0;
            impl->ansi_word_cols = 0;
        } else if (impl->opts.width > 0 && cols > (size_t)impl->opts.width) {
            if (ansi_write_styled_split_word_full_width(impl, sink, pending_style, s + start, i - start) != 0) return -1;
        } else {
            size_t split;

            split = 0;
            if (i - start >= 3) {
                size_t k;

                for (k = start + 1; k + 1 < i; k++) {
                    if (s[k] == ':' &&
                        ascii_is_digit_char(s[k - 1]) &&
                        ascii_is_digit_char(s[k + 1])) {
                        split = k + 1 - start;
                        break;
                    }
                }
            }
            if (split > 0) {
                if (ansi_emit_styled_visible_chunk(impl, sink, pending_style, s + start, split) != 0) {
                    return -1;
                }
                if (ansi_emit_visible_chunk(impl, sink, s + start + split, i - start - split) != 0) {
                    return -1;
                }
            } else if (ansi_emit_styled_visible_chunk(impl, sink, pending_style, s + start, i - start) != 0) {
                return -1;
            }
        }
        pending_style = "";
        wrote_word = 1;
    }
    return 0;
}

static int ansi_write_styled_words(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, const char *style)
{
    return ansi_write_styled_words_inner(impl, sink, s, len, style, 1, 0);
}

static int ansi_write_styled_span_reserved(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, const char *style, size_t trailing_reserve)
{
    size_t start;
    size_t end;

    if (len == 0) {
        return 0;
    }
    start = 0;
    while (start < len && s[start] == ' ') {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (ansi_write_word_byte(impl, sink, ' ') != 0) return -1;
        start++;
    }
    end = len;
    while (end > start && s[end - 1] == ' ') {
        end--;
    }
    if (end > start && ansi_write_styled_words_inner(impl, sink, s + start, end - start, style, 1, trailing_reserve) != 0) return -1;
    while (end < len) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (ansi_write_word_byte(impl, sink, ' ') != 0) return -1;
        end++;
    }
    return 0;
}

static int inline_full_emphasis(mdf_impl *impl, const char *text, size_t text_len, const char **inner, size_t *inner_len, const char **style)
{
    char delim;

    if (text_len < 3) {
        return 0;
    }
    delim = text[0];
    if ((delim != '*' && delim != '_') || text[text_len - 1] != delim) {
        return 0;
    }
    if (text_len >= 4 && text[1] == delim && text[text_len - 2] == delim) {
        *inner = text + 2;
        *inner_len = text_len - 4;
        *style = mdf_theme_strong(impl);
        return *inner_len > 0;
    }
    *inner = text + 1;
    *inner_len = text_len - 2;
    *style = mdf_theme_emphasis(impl);
    return *inner_len > 0;
}

static size_t inline_link_label_first_word_cols(mdf_impl *impl, const char *text, size_t text_len)
{
    const char *inner;
    const char *style;
    size_t inner_len;

    if (inline_full_emphasis(impl, text, text_len, &inner, &inner_len, &style)) {
        (void)style;
        return visible_first_word_cols(inner, inner_len);
    }
    return visible_first_word_cols(text, text_len);
}

static size_t inline_link_label_wrap_cols(mdf_impl *impl, const char *text, size_t text_len)
{
    return inline_link_label_first_word_cols(impl, text, text_len);
}

static const char *ansi_join_styles(const char *prefix, const char *suffix, char *buf, size_t buf_cap)
{
    size_t prefix_len;
    size_t suffix_len;

    if (prefix == NULL || prefix[0] == '\0') {
        return suffix;
    }
    prefix_len = strlen(prefix);
    suffix_len = strlen(suffix);
    if (prefix_len + suffix_len + 1 > buf_cap) {
        return suffix;
    }
    memcpy(buf, prefix, prefix_len);
    memcpy(buf + prefix_len, suffix, suffix_len + 1);
    return buf;
}

static int ansi_write_osc8_start(mdf_impl *impl, mdf_sink *sink, const char *prefix, const char *url, size_t url_len)
{
    size_t prefix_len;

    prefix_len = prefix == NULL ? 0 : strlen(prefix);
    if (mdf_emit_buffer_reset(impl) != 0 ||
        mdf_emit_buffer_append(impl, "\033]8;;", 5) != 0 ||
        mdf_emit_buffer_append(impl, prefix, prefix_len) != 0 ||
        mdf_emit_buffer_append(impl, url, url_len) != 0 ||
        mdf_emit_buffer_append(impl, "\033\\", 2) != 0) {
        mdf_impl_mark_oom(impl);
        return -1;
    }
    return mdf_emit_buffer_commit(impl, sink);
}

static int ansi_emit_link_label(mdf_impl *impl, mdf_sink *sink, const char *text, size_t text_len, const char *prefix_style)
{
    const char *inner;
    const char *style;
    size_t inner_len;
    ansi_sim_input input;
    char style_buf[96];
    char style_buf2[160];

    #define ANSI_EMIT_LINK_LABEL_INPUT(label_text, label_len, label_style) \
        do { \
            input.text = (label_text); \
            input.len = (label_len); \
            input.style = (label_style); \
            input.kind = ANSI_SIM_TEXT; \
            if (impl->inline_outer_paren_pending && input.len > 0) { \
                size_t first_len; \
                size_t style_len; \
                size_t reset_len; \
                first_len = 0; \
                while (first_len < input.len && input.text[first_len] != ' ') { \
                    first_len++; \
                } \
                style_len = strlen(input.style); \
                reset_len = (impl->ansi_pending_style_reset || impl->quote_text_open) && !impl->opts.boring ? 4 : 0; \
                if (mdf_emit_buffer_reset(impl) != 0 || \
                    mdf_emit_buffer_append(impl, "\033[0m", reset_len) != 0 || \
                    mdf_emit_buffer_append(impl, "(", 1) != 0 || \
                    mdf_emit_buffer_append(impl, input.style, style_len) != 0 || \
                    mdf_emit_buffer_append(impl, input.text, first_len) != 0) { \
                    mdf_impl_mark_oom(impl); \
                    return -1; \
                } \
                impl->ansi_pending_style_reset = 0; \
                impl->quote_text_open = 0; \
                if (mdf_emit_buffer_commit(impl, sink) != 0) return -1; \
                ansi_update_visible_output_state(impl, "(", 1); \
                ansi_update_visible_output_state(impl, input.text, first_len); \
                impl->inline_outer_paren_pending = 0; \
                if (first_len < input.len) { \
                    input.text += first_len; \
                    input.len -= first_len; \
                    if (ansi_sim_emit_inputs_active(impl, sink, &input, 1, input.style) != 0) return -1; \
                } \
            } else if (ansi_sim_emit_inputs(impl, sink, &input, 1) != 0) return -1; \
        } while (0)

	    if (inline_full_emphasis(impl, text, text_len, &inner, &inner_len, &style)) {
	        if (style[2] == '3') {
	            ANSI_EMIT_LINK_LABEL_INPUT(inner,
	                                       inner_len,
	                                       impl->opts.boring ? "" :
	                                       ansi_join_styles(ansi_join_styles(prefix_style, mdf_theme_link_text(impl), style_buf, sizeof(style_buf)),
	                                                        mdf_theme_emphasis(impl),
	                                                        style_buf2,
	                                                        sizeof(style_buf2)));
	        } else {
	            ANSI_EMIT_LINK_LABEL_INPUT(inner,
	                                       inner_len,
	                                       impl->opts.boring ? "" :
	                                       ansi_join_styles(ansi_join_styles(prefix_style, mdf_theme_link_text(impl), style_buf, sizeof(style_buf)),
	                                                        mdf_theme_strong(impl),
	                                                        style_buf2,
	                                                        sizeof(style_buf2)));
	        }
        return 0;
    }
    {
        size_t scheme_end;

        for (scheme_end = 0; scheme_end + 2 < text_len; scheme_end++) {
            if (text[scheme_end] == ':' && text[scheme_end + 1] == '/' && text[scheme_end + 2] == '/') {
                break;
            }
        }
        if (scheme_end + 2 < text_len &&
            impl->opts.width > 0 &&
            (int)visible_cols(text + scheme_end + 1, text_len - scheme_end - 1) <= impl->opts.width) {
            const char *link_style;

            link_style = impl->opts.boring ? "" : ansi_join_styles(prefix_style, mdf_theme_link_text(impl), style_buf, sizeof(style_buf));
            if (ansi_emit_styled_visible_chunk(impl, sink, link_style, text, scheme_end + 1) != 0) return -1;
            if (ansi_emit_visible_chunk(impl, sink, text + scheme_end + 1, text_len - scheme_end - 1) != 0) return -1;
            return 0;
        }
    }
    ANSI_EMIT_LINK_LABEL_INPUT(text,
                               text_len,
                               impl->opts.boring ? "" :
                               ansi_join_styles(prefix_style, mdf_theme_link_text(impl), style_buf, sizeof(style_buf)));
    #undef ANSI_EMIT_LINK_LABEL_INPUT
    return 0;
}

static int ansi_emit_link_parts_ex(mdf_impl *impl, mdf_sink *sink,
                                   const char *text, size_t text_len,
                                   const char *url, size_t url_len,
                                   const char *prefix_style,
                                   const char *resume_style)
{
    size_t first_word_cols;
    size_t label_wrap_cols;
    size_t open_prefix_cols;
    size_t wrapped_fallback_cols;
    int outer_paren_context;
    const char *after_link_style;

    after_link_style = resume_style != NULL ? resume_style : prefix_style;
    first_word_cols = inline_link_label_first_word_cols(impl, text, text_len);
    label_wrap_cols = inline_link_label_wrap_cols(impl, text, text_len);
    outer_paren_context = impl->inline_outer_paren_pending;
    open_prefix_cols = outer_paren_context ? 1 : 0;
    wrapped_fallback_cols = visible_cols(url, url_len) + 2;
    if (impl->opts.osc8) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (ansi_flush_pending_space_for(impl, sink, label_wrap_cols + open_prefix_cols) != 0) return -1;
        if (impl->inline_outer_paren_pending) {
            if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
            if (ansi_flush_word(impl, sink) != 0) return -1;
            impl->inline_outer_paren_pending = 0;
        }
        if (impl->opts.width > 0 &&
            impl->ansi_col > ansi_current_prefix_width(impl) &&
            impl->ansi_col + (int)first_word_cols > impl->opts.width &&
            ansi_emit_newline(impl, sink) != 0) return -1;
        if (text_len > 0 && ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (ansi_write_osc8_start(impl, sink, "", url, url_len) != 0) return -1;
        if (((prefix_style != NULL && prefix_style[0] != '\0') ||
             (after_link_style != NULL && after_link_style[0] != '\0')) &&
            !impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
        }
        if (ansi_emit_link_label(impl, sink, text, text_len, prefix_style) != 0) return -1;
        if (mdf_emit_cstr(impl, sink, "\033]8;;\033\\") != 0) return -1;
        if (after_link_style != NULL &&
            after_link_style[0] != '\0' &&
            !impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
            impl->ansi_pending_inline_style = after_link_style;
            impl->ansi_active_inline_style = NULL;
        } else if (!impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
        }
    } else {
        ansi_sim_input inputs[3];
        size_t fallback_cols;
        int wrapped_fallback;
        const char *saved_pending_inline_style;
        const char *saved_active_inline_style;
        const char *label_pending_inline_style;
        const char *label_active_inline_style;
        int label_reset_pending;

        wrapped_fallback = 0;
        label_reset_pending = 0;
        if (ansi_flush_pending_space_for(impl, sink, label_wrap_cols + open_prefix_cols) != 0) return -1;
        label_pending_inline_style = impl->ansi_pending_inline_style;
        label_active_inline_style = impl->ansi_active_inline_style;
        if (((prefix_style != NULL && prefix_style[0] != '\0') ||
             (after_link_style != NULL && after_link_style[0] != '\0')) &&
            !impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
        }
        impl->ansi_pending_inline_style = NULL;
        impl->ansi_active_inline_style = NULL;
        if (ansi_emit_link_label(impl, sink, text, text_len, prefix_style) != 0) return -1;
        if (after_link_style != NULL && after_link_style[0] != '\0') {
            impl->ansi_pending_inline_style = after_link_style;
        } else {
            impl->ansi_pending_inline_style = label_pending_inline_style != NULL ? label_pending_inline_style : label_active_inline_style;
        }
        impl->ansi_active_inline_style = NULL;
        label_reset_pending = !impl->opts.boring;
        fallback_cols = 2 + visible_cols(url, url_len) + 1;
        if (outer_paren_context &&
            impl->opts.width > 0 &&
            impl->ansi_col > 0 &&
            impl->ansi_col + (int)fallback_cols + 1 > impl->opts.width) {
            if (label_reset_pending && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            label_reset_pending = 0;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            wrapped_fallback = 1;
        }
        if (outer_paren_context &&
            wrapped_fallback &&
            impl->opts.width > 0 &&
            wrapped_fallback_cols + 1 <= (size_t)impl->opts.width) {
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, "(", 1) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0) ||
                mdf_emit_buffer_append(impl, url, url_len) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
                mdf_emit_buffer_append(impl, "))", 2) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            ansi_update_visible_output_state(impl, "(", 1);
            ansi_update_visible_output_state(impl, url, url_len);
            ansi_update_visible_output_state(impl, "))", 2);
            impl->pending_wrapped_link_outer_close = 1;
            return 0;
        }
        if (!outer_paren_context &&
            impl->opts.width > 0 &&
            impl->ansi_col > 0 &&
            impl->ansi_col + 1 + (int)wrapped_fallback_cols > impl->opts.width &&
            wrapped_fallback_cols <= (size_t)impl->opts.width) {
            if (label_reset_pending && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            label_reset_pending = 0;
            if (ansi_emit_newline(impl, sink) != 0) return -1;
            if (prefix_style != NULL && prefix_style[0] != '\0') {
                if (ansi_emit_link_fallback_only(impl, sink, url, url_len, 0) != 0) return -1;
                if (impl->ansi_pending_fallback_emit_valid &&
                    ansi_flush_pending_fallback_emit(impl, sink) != 0) return -1;
                if (impl->pending_fallback_exact &&
                    ansi_flush_pending_exact_fallback(impl, sink, '\0') != 0) return -1;
                return 0;
            }
            return ansi_set_pending_exact_fallback(impl, url, url_len);
        }
        if (!outer_paren_context && impl->opts.width > 0 && url_len > 0 && url[0] == '#') {
            if (impl->ansi_col > 0 &&
                impl->ansi_col + 1 + (int)wrapped_fallback_cols > impl->opts.width) {
                if (label_reset_pending && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
                label_reset_pending = 0;
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            } else if (label_reset_pending) {
                if (mdf_emit_cstr(impl, sink, "\033[0m ") != 0) return -1;
                impl->ansi_col++;
                impl->ansi_prev_char = ' ';
                impl->ansi_line_has_space = 1;
                label_reset_pending = 0;
            } else {
                if (mdf_emit_all(impl, sink, " ", 1) != 0) return -1;
                impl->ansi_col++;
                impl->ansi_prev_char = ' ';
                impl->ansi_line_has_space = 1;
            }
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, "(", 1) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0) ||
                mdf_emit_buffer_append(impl, url, url_len) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
                mdf_emit_buffer_append(impl, ")", 1) != 0 ||
                ansi_flush_pending_fallback_emit(impl, sink) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (ansi_pending_emit_append(impl, impl->emit_buf, impl->emit_len) != 0 ||
                mdf_emit_buffer_reset(impl) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            impl->ansi_pending_fallback_emit_valid = 1;
            ansi_update_visible_output_state(impl, "(", 1);
            ansi_update_visible_output_state(impl, url, url_len);
            ansi_update_visible_output_state(impl, ")", 1);
            return 0;
        }
        if (impl->opts.width > 0 &&
            impl->ansi_col > 0 &&
            impl->ansi_col + 1 + (int)wrapped_fallback_cols <= impl->opts.width) {
            if (label_reset_pending) {
                if (mdf_emit_buffer_reset(impl) != 0 ||
                    mdf_emit_buffer_append(impl, "\033[0m ", 5) != 0) {
                    mdf_impl_mark_oom(impl);
                    return -1;
                }
                if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
                label_reset_pending = 0;
            } else if (mdf_emit_all(impl, sink, " ", 1) != 0) {
                return -1;
            }
            impl->ansi_col++;
            impl->ansi_prev_char = ' ';
            impl->ansi_line_has_space = 1;
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, "(", 1) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0) ||
                mdf_emit_buffer_append(impl, url, url_len) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
                mdf_emit_buffer_append(impl, ")", 1) != 0 ||
                ansi_flush_pending_fallback_emit(impl, sink) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (ansi_pending_emit_append(impl, impl->emit_buf, impl->emit_len) != 0 ||
                mdf_emit_buffer_reset(impl) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            impl->ansi_pending_fallback_emit_valid = 1;
            ansi_update_visible_output_state(impl, "(", 1);
            ansi_update_visible_output_state(impl, url, url_len);
            ansi_update_visible_output_state(impl, ")", 1);
            return 0;
        }
        if (label_reset_pending) {
            if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            label_reset_pending = 0;
        }
        if (impl->ansi_col <= ansi_current_prefix_width(impl) &&
            impl->opts.width > 0 &&
            wrapped_fallback_cols <= (size_t)impl->opts.width) {
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, "(", 1) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0) ||
                mdf_emit_buffer_append(impl, url, url_len) != 0 ||
                (!impl->opts.boring && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
                mdf_emit_buffer_append(impl, ")", 1) != 0 ||
                ansi_flush_pending_fallback_emit(impl, sink) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (ansi_pending_emit_append(impl, impl->emit_buf, impl->emit_len) != 0 ||
                mdf_emit_buffer_reset(impl) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            impl->ansi_pending_fallback_emit_valid = 1;
            ansi_update_visible_output_state(impl, "(", 1);
            ansi_update_visible_output_state(impl, url, url_len);
            ansi_update_visible_output_state(impl, ")", 1);
            return 0;
        }
        inputs[0].text = wrapped_fallback ? "(" : " (";
        inputs[0].len = wrapped_fallback ? 1 : 2;
        inputs[0].style = "";
        inputs[0].kind = ANSI_SIM_STRUCT;
        inputs[1].text = url;
        inputs[1].len = url_len;
        inputs[1].style = impl->opts.boring ? "" : mdf_theme_link_url(impl);
        inputs[1].kind = ANSI_SIM_URL;
        inputs[2].text = ")";
        inputs[2].len = 1;
        inputs[2].style = "";
        inputs[2].kind = ANSI_SIM_STRUCT;
        saved_pending_inline_style = impl->ansi_pending_inline_style;
        saved_active_inline_style = impl->ansi_active_inline_style;
        impl->ansi_pending_inline_style = NULL;
        impl->ansi_active_inline_style = NULL;
        if (ansi_sim_emit_inputs(impl, sink, inputs, 3) != 0) {
            impl->ansi_pending_inline_style = saved_pending_inline_style;
            impl->ansi_active_inline_style = saved_active_inline_style;
            return -1;
        }
        if (after_link_style != NULL && after_link_style[0] != '\0') {
            impl->ansi_pending_inline_style = after_link_style;
        } else {
            impl->ansi_pending_inline_style = saved_pending_inline_style != NULL ? saved_pending_inline_style : saved_active_inline_style;
        }
        impl->ansi_active_inline_style = NULL;
    }
    return 0;
}

static int ansi_emit_link_parts(mdf_impl *impl, mdf_sink *sink, const char *text, size_t text_len, const char *url, size_t url_len, const char *prefix_style)
{
    return ansi_emit_link_parts_ex(impl, sink, text, text_len, url, url_len, prefix_style, NULL);
}

int ansi_emit_link_fallback_only(mdf_impl *impl, mdf_sink *sink, const char *url, size_t url_len, int outer_paren_context)
{
    ansi_sim_input inputs[3];
    size_t fallback_cols;
    size_t wrapped_fallback_cols;
    int wrapped_fallback;

    if (!impl->opts.boring) {
        impl->ansi_pending_style_reset = 1;
    }
    wrapped_fallback = 0;
    wrapped_fallback_cols = visible_cols(url, url_len) + 2;
    fallback_cols = 2 + visible_cols(url, url_len) + 1;
    if (outer_paren_context &&
        impl->opts.width > 0 &&
        impl->ansi_col > 0 &&
        impl->ansi_col + (int)fallback_cols + 1 > impl->opts.width) {
        if (ansi_emit_newline(impl, sink) != 0) return -1;
        wrapped_fallback = 1;
    }
    if (!outer_paren_context &&
        impl->opts.width > 0 &&
        impl->ansi_col > 0 &&
        impl->ansi_col + 1 + (int)wrapped_fallback_cols > impl->opts.width &&
        wrapped_fallback_cols <= (size_t)impl->opts.width) {
        if (ansi_emit_newline(impl, sink) != 0) return -1;
        return ansi_set_pending_exact_fallback(impl, url, url_len);
    }
    if (impl->ansi_col <= ansi_current_prefix_width(impl) &&
        impl->opts.width > 0 &&
        wrapped_fallback_cols <= (size_t)impl->opts.width) {
        if (mdf_emit_buffer_reset(impl) != 0 ||
            mdf_emit_buffer_append(impl, "(", 1) != 0 ||
            (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_url(impl)) != 0) ||
            mdf_emit_buffer_append(impl, url, url_len) != 0 ||
            (!impl->opts.boring && mdf_emit_buffer_append(impl, "\033[0m", 4) != 0) ||
            mdf_emit_buffer_append(impl, outer_paren_context ? "))" : ")", outer_paren_context ? 2 : 1) != 0 ||
            ansi_flush_pending_fallback_emit(impl, sink) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        if (ansi_pending_emit_append(impl, impl->emit_buf, impl->emit_len) != 0 ||
            mdf_emit_buffer_reset(impl) != 0) {
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->ansi_pending_fallback_emit_valid = 1;
        ansi_update_visible_output_state(impl, "(", 1);
        ansi_update_visible_output_state(impl, url, url_len);
        ansi_update_visible_output_state(impl, outer_paren_context ? "))" : ")", outer_paren_context ? 2 : 1);
        if (outer_paren_context) {
            impl->pending_wrapped_link_outer_close = 1;
        }
        impl->ansi_pending_style_reset = 0;
        return 0;
    }
    inputs[0].text = wrapped_fallback ? "(" : " (";
    inputs[0].len = wrapped_fallback ? 1 : 2;
    inputs[0].style = "";
    inputs[0].kind = ANSI_SIM_STRUCT;
    inputs[1].text = url;
    inputs[1].len = url_len;
    inputs[1].style = impl->opts.boring ? "" : mdf_theme_link_url(impl);
    inputs[1].kind = ANSI_SIM_URL;
    inputs[2].text = ")";
    inputs[2].len = 1;
    inputs[2].style = "";
    inputs[2].kind = ANSI_SIM_STRUCT;
    return ansi_sim_emit_inputs(impl, sink, inputs, 3);
}

static int ansi_autolink_can_attach_trailing_punct(const char *text, size_t text_len, int is_email)
{
    size_t i;

    if (is_email || text_len < 3) {
        return 0;
    }
    for (i = 0; i + 2 < text_len; i++) {
        if (text[i] == ':' && text[i + 1] == '/' && text[i + 2] == '/') {
            return 1;
        }
    }
    return 0;
}

static int ansi_emit_autolink_text(mdf_impl *impl, mdf_sink *sink, const char *text, size_t text_len, int reset_before_link_style)
{
    int limit;
    size_t fitted_len;
    size_t start;
    size_t total_cols;
    int is_email;
    const char *saved_pending_inline_style;
    const char *saved_active_inline_style;

    start = 0;
    fitted_len = text_len;
    limit = ansi_content_limit(impl);
    total_cols = visible_cols(text, text_len);
    is_email = autolink_is_email(text, text_len);
    if (limit > 0) {
        if ((int)total_cols > limit) {
            start = url_visible_fit_start(text, text_len, limit);
        }
        if (start > 0) {
            fitted_len = text_len - start;
        } else if ((int)total_cols > limit) {
            fitted_len = (size_t)limit;
        }
    }
    if (impl->opts.osc8) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (ansi_flush_pending_space_for(impl, sink, fitted_len) != 0) return -1;
        if (impl->opts.width > 0 && impl->ansi_col > 0 &&
            impl->ansi_col + (int)fitted_len > impl->opts.width &&
            ansi_emit_newline(impl, sink) != 0) return -1;
        if (text_len > 0 && ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (ansi_write_osc8_start(impl, sink, is_email ? "mailto:" : "", text, text_len) != 0) return -1;
    } else {
        if (ansi_flush_pending_space_for(impl, sink, fitted_len) != 0) return -1;
    }
    if (reset_before_link_style &&
        !impl->opts.boring &&
        impl->ansi_col > ansi_current_prefix_width(impl)) {
        impl->ansi_pending_style_reset = 1;
    }
    saved_pending_inline_style = impl->ansi_pending_inline_style;
    saved_active_inline_style = impl->ansi_active_inline_style;
    if (limit > 0 &&
        start > 0 &&
        (ansi_word_contains_byte(text + start, text_len - start, '/') ||
         ansi_word_contains_byte(text + start, text_len - start, '?'))) {
        size_t off;
        int first;
        const char *link_style;

        impl->ansi_pending_inline_style = NULL;
        impl->ansi_active_inline_style = NULL;
        link_style = impl->opts.boring ? "" : mdf_theme_link_text(impl);
        off = start;
        first = 1;
        while (off < text_len) {
            unsigned long cp;
            size_t adv;

            adv = utf8_decode_codepoint(text + off, text_len - off, &cp);
            if (adv == 0) {
                break;
            }
            (void)cp;
            if (first && link_style[0] != '\0') {
                if (ansi_emit_styled_visible_chunk(impl, sink, link_style, text + off, adv) != 0) {
                    impl->ansi_pending_inline_style = saved_pending_inline_style;
                    impl->ansi_active_inline_style = saved_active_inline_style;
                    return -1;
                }
                first = 0;
            } else if (ansi_emit_visible_chunk(impl, sink, text + off, adv) != 0) {
                impl->ansi_pending_inline_style = saved_pending_inline_style;
                impl->ansi_active_inline_style = saved_active_inline_style;
                return -1;
            }
            off += adv;
        }
        impl->ansi_pending_inline_style = saved_pending_inline_style != NULL ? saved_pending_inline_style : saved_active_inline_style;
        impl->ansi_active_inline_style = NULL;
        if (impl->opts.osc8) {
            if (mdf_emit_cstr(impl, sink, "\033]8;;\033\\") != 0) return -1;
        }
        if (!impl->opts.boring) {
            impl->ansi_pending_style_reset = 1;
        }
        return 0;
    }
    impl->ansi_pending_inline_style = impl->opts.boring ? NULL : mdf_theme_link_text(impl);
    impl->ansi_active_inline_style = NULL;
    if (impl->opts.osc8 && start == 0 && (limit <= 0 || (int)total_cols <= limit)) {
        if (ansi_emit_visible_chunk(impl, sink, text, text_len) != 0) {
            impl->ansi_pending_inline_style = saved_pending_inline_style;
            impl->ansi_active_inline_style = saved_active_inline_style;
            return -1;
        }
    } else if (limit > 0 && ((int)total_cols > limit || start > 0)) {
        if (ansi_write_url_fit(impl, sink, text, text_len, limit) != 0) {
            impl->ansi_pending_inline_style = saved_pending_inline_style;
            impl->ansi_active_inline_style = saved_active_inline_style;
            return -1;
        }
    } else if (!impl->opts.osc8 && ansi_autolink_can_attach_trailing_punct(text, text_len, is_email)) {
        if (ansi_flush_pending_code_emit(impl, sink) != 0) {
            impl->ansi_pending_inline_style = saved_pending_inline_style;
            impl->ansi_active_inline_style = saved_active_inline_style;
            return -1;
        }
        impl->ansi_pending_emit_len = 0;
        impl->ansi_pending_emit_offsets_len = 0;
        impl->ansi_pending_emit_start_col = impl->ansi_col;
        impl->ansi_pending_emit_valid = 0;
        impl->ansi_pending_autolink_emit_valid = 0;
        if (mdf_emit_buffer_reset(impl) != 0 ||
            (!impl->opts.boring && mdf_emit_buffer_append_cstr(impl, mdf_theme_link_text(impl)) != 0) ||
            mdf_emit_buffer_append(impl, text, text_len) != 0 ||
            ansi_pending_emit_append(impl, impl->emit_buf, impl->emit_len) != 0 ||
            mdf_emit_buffer_reset(impl) != 0) {
            impl->ansi_pending_inline_style = saved_pending_inline_style;
            impl->ansi_active_inline_style = saved_active_inline_style;
            mdf_impl_mark_oom(impl);
            return -1;
        }
        impl->ansi_pending_autolink_emit_valid = 1;
        ansi_update_visible_output_state(impl, text, text_len);
    } else if (ansi_write_visible(impl, sink, text, text_len) != 0) {
        impl->ansi_pending_inline_style = saved_pending_inline_style;
        impl->ansi_active_inline_style = saved_active_inline_style;
        return -1;
    }
    if (!impl->ansi_pending_emit_valid && ansi_flush_word(impl, sink) != 0) {
        impl->ansi_pending_inline_style = saved_pending_inline_style;
        impl->ansi_active_inline_style = saved_active_inline_style;
        return -1;
    }
    impl->ansi_pending_inline_style = saved_pending_inline_style != NULL ? saved_pending_inline_style : saved_active_inline_style;
    impl->ansi_active_inline_style = NULL;
    if (impl->opts.osc8) {
        if (mdf_emit_cstr(impl, sink, "\033]8;;\033\\") != 0) return -1;
    }
    if (!impl->opts.boring) {
        impl->ansi_pending_style_reset = 1;
    }
    return 0;
}

static int ansi_inline_emit_link(mdf_impl *impl, mdf_sink *sink)
{
    const char *prefix;
    char heading_style_buf[160];

    prefix = impl->inline_emph_count > 0 ?
             ansi_inline_emphasis_prefix(impl, 0, heading_style_buf, sizeof(heading_style_buf)) :
             "";
    if (impl->heading_open && prefix != NULL && prefix[0] != '\0') {
        prefix = ansi_store_owned_inline_style(impl, prefix);
        if (prefix == NULL) return -1;
    }
    if (ansi_emit_link_parts(impl, sink, impl->inline_text, impl->inline_text_len, impl->inline_url, impl->inline_url_len, prefix) != 0) return -1;
    if (impl->inline_emph_count > 0) {
        impl->inline_mode = 5;
        impl->inline_emph_streaming = 2;
    } else {
        impl->inline_mode = 0;
    }
    impl->inline_text_len = 0;
    impl->inline_url_len = 0;
    return 0;
}

static int ansi_capture_inline_code_wrapped(mdf_impl *impl, const char *code, size_t code_len, int leading_space)
{
    ansi_pending_emit_sink pending;
    mdf_sink capture_sink;
    mdf_write_trace saved_trace;
    int rc;

    impl->ansi_pending_emit_len = 0;
    impl->ansi_pending_emit_offsets_len = 0;
    impl->ansi_pending_emit_start_col = impl->ansi_col;
    impl->ansi_pending_emit_leading_space = 0;
    if (impl->ansi_pending_emit_start_col < ansi_current_prefix_width(impl)) {
        impl->ansi_pending_emit_start_col = ansi_current_prefix_width(impl);
    }
    if (impl->ansi_pending_emit_start_col < impl->ansi_wrap_indent) {
        impl->ansi_pending_emit_start_col = impl->ansi_wrap_indent;
    }
    impl->ansi_pending_emit_valid = 0;
    pending.impl = impl;
    pending.oom = 0;
    capture_sink.userdata = &pending;
    capture_sink.write = ansi_pending_emit_write;
    saved_trace = impl->opts.write_trace;
    impl->opts.write_trace.userdata = NULL;
    impl->opts.write_trace.emit = NULL;
    if (leading_space) {
        if (ansi_pending_emit_append(impl, " ", 1) != 0) {
            impl->opts.write_trace = saved_trace;
            mdf_impl_mark_oom(impl);
            impl->ansi_pending_emit_len = 0;
            impl->ansi_pending_emit_offsets_len = 0;
            impl->ansi_pending_emit_valid = 0;
            impl->ansi_pending_emit_leading_space = 0;
            return -1;
        }
        impl->ansi_pending_emit_leading_space = 1;
        ansi_update_visible_output_state(impl, " ", 1);
    }
    if (impl->inline_outer_paren_pending) {
        if (ansi_pending_emit_append(impl, "(", 1) != 0) {
            impl->opts.write_trace = saved_trace;
            mdf_impl_mark_oom(impl);
            impl->ansi_pending_emit_len = 0;
            impl->ansi_pending_emit_offsets_len = 0;
            impl->ansi_pending_emit_valid = 0;
            impl->ansi_pending_emit_leading_space = 0;
            return -1;
        }
        if (!leading_space) {
            impl->ansi_pending_emit_offsets_len = 0;
        }
        ansi_update_visible_output_state(impl, "(", 1);
        impl->inline_outer_paren_pending = 0;
    }
    rc = ansi_write_inline_code_wrapped(impl, &capture_sink, code, code_len);
    impl->opts.write_trace = saved_trace;
    if (rc != 0) {
        if (pending.oom) {
            mdf_impl_mark_oom(impl);
        }
        impl->ansi_pending_emit_len = 0;
        impl->ansi_pending_emit_valid = 0;
        impl->ansi_pending_emit_leading_space = 0;
        return -1;
    }
    return 0;
}

static int ansi_inline_emit_code(mdf_impl *impl, mdf_sink *sink)
{
    const char *code;
    size_t code_len;
    size_t i;
    char *merged;
    size_t merged_len;
    size_t merged_cap;
    int trim_padding;
    int use_wrapped_code;
    int capture_leading_space;

    code = impl->inline_code;
    code_len = impl->inline_code_len;
    trim_padding = 0;
    if (code_len >= 3 && code[0] == ' ' && code[code_len - 1] == ' ') {
        for (i = 1; i + 1 < code_len; i++) {
            if (code[i] != ' ') {
                trim_padding = 1;
                break;
            }
        }
    }
    if (trim_padding) {
        code++;
        code_len -= 2;
    }
    use_wrapped_code = impl->opts.width > 0;
    capture_leading_space = 0;
    if (impl->format == MDF_FORMAT_HTML && impl->inline_outer_paren_pending) {
        if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
        if (ansi_flush_word(impl, sink) != 0) return -1;
        impl->inline_outer_paren_pending = 0;
    }
    if (impl->inline_outer_paren_pending &&
        code_len != 1 &&
        (!use_wrapped_code ||
         (impl->opts.width > 0 &&
          ansi_word_contains_byte(code, code_len, ' ') &&
          1 + (int)visible_cols(code, code_len) > impl->opts.width - ansi_current_prefix_width(impl)))) {
        if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
        impl->inline_outer_paren_pending = 0;
    }
    merged = NULL;
    merged_len = 0;
    merged_cap = 0;
    if (impl->ansi_word_len > 0 &&
        ansi_word_ends_with_open_bracket(impl->ansi_word, impl->ansi_word_len) &&
        ansi_word_contains_byte(code, code_len, ' ') &&
        impl->opts.width > 0 &&
        (int)(impl->ansi_word_cols + visible_cols(code, code_len)) > impl->opts.width &&
        impl->ansi_col + (impl->ansi_pending_space ? 1 : 0) + (int)impl->ansi_word_cols + (int)visible_cols(code, code_len) > impl->opts.width) {
        if (ansi_inline_append(impl, &merged, &merged_len, &merged_cap, impl->ansi_word, impl->ansi_word_len) != 0) return -1;
        if (ansi_inline_append(impl, &merged, &merged_len, &merged_cap, code, code_len) != 0) {
            mdf_free_mem(&impl->allocator, merged, merged_cap);
            return -1;
        }
        if (impl->ansi_pending_space) {
            if (ansi_flush_pending_space_only_for(impl, sink, visible_cols(merged, merged_len)) != 0) {
                mdf_free_mem(&impl->allocator, merged, merged_cap);
                return -1;
            }
        }
        impl->ansi_word_len = 0;
        impl->ansi_word_cols = 0;
        if (ansi_write_plain_code_wrapped(impl, sink, merged, merged_len) != 0) {
            mdf_free_mem(&impl->allocator, merged, merged_cap);
            return -1;
        }
        mdf_free_mem(&impl->allocator, merged, merged_cap);
        impl->inline_mode = 0;
        impl->inline_code_len = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (impl->ansi_word_len > 0) {
        if (ansi_flush_word_reserved(impl, sink, code_len) != 0) return -1;
    } else if (use_wrapped_code) {
        if (impl->ansi_pending_space &&
            !ansi_line_has_only_list_prefix(impl) &&
            (impl->ansi_active_inline_style == NULL || ansi_active_inline_style_is_code(impl)) &&
            !ansi_word_contains_byte(code, code_len, ' ') &&
            visible_cols(code, code_len) >= 8 &&
            impl->opts.width > 0 &&
            impl->ansi_col > 0 &&
            impl->ansi_col + 1 + (int)visible_cols(code, code_len) <= impl->opts.width) {
            capture_leading_space = 1;
            impl->ansi_pending_space = 0;
            impl->ansi_pending_space_no_split = 0;
            impl->ansi_pending_space_plain = 0;
        } else if (ansi_line_has_only_list_prefix(impl)) {
            size_t first_segment_cols;

            first_segment_cols = 1;
            for (first_segment_cols = 0; first_segment_cols < code_len; ) {
                unsigned long cp;
                size_t adv;

                adv = utf8_decode_codepoint(code + first_segment_cols, code_len - first_segment_cols, &cp);
                if (adv == 0) {
                    break;
                }
                first_segment_cols += adv;
                if (ansi_inline_code_delim(cp)) {
                    break;
                }
            }
            first_segment_cols = visible_cols(code, first_segment_cols);
            if (ansi_flush_pending_space_only_for(impl, sink, first_segment_cols) != 0) {
                return -1;
            }
        } else if (ansi_flush_pending_space_for(impl, sink, code_len) != 0) {
            return -1;
        }
    } else if (ansi_flush_pending_space_for(impl, sink, code_len) != 0) {
        return -1;
    }
    if (use_wrapped_code) {
        if (impl->inline_outer_paren_pending && ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (code_len > 0 && ansi_capture_inline_code_wrapped(impl, code, code_len, capture_leading_space) != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_code_len = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (impl->format == MDF_FORMAT_HTML) {
        if (code_len > 0 &&
            ansi_emit_styled_visible_chunk(impl, sink, impl->opts.boring ? "" : mdf_theme_code_inline(impl), code, code_len) != 0) {
            return -1;
        }
        if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        impl->inline_mode = 0;
        impl->inline_code_len = 0;
        impl->inline_code_delim_len = 0;
        impl->inline_code_pending_ticks = 0;
        return 0;
    }
    if (!impl->opts.boring && ansi_emit_styled_prefix(impl, sink, mdf_theme_code_inline(impl)) != 0) return -1;
    if (code_len > 0 && ansi_write_visible(impl, sink, code, code_len) != 0) return -1;
    if (ansi_flush_word(impl, sink) != 0) return -1;
    if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    impl->inline_mode = 0;
    impl->inline_code_len = 0;
    impl->inline_code_delim_len = 0;
    impl->inline_code_pending_ticks = 0;
    return 0;
}

static int ansi_inline_emit_pending_emphasis(mdf_impl *impl, mdf_sink *sink, size_t reserve_cols)
{
    const char *prefix;
    const char *effective_prefix;
    const char *continuation_prefix;
    const char *text;
    size_t text_len;
    size_t first_word_cols;
    size_t pos;
    size_t scan;
    size_t label_start;
    size_t label_end;
    size_t url_start;
    size_t url_end;
    size_t first_word_reserve;
    size_t attached_word_cols;
    int emitted_link;
    int merge_attached_suffix;
    int consumed_trailing_suffix;
    const char *attached_style;
    char heading_style_buf[160];

    impl->inline_emph_pending = 0;
    consumed_trailing_suffix = 0;
    text = impl->inline_emph;
    text_len = impl->inline_emph_len;
    if (impl->inline_emph_count == 2 && text_len >= 2 &&
        ((text[0] == '_' && text[text_len - 1] == '_') || (text[0] == '*' && text[text_len - 1] == '*'))) {
        prefix = mdf_theme_emphasis_strong(impl);
        text++;
        text_len -= 2;
    } else if (impl->inline_emph_count <= 1) {
        prefix = mdf_theme_emphasis(impl);
    } else if (impl->inline_emph_count == 2) {
        prefix = mdf_theme_strong(impl);
    } else {
        prefix = mdf_theme_emphasis_strong(impl);
    }
    effective_prefix = prefix;
    continuation_prefix = prefix;
    if (impl->heading_open) {
        effective_prefix = ansi_heading_inline_style(impl, impl->heading_level, prefix, heading_style_buf, sizeof(heading_style_buf));
        continuation_prefix = ansi_heading_inline_suffix(impl, prefix);
    }
    if (impl->inline_emph_after_word &&
        impl->ansi_word_len > 0 &&
        !ansi_text_contains_space(text, text_len)) {
        size_t total_cols;
        size_t style_len;

        total_cols = visible_cols(impl->ansi_word, impl->ansi_word_len) + visible_cols(text, text_len) + reserve_cols;
        if (impl->opts.width <= 0 ||
            impl->ansi_col == 0 ||
            impl->ansi_col + (int)total_cols <= impl->opts.width) {
            style_len = impl->opts.boring ? 0 : strlen(effective_prefix);
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, impl->ansi_word, impl->ansi_word_len) != 0 ||
                mdf_emit_buffer_append(impl, effective_prefix, style_len) != 0 ||
                mdf_emit_buffer_append(impl, text, text_len) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            ansi_update_visible_output_state(impl, impl->ansi_word, impl->ansi_word_len);
            ansi_update_visible_output_state(impl, text, text_len);
            impl->ansi_word_len = 0;
            impl->ansi_word_cols = 0;
            if (!impl->opts.boring) {
                impl->ansi_pending_style_reset = 1;
            }
            impl->ansi_active_inline_style = NULL;
            impl->ansi_pending_inline_style = NULL;
            impl->inline_mode = 0;
            impl->inline_emph_len = 0;
            impl->inline_emph_count = 0;
            impl->inline_emph_close_count = 0;
            impl->inline_emph_pending = 0;
            impl->inline_emph_after_word = 0;
            impl->ansi_pending_attached_style = NULL;
            return 0;
        }
    }
    first_word_reserve = reserve_cols;
    attached_word_cols = visible_first_word_cols(text, text_len) + visible_cols(impl->ansi_word, impl->ansi_word_len);
    merge_attached_suffix = !impl->inline_emph_after_word &&
                            impl->ansi_word_len > 0 &&
                            !ansi_text_contains_space(text, text_len) &&
                            ansi_word_has_em_dash_prefix(impl->ansi_word, impl->ansi_word_len) &&
                            impl->opts.width > 0 &&
                            attached_word_cols > (size_t)impl->opts.width &&
                            !ansi_emphasis_suffix_trailing_only(impl->ansi_word, impl->ansi_word_len);
    if (!merge_attached_suffix &&
        !impl->inline_emph_after_word &&
        impl->ansi_word_len > 0 &&
        ansi_text_contains_space(text, text_len) &&
        ansi_word_has_em_dash_prefix(impl->ansi_word, impl->ansi_word_len)) {
        first_word_reserve = 0;
    }
    first_word_cols = visible_first_word_cols(text, text_len) + first_word_reserve;
    if (!impl->inline_emph_after_word &&
        impl->ansi_word_len > 0 &&
        ansi_word_has_em_dash_prefix(impl->ansi_word, impl->ansi_word_len)) {
        first_word_cols += 2;
    }
    if (impl->inline_emph_after_word) {
        if (ansi_flush_word_reserved(impl, sink, first_word_cols) != 0) return -1;
        impl->inline_emph_after_word = 0;
    } else {
        if (ansi_flush_pending_space_only_for(impl, sink, first_word_cols) != 0) {
            return -1;
        }
    }
    if (impl->heading_open && (impl->heading_style_suspended || impl->heading_style_pending_prefix) &&
        impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) {
        continuation_prefix = effective_prefix;
    }
    if (impl->heading_open && !impl->opts.boring &&
        impl->ansi_col != (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) {
        impl->ansi_pending_style_reset = 1;
    }
    if (!ansi_text_contains_space(text, text_len) &&
        impl->ansi_word_len > 0 &&
        ansi_word_has_em_dash_prefix(impl->ansi_word, impl->ansi_word_len) &&
        !ansi_emphasis_suffix_trailing_only(impl->ansi_word, impl->ansi_word_len) &&
        !merge_attached_suffix &&
        text_len + impl->ansi_word_len + 1 <= 512) {
        const char *style;
        char combined[512];
        size_t combined_len;

        style = (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix;
        combined_len = text_len + impl->ansi_word_len;
        if (impl->opts.width > 0 &&
            visible_cols(text, text_len) + visible_cols(impl->ansi_word, impl->ansi_word_len) > (size_t)impl->opts.width) {
            memcpy(combined, text, text_len);
            memcpy(combined + text_len, impl->ansi_word, impl->ansi_word_len);
            if (ansi_write_styled_split_word_full_width(impl, sink, style, combined, combined_len) != 0) return -1;
            impl->ansi_pending_style_reset = 1;
        } else if (ansi_emit_styled_word_reset_suffix(impl, sink, style, text, text_len, impl->ansi_word, impl->ansi_word_len) != 0) return -1;
        impl->ansi_word_len = 0;
        impl->ansi_word_cols = 0;
        impl->ansi_pending_attached_style = NULL;
        if (impl->heading_open) {
            impl->heading_style_suspended = 1;
        }
        impl->inline_mode = 0;
        impl->inline_emph_len = 0;
        impl->inline_emph_count = 0;
        impl->inline_emph_close_count = 0;
        impl->inline_emph_pending = 0;
        impl->inline_emph_after_word = 0;
        return 0;
    }
    if (!ansi_text_contains_space(text, text_len) &&
        impl->ansi_word_len > 0 &&
        ansi_emphasis_suffix_trailing_only(impl->ansi_word, impl->ansi_word_len) &&
        !merge_attached_suffix &&
        text_len + impl->ansi_word_len + 64 <= 512) {
        const char *style;

        style = (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix;
        if (ansi_emit_styled_word_reset_suffix(impl, sink, style, text, text_len, impl->ansi_word, impl->ansi_word_len) != 0) return -1;
        impl->ansi_word_len = 0;
        impl->ansi_word_cols = 0;
        impl->ansi_pending_attached_style = NULL;
        if (impl->heading_open) {
            impl->heading_style_suspended = 1;
        }
        impl->inline_mode = 0;
        impl->inline_emph_len = 0;
        impl->inline_emph_count = 0;
        impl->inline_emph_close_count = 0;
        impl->inline_emph_pending = 0;
        impl->inline_emph_after_word = 0;
        return 0;
    }
    pos = 0;
    emitted_link = 0;
    while (pos < text_len) {
        if (ansi_flush_pending_exact_fallback(impl, sink, text[pos]) != 0) return -1;
        scan = pos;
        while (scan < text_len && text[scan] != '[' && text[scan] != '<') {
            scan++;
        }
        if (scan > pos) {
            size_t segment_start;
            size_t segment_len;
            size_t autolink_start;
            size_t autolink_end;
            size_t leading_spaces;
            size_t unstyled_segment_len;
            int trim_autolink_space;
            int trim_leading_space;

            segment_start = pos;
            segment_len = scan - segment_start;
            autolink_start = 0;
            autolink_end = 0;
            leading_spaces = 0;
            trim_autolink_space = 0;
            trim_leading_space = 0;
            while (leading_spaces < segment_len && text[segment_start + leading_spaces] == ' ') {
                leading_spaces++;
            }
            if (leading_spaces > 0 && leading_spaces < segment_len &&
                impl->opts.width > 0 && impl->ansi_col > 0 &&
                impl->ansi_col + 1 + (int)visible_first_word_cols(text + segment_start + leading_spaces, segment_len - leading_spaces) > impl->opts.width) {
                trim_leading_space = 1;
            }
            if (trim_leading_space) {
                impl->ansi_pending_space = 1;
                segment_start += leading_spaces;
                segment_len -= leading_spaces;
            }
            if (scan < text_len && text[scan] == '<') {
                autolink_start = scan + 1;
                autolink_end = autolink_start;
                while (autolink_end < text_len && text[autolink_end] != '>') {
                    autolink_end++;
                }
                if (autolink_end < text_len && autolink_likely(text + autolink_start, autolink_end - autolink_start)) {
                    unstyled_segment_len = segment_len;
                    while (unstyled_segment_len > 0 && text[segment_start + unstyled_segment_len - 1] == ' ') {
                        unstyled_segment_len--;
                    }
                    if (unstyled_segment_len < segment_len &&
                        impl->opts.width > 0 && impl->ansi_col > 0 &&
                        impl->ansi_col + 1 + (int)visible_cols(text + segment_start, unstyled_segment_len) +
                            (int)visible_cols(text + autolink_start, autolink_end - autolink_start) > impl->opts.width) {
                        trim_autolink_space = 1;
                    }
                }
                if (trim_autolink_space) {
                    while (segment_len > 0 && text[segment_start + segment_len - 1] == ' ') {
                        segment_len--;
                    }
                }
            }
            if (trim_leading_space && segment_len > 0 && impl->ansi_pending_space &&
                impl->opts.width > 0 && impl->ansi_col > 0 &&
                impl->ansi_col + 1 + (int)visible_first_word_cols(text + segment_start, segment_len) > impl->opts.width) {
                impl->ansi_pending_space = 0;
                impl->ansi_pending_space_no_split = 0;
                impl->ansi_pending_space_plain = 0;
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            if (segment_len > 0) {
                int had_trailing_suffix;

                had_trailing_suffix = scan >= text_len &&
                                      impl->ansi_word_len > 0 &&
                                      ansi_emphasis_suffix_trailing_only(impl->ansi_word, impl->ansi_word_len);
                attached_style = (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix;
                if (scan >= text_len && merge_attached_suffix && impl->ansi_word_len > 0 &&
                    segment_len + impl->ansi_word_len <= 512) {
                    char combined[512];
                    size_t combined_len;

                    combined_len = segment_len + impl->ansi_word_len;
                    memcpy(combined, text + segment_start, segment_len);
                    memcpy(combined + segment_len, impl->ansi_word, impl->ansi_word_len);
                    if (ansi_write_styled_split_word_full_width(impl, sink, attached_style, combined, combined_len) != 0) return -1;
                    impl->ansi_pending_style_reset = 1;
                    impl->ansi_word_len = 0;
                    impl->ansi_word_cols = 0;
                } else if (ansi_write_styled_span_reserved(impl, sink, text + segment_start, segment_len,
                        attached_style,
                        scan >= text_len ? reserve_cols : 0) != 0) return -1;
                if (had_trailing_suffix && impl->ansi_word_len == 0) {
                    consumed_trailing_suffix = 1;
                }
            }
            if (scan >= text_len && merge_attached_suffix && impl->ansi_word_len > 0) {
                attached_style = (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix;
                if (ansi_sim_emit_styled_direct(impl, sink, impl->ansi_word, impl->ansi_word_len,
                        attached_style, &attached_style) != 0) return -1;
                impl->ansi_word_len = 0;
                impl->ansi_word_cols = 0;
            }
            if (segment_start + segment_len < scan) {
                impl->ansi_pending_space = 1;
                impl->ansi_pending_space_no_split = 0;
                impl->ansi_pending_space_plain = 0;
            }
        }
        if (scan >= text_len) {
            pos = scan;
            break;
        }
        if (text[scan] == '<') {
            url_start = scan + 1;
            url_end = url_start;
            while (url_end < text_len && text[url_end] != '>') {
                url_end++;
            }
            if (url_end < text_len && autolink_likely(text + url_start, url_end - url_start)) {
                if (ansi_emit_autolink_text(impl, sink, text + url_start, url_end - url_start, 1) != 0) return -1;
                pos = url_end + 1;
                continue;
            }
            if (ansi_write_styled_words(impl, sink, text + scan, 1,
                    (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix) != 0) return -1;
            pos = scan + 1;
            continue;
        }
        label_start = scan + 1;
        label_end = label_start;
        while (label_end < text_len && text[label_end] != ']') {
            label_end++;
        }
        if (label_end >= text_len || label_end + 1 >= text_len || text[label_end + 1] != '(') {
            if (ansi_write_styled_words(impl, sink, text + scan, 1,
                    (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix) != 0) return -1;
            pos = scan + 1;
            continue;
        }
        url_start = label_end + 2;
        url_end = url_start;
        while (url_end < text_len && text[url_end] != ')') {
            url_end++;
        }
        if (url_end >= text_len) {
            if (ansi_write_styled_words(impl, sink, text + scan, 1,
                    (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix) != 0) return -1;
            pos = scan + 1;
            continue;
        }
        if ((emitted_link || scan > 0) && !impl->opts.osc8) {
            if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        }
        if (ansi_emit_link_parts(impl, sink, text + label_start, label_end - label_start, text + url_start, url_end - url_start,
                (impl->heading_open && impl->ansi_col == (impl->quote_wrap_active ? 0 : impl->heading_level + 1)) ? continuation_prefix : effective_prefix) != 0) return -1;
        emitted_link = 1;
        pos = url_end + 1;
        if (impl->ansi_pending_fallback_emit_valid &&
            ansi_flush_pending_fallback_emit(impl, sink) != 0) return -1;
        if (impl->pending_fallback_exact &&
            ansi_flush_pending_exact_fallback(impl, sink, pos < text_len ? text[pos] : '\0') != 0) return -1;
    }
    if (ansi_flush_pending_exact_fallback(impl, sink, '\0') != 0) return -1;
    if (!impl->opts.boring && !consumed_trailing_suffix) {
        impl->ansi_pending_style_reset = 1;
    }
    if (impl->heading_open) {
        impl->heading_style_suspended = 1;
    }
    impl->inline_mode = 0;
    impl->inline_emph_len = 0;
    impl->inline_emph_count = 0;
    impl->inline_emph_close_count = 0;
    impl->inline_emph_pending = 0;
    impl->inline_emph_after_word = 0;
    impl->ansi_pending_attached_style = NULL;
    return 0;
}

static const char *ansi_inline_emphasis_prefix(mdf_impl *impl, int continuation, char *buf, size_t buf_cap)
{
    const char *prefix;

    if (impl->inline_emph_nested_delim != 0 &&
        impl->inline_emph_nested_count > 0) {
        prefix = ansi_inline_nested_emphasis_style(impl,
                                                  impl->inline_emph_nested_count,
                                                  buf,
                                                  buf_cap);
    } else if (impl->inline_emph_nested_delim != 0 &&
        impl->inline_emph_nested_count == 0 &&
        impl->inline_emph_count == 2) {
        prefix = mdf_theme_emphasis_strong(impl);
    } else if (impl->inline_emph_count <= 1) {
        prefix = mdf_theme_emphasis(impl);
    } else if (impl->inline_emph_count == 2) {
        prefix = mdf_theme_strong(impl);
    } else {
        prefix = mdf_theme_emphasis_strong(impl);
    }
    if (impl->heading_open) {
        return continuation ? ansi_heading_inline_suffix(impl, prefix) :
                              ansi_heading_inline_style(impl, impl->heading_level, prefix, buf, buf_cap);
    }
    return prefix;
}

static const char *ansi_inline_outer_emphasis_prefix(mdf_impl *impl, int continuation, char *buf, size_t buf_cap)
{
    const char *prefix;

    if (impl->inline_emph_count <= 1) {
        prefix = mdf_theme_emphasis(impl);
    } else if (impl->inline_emph_count == 2) {
        prefix = mdf_theme_strong(impl);
    } else {
        prefix = mdf_theme_emphasis_strong(impl);
    }
    if (impl->heading_open) {
        return continuation ? ansi_heading_inline_suffix(impl, prefix) :
                              ansi_heading_inline_style(impl, impl->heading_level, prefix, buf, buf_cap);
    }
    return prefix;
}

void ansi_inline_clear_emphasis_state(mdf_impl *impl)
{
    impl->inline_mode = 0;
    impl->inline_emph_len = 0;
    impl->inline_emph_count = 0;
    impl->inline_emph_close_count = 0;
    impl->inline_emph_pending = 0;
    impl->inline_emph_after_word = 0;
    impl->inline_emph_streaming = 0;
    impl->inline_emph_skip_spaces = 0;
    impl->inline_emph_nested_delim = 0;
    impl->inline_emph_nested_count = 0;
    impl->inline_emph_nested_close_count = 0;
    impl->inline_emph_nested_saw_space = 0;
    impl->ansi_pending_final_emph_style = NULL;
    impl->ansi_pending_final_emph_suffix = 0;
    impl->ansi_pending_final_emph_word_suffix = 0;
    impl->ansi_pending_final_emph_base_len = 0;
    impl->ansi_pending_final_emph_base_cols = 0;
}

int ansi_inline_emit_streamed_emphasis_word(mdf_impl *impl, mdf_sink *sink, int closing)
{
    const char *prefix;
    const char *active_prefix;
    size_t word_cols;
    char heading_style_buf[160];

    if (impl->inline_emph_streaming >= 2) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        return 0;
    }
    if (impl->inline_emph_len == 0) {
        return 0;
    }
    if (impl->inline_emph_streaming == 1 &&
        impl->inline_emph_nested_delim == 0 &&
        impl->inline_emph_count == 2 &&
        impl->inline_emph_len > 0 &&
        (impl->inline_emph[0] == '_' || impl->inline_emph[0] == '*') &&
        impl->inline_emph[0] != impl->inline_emph_delim) {
        impl->inline_emph_nested_delim = impl->inline_emph[0];
        memmove(impl->inline_emph, impl->inline_emph + 1, impl->inline_emph_len - 1);
        impl->inline_emph_len--;
        if (impl->inline_emph_len == 0) {
            return 0;
        }
    }
    word_cols = visible_cols(impl->inline_emph, impl->inline_emph_len);
    prefix = ansi_inline_emphasis_prefix(impl, closing ? 1 : 0, heading_style_buf, sizeof(heading_style_buf));
    active_prefix = ansi_inline_emphasis_prefix(impl, 1, NULL, 0);
    if (impl->inline_emph_after_word &&
        impl->ansi_word_len > 0 &&
        impl->inline_emph_len > 0 &&
        !ansi_text_contains_space(impl->inline_emph, impl->inline_emph_len)) {
        size_t total_cols;
        size_t style_len;

        total_cols = impl->ansi_word_cols + word_cols;
        if (impl->opts.width <= 0 || total_cols <= (size_t)impl->opts.width) {
            if (impl->ansi_pending_space &&
                impl->opts.width > 0 &&
                impl->ansi_col > 0 &&
                impl->ansi_col + 1 + (int)total_cols > impl->opts.width) {
                impl->ansi_pending_space = 0;
                impl->ansi_pending_space_no_split = 0;
                impl->ansi_pending_space_plain = 0;
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            } else if (impl->ansi_pending_space) {
                impl->ansi_pending_space = 0;
                impl->ansi_pending_space_no_split = 0;
                impl->ansi_pending_space_plain = 0;
                if (ansi_emit_visible_chunk(impl, sink, " ", 1) != 0) return -1;
            } else if (impl->opts.width > 0 &&
                       impl->ansi_col > 0 &&
                       impl->ansi_col + (int)total_cols > impl->opts.width) {
                if (ansi_emit_newline(impl, sink) != 0) return -1;
            }
            style_len = impl->opts.boring ? 0 : strlen(prefix);
            if (mdf_emit_buffer_reset(impl) != 0 ||
                mdf_emit_buffer_append(impl, impl->ansi_word, impl->ansi_word_len) != 0 ||
                mdf_emit_buffer_append(impl, prefix, style_len) != 0 ||
                mdf_emit_buffer_append(impl, impl->inline_emph, impl->inline_emph_len) != 0) {
                mdf_impl_mark_oom(impl);
                return -1;
            }
            if (mdf_emit_buffer_commit(impl, sink) != 0) return -1;
            impl->ansi_col += (int)total_cols;
            impl->ansi_prev_char = impl->inline_emph[impl->inline_emph_len - 1];
            impl->ansi_word_len = 0;
            impl->ansi_word_cols = 0;
            if (!impl->opts.boring) {
                impl->ansi_active_inline_style = active_prefix;
            }
            impl->inline_emph_after_word = 0;
            impl->inline_emph_len = 0;
            impl->inline_emph_streaming = 2;
            return 0;
        }
    }
    if (impl->inline_emph_after_word) {
        if (ansi_flush_word_reserved(impl, sink, word_cols) != 0) return -1;
        impl->inline_emph_after_word = 0;
    } else {
        if (ansi_flush_pending_space_only_for(impl, sink, word_cols) != 0) return -1;
    }
    if (ansi_write_styled_words_inner(impl, sink, impl->inline_emph, impl->inline_emph_len, prefix, 1, 0) != 0) {
        return -1;
    }
    if (!impl->opts.boring) {
        impl->ansi_active_inline_style = active_prefix;
    }
    impl->inline_emph_len = 0;
    impl->inline_emph_streaming = 2;
    return 0;
}

static int ansi_inline_flush_unmatched_nested_emphasis(mdf_impl *impl, mdf_sink *sink)
{
    int i;

    if (impl->inline_emph_nested_delim != 0 &&
        impl->inline_emph_nested_count == 0 &&
        impl->inline_emph_nested_close_count < 0) {
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        return 0;
    }
    if (impl->inline_emph_nested_delim != 0 &&
        impl->inline_emph_nested_count == 0 &&
        impl->inline_emph_nested_close_count > 0) {
        while (impl->inline_emph_nested_close_count > 0) {
            if (ansi_write_visible_char(impl, sink, impl->inline_emph_nested_delim) != 0) return -1;
            impl->inline_emph_nested_close_count--;
        }
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_saw_space = 0;
        return 0;
    }
    if (impl->inline_emph_nested_delim == 0 || impl->inline_emph_nested_count <= 0) {
        return 0;
    }
    if (impl->inline_emph_nested_saw_space && impl->ansi_word_len > 0) {
        size_t old_len;

        old_len = impl->ansi_word_len;
        for (i = 0; i < impl->inline_emph_nested_count; i++) {
            if (ansi_inline_append(impl,
                                   &impl->ansi_word,
                                   &impl->ansi_word_len,
                                   &impl->ansi_word_cap,
                                   &impl->inline_emph_nested_delim,
                                   1) != 0) {
                return -1;
            }
        }
        memmove(impl->ansi_word + impl->inline_emph_nested_count,
                impl->ansi_word,
                old_len);
        for (i = 0; i < impl->inline_emph_nested_count; i++) {
            impl->ansi_word[i] = impl->inline_emph_nested_delim;
        }
        impl->ansi_word_len = old_len + (size_t)impl->inline_emph_nested_count;
        impl->ansi_word_cols += impl->inline_emph_nested_count;
        impl->inline_emph_len = 0;
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_count = 0;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        impl->inline_emph_streaming = 2;
        return 0;
    }
    if (impl->ansi_word_len > 0) {
        size_t extra;
        size_t old_len;

        extra = (size_t)(impl->inline_emph_nested_count + impl->inline_emph_nested_close_count);
        old_len = impl->ansi_word_len;
        for (i = 0; i < (int)extra; i++) {
            if (ansi_inline_append(impl,
                                   &impl->ansi_word,
                                   &impl->ansi_word_len,
                                   &impl->ansi_word_cap,
                                   &impl->inline_emph_nested_delim,
                                   1) != 0) {
                return -1;
            }
        }
        memmove(impl->ansi_word + impl->inline_emph_nested_count,
                impl->ansi_word,
                old_len);
        for (i = 0; i < impl->inline_emph_nested_count; i++) {
            impl->ansi_word[i] = impl->inline_emph_nested_delim;
        }
        impl->ansi_word_len = old_len + extra;
        impl->ansi_word_cols += (int)extra;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_len = 0;
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        impl->inline_emph_streaming = 2;
        return 0;
    }
    for (i = 0; i < impl->inline_emph_nested_count; i++) {
        if (ansi_write_visible_char(impl, sink, impl->inline_emph_nested_delim) != 0) return -1;
    }
    if (impl->inline_emph_len > 0 &&
        ansi_write_visible(impl, sink, impl->inline_emph, impl->inline_emph_len) != 0) {
        return -1;
    }
    for (i = 0; i < impl->inline_emph_nested_close_count; i++) {
        if (ansi_write_visible_char(impl, sink, impl->inline_emph_nested_delim) != 0) return -1;
    }
    impl->inline_emph_len = 0;
    impl->inline_emph_nested_delim = 0;
    impl->inline_emph_nested_count = 0;
    impl->inline_emph_nested_close_count = 0;
    impl->inline_emph_nested_saw_space = 0;
    impl->inline_emph_streaming = 2;
    return 0;
}

static int ansi_inline_pop_pending_word_backslash(mdf_impl *impl)
{
    if (impl->ansi_word_len == 0 || impl->ansi_word[impl->ansi_word_len - 1] != '\\') {
        return 0;
    }
    impl->ansi_word_len--;
    if (impl->ansi_word_cols > 0) {
        impl->ansi_word_cols--;
    }
    impl->ansi_prev_char = impl->ansi_word_len > 0 ? impl->ansi_word[impl->ansi_word_len - 1] : 0;
    return 1;
}

static int ansi_inline_prev_emphasis_char_is_alnum(mdf_impl *impl)
{
    char c;

    if (impl->inline_emph_nested_delim != 0 &&
        impl->inline_emph_nested_count > 0 &&
        impl->inline_emph_len > 0) {
        c = impl->inline_emph[impl->inline_emph_len - 1];
        return isalnum((unsigned char)c) != 0;
    }
    if (impl->inline_emph_streaming < 2 && impl->inline_emph_len > 0) {
        c = impl->inline_emph[impl->inline_emph_len - 1];
    } else if (impl->ansi_word_len > 0) {
        c = impl->ansi_word[impl->ansi_word_len - 1];
    } else if (impl->ansi_pending_space || impl->inline_emph_skip_spaces) {
        return 0;
    } else {
        c = impl->ansi_prev_char;
    }
    return isalnum((unsigned char)c) != 0;
}

static int ansi_inline_underscore_is_intraword(mdf_impl *impl, const char *src, size_t i, size_t len)
{
    size_t run;

    if (!ansi_inline_prev_emphasis_char_is_alnum(impl)) {
        return 0;
    }
    run = 0;
    while (i + run < len && src[i + run] == '_') {
        run++;
    }
    if (i + run >= len) {
        return 1;
    }
    return isalnum((unsigned char)src[i + run]) != 0;
}

int ansi_inline_emit_streamed_emphasis_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (impl->inline_emph_nested_delim != 0 && impl->inline_emph_nested_count > 0) {
        if (c == ' ' || c == '\t' || c == '\n') {
            char space;

            impl->inline_emph_nested_saw_space = 1;
            space = ' ';
            if (ansi_inline_append(impl,
                                   &impl->ansi_word,
                                   &impl->ansi_word_len,
                                   &impl->ansi_word_cap,
                                   &space,
                                   1) != 0) {
                return -1;
            }
            impl->ansi_word_cols++;
            impl->ansi_prev_char = space;
            return 0;
        }
        if (c == impl->inline_emph_nested_delim &&
            impl->ansi_word_len > 0 &&
            impl->ansi_word[impl->ansi_word_len - 1] == '\\' &&
            impl->inline_emph_nested_close_count == 0) {
            impl->ansi_word_len--;
            if (impl->ansi_word_cols > 0) {
                impl->ansi_word_cols--;
            }
            return ansi_write_visible_char(impl, sink, c);
        }
        if (c == impl->inline_emph_nested_delim &&
            impl->ansi_word_len == 0 &&
            impl->ansi_prev_char == 0 &&
            impl->inline_emph_nested_close_count == 0 &&
            impl->inline_emph_nested_count < 3) {
            impl->inline_emph_nested_count++;
            if (!impl->opts.boring) {
                const char *nested_style;
                char nested_style_buf[160];

                nested_style = ansi_inline_emphasis_prefix(impl,
                                                           0,
                                                           nested_style_buf,
                                                           sizeof(nested_style_buf));
                if (impl->heading_open) {
                    nested_style = ansi_store_owned_inline_style(impl, nested_style);
                    if (nested_style == NULL) return -1;
                }
                impl->ansi_pending_inline_style = nested_style;
                impl->ansi_active_inline_style = NULL;
                impl->ansi_pending_style_reset = 1;
            }
            return 0;
        }
        if (c == '_' &&
            c == impl->inline_emph_nested_delim &&
            impl->inline_emph_nested_close_count == 0 &&
            (impl->ansi_word_len > 0 || impl->ansi_prev_char != 0)) {
            impl->inline_emph_nested_close_count++;
            return 0;
        }
        if (c == impl->inline_emph_nested_delim) {
            impl->inline_emph_nested_close_count++;
            if (impl->inline_emph_nested_close_count >= impl->inline_emph_nested_count) {
                const char *closed_style;
                const char *outer_style;
                char outer_style_buf[160];

                closed_style = NULL;
                outer_style = NULL;
                if (ansi_flush_word(impl, sink) != 0) return -1;
                if (!impl->opts.boring) {
                    closed_style = ansi_inline_nested_emphasis_style(impl,
                                                                     impl->inline_emph_nested_count,
                                                                     NULL,
                                                                     0);
                }
                impl->inline_emph_nested_delim = c;
                impl->inline_emph_nested_count = 0;
                impl->inline_emph_nested_close_count = -1;
                impl->inline_emph_nested_saw_space = 0;
                if (!impl->opts.boring) {
                    outer_style = ansi_inline_outer_emphasis_prefix(impl,
                                                                    0,
                                                                    outer_style_buf,
                                                                    sizeof(outer_style_buf));
                    if (impl->heading_open) {
                        outer_style = ansi_store_owned_inline_style(impl, outer_style);
                        if (outer_style == NULL) return -1;
                    }
                    impl->ansi_pending_inline_style = outer_style;
                    impl->ansi_active_inline_style = NULL;
                    if (closed_style != NULL && closed_style[0] != '\0') {
                        impl->ansi_pending_style_reset = 1;
                    }
                }
            }
            return 0;
        }
        while (impl->inline_emph_nested_close_count > 0) {
            if (ansi_write_visible_char(impl, sink, impl->inline_emph_nested_delim) != 0) return -1;
            impl->inline_emph_nested_close_count--;
        }
    }
    if (impl->inline_emph_skip_spaces) {
        if (c == ' ') {
            return 0;
        }
        impl->inline_emph_skip_spaces = 0;
    }
    if (impl->inline_emph_streaming < 2) {
        if (c == ' ') {
            if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 0) != 0) return -1;
            return ansi_handle_visible_space(impl, sink);
        }
        if ((c == '*' || c == '_') &&
            c != impl->inline_emph_delim &&
            impl->inline_emph_count <= 3 &&
            !(impl->inline_emph_count == 2 && impl->inline_emph_len == 0) &&
            !(impl->inline_emph_count == 2 &&
              impl->inline_emph_nested_delim == c &&
              impl->inline_emph_nested_count == 0)) {
            if (impl->inline_emph_len > 0 && impl->inline_emph[impl->inline_emph_len - 1] == '\\') {
                impl->inline_emph_len--;
                impl->inline_emph[impl->inline_emph_len] = '\0';
                if (ansi_inline_append(impl, &impl->inline_emph, &impl->inline_emph_len, &impl->inline_emph_cap, &c, 1) != 0) {
                    return -1;
                }
                impl->inline_emph_streaming = 1;
                return 0;
            }
            if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 0) != 0) return -1;
            if (ansi_flush_pending_space_only_for(impl, sink, 0) != 0) return -1;
            impl->inline_emph_nested_delim = c;
            impl->inline_emph_nested_count = 0;
            impl->inline_emph_nested_close_count = 1;
            impl->inline_emph_nested_saw_space = 0;
            impl->inline_emph_len = 0;
            impl->inline_emph_streaming = 2;
            return 0;
        }
        if (ansi_inline_append(impl, &impl->inline_emph, &impl->inline_emph_len, &impl->inline_emph_cap, &c, 1) != 0) {
            return -1;
        }
        impl->inline_emph_streaming = 1;
        return 0;
    }
    if (c == impl->inline_emph_delim &&
        impl->inline_emph_count > 1 &&
        impl->inline_emph_close_count > 0 &&
        impl->inline_emph_close_count < impl->inline_emph_count &&
        impl->ansi_word_len > (size_t)impl->inline_emph_close_count &&
        impl->ansi_word[0] == c) {
        int opener_count;
        const char *nested_style;
        char nested_style_buf[160];

        opener_count = impl->inline_emph_close_count;
        memmove(impl->ansi_word,
                impl->ansi_word + opener_count,
                impl->ansi_word_len - (size_t)opener_count);
        impl->ansi_word_len -= (size_t)opener_count;
        if (impl->ansi_word_cols >= (size_t)opener_count) {
            impl->ansi_word_cols -= (size_t)opener_count;
        }
        impl->inline_emph_close_count = 0;
        impl->inline_emph_nested_delim = c;
        impl->inline_emph_nested_count = opener_count;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        if (!impl->opts.boring) {
            nested_style = ansi_inline_emphasis_prefix(impl,
                                                       0,
                                                       nested_style_buf,
                                                       sizeof(nested_style_buf));
            if (impl->heading_open) {
                nested_style = ansi_store_owned_inline_style(impl, nested_style);
                if (nested_style == NULL) return -1;
            }
            impl->ansi_pending_inline_style = nested_style;
            impl->ansi_active_inline_style = NULL;
            impl->ansi_pending_style_reset = 1;
        }
        return ansi_inline_emit_streamed_emphasis_char(impl, sink, c);
    }
    if ((c == '*' || c == '_') &&
        c != impl->inline_emph_delim &&
        impl->inline_emph_count <= 3 &&
        !(impl->inline_emph_count == 2 &&
          impl->inline_emph_nested_delim == c &&
          impl->inline_emph_nested_count == 0)) {
        if (ansi_inline_pop_pending_word_backslash(impl)) {
            return ansi_write_visible_char(impl, sink, c);
        }
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (ansi_flush_pending_space_only_for(impl, sink, 0) != 0) return -1;
        impl->inline_emph_nested_delim = c;
        impl->inline_emph_nested_count = 0;
        impl->inline_emph_nested_close_count = 1;
        impl->inline_emph_nested_saw_space = 0;
        impl->inline_emph_len = 0;
        return 0;
    }
    return ansi_write_visible_char(impl, sink, c);
}

static int ansi_inline_emit_streamed_emphasis_literal_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (impl->inline_emph_nested_delim != 0 && impl->inline_emph_nested_count > 0) {
        while (impl->inline_emph_nested_close_count > 0) {
            if (ansi_write_visible_char(impl, sink, impl->inline_emph_nested_delim) != 0) {
                return -1;
            }
            impl->inline_emph_nested_close_count--;
        }
        return ansi_write_visible_char(impl, sink, c);
    }
    if (impl->inline_emph_skip_spaces) {
        impl->inline_emph_skip_spaces = 0;
    }
    if (impl->inline_emph_streaming < 2) {
        if (ansi_inline_append(impl, &impl->inline_emph, &impl->inline_emph_len, &impl->inline_emph_cap, &c, 1) != 0) {
            return -1;
        }
        impl->inline_emph_streaming = 1;
        return 0;
    }
    return ansi_write_visible_char(impl, sink, c);
}

int ansi_inline_flush_streamed_emphasis_closers(mdf_impl *impl, mdf_sink *sink)
{
    while (impl->inline_emph_close_count > 0) {
        if (ansi_inline_emit_streamed_emphasis_char(impl, sink, impl->inline_emph_delim) != 0) {
            return -1;
        }
        impl->inline_emph_close_count--;
    }
    return 0;
}

static int ansi_inline_close_streamed_emphasis(mdf_impl *impl, mdf_sink *sink)
{
    const char *prefix;
    int single_word;

    prefix = ansi_inline_emphasis_prefix(impl, 1, NULL, 0);
    single_word = impl->inline_emph_streaming < 2;
    if (single_word && impl->inline_emph_len > 0) {
        impl->inline_mode = 0;
        impl->inline_emph_pending = 1;
        impl->ansi_pending_attached_style = prefix;
        return 0;
    }
    if (!single_word && impl->ansi_word_len > 0) {
        if (impl->inline_emph_nested_delim != 0 &&
            impl->ansi_word_len > 0 &&
            impl->ansi_word[impl->ansi_word_len - 1] == impl->inline_emph_nested_delim) {
            impl->ansi_word_len--;
            if (impl->ansi_word_cols > 0) {
                impl->ansi_word_cols--;
            }
        }
        impl->inline_mode = 0;
        impl->inline_emph_len = 0;
        impl->inline_emph_count = 0;
        impl->inline_emph_close_count = 0;
        impl->inline_emph_pending = 0;
        impl->inline_emph_after_word = 0;
        impl->inline_emph_streaming = 0;
        impl->inline_emph_nested_delim = 0;
        impl->inline_emph_nested_count = 0;
        impl->inline_emph_nested_close_count = 0;
        impl->inline_emph_nested_saw_space = 0;
        impl->ansi_pending_attached_style = NULL;
        impl->ansi_pending_style_reset_after_word = 0;
        impl->ansi_pending_final_emph_style = prefix;
        impl->ansi_pending_final_emph_suffix = 0;
        impl->ansi_pending_final_emph_word_suffix = 0;
        impl->ansi_pending_final_emph_base_len = 0;
        impl->ansi_pending_final_emph_base_cols = 0;
        return 0;
    }
    if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 1) != 0) return -1;
    if (!impl->opts.boring) {
        impl->ansi_pending_style_reset = 1;
        if (single_word) {
            impl->ansi_pending_attached_style = prefix;
        }
    }
    impl->ansi_active_inline_style = NULL;
    impl->ansi_pending_inline_style = NULL;
    if (impl->heading_open) {
        impl->heading_style_suspended = 1;
    }
    ansi_inline_clear_emphasis_state(impl);
    return 0;
}

static int ansi_inline_handle_streamed_emphasis_delim(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->inline_emph_close_count < 0) {
        impl->inline_emph_close_count = 0;
    }
    if (!impl->inline_emph_streaming &&
        impl->inline_emph_len == 0 &&
        impl->inline_emph_close_count == 0 &&
        impl->inline_emph_count < 3) {
        impl->inline_emph_count++;
    } else {
        impl->inline_emph_close_count++;
        if (impl->inline_emph_close_count == impl->inline_emph_count &&
            !(impl->inline_emph_streaming >= 2 &&
              impl->ansi_pending_space &&
              impl->ansi_word_len == 0)) {
            if (ansi_inline_close_streamed_emphasis(impl, sink) != 0) return -1;
        }
    }
    return 0;
}

static const char *ansi_inline_nested_emphasis_style(mdf_impl *impl, int nested_count, char *buf, size_t buf_cap)
{
    int has_emphasis;
    int has_strong;
    const char *style;

    if ((impl->inline_emph_count == 1 && nested_count == 1) ||
        (impl->inline_emph_count == 2 && nested_count == 2)) {
        return impl->heading_open ? ansi_heading_style(impl, impl->heading_level) : "";
    }
    has_emphasis = impl->inline_emph_count == 1 || impl->inline_emph_count >= 3 ||
                   nested_count == 1 || nested_count >= 3;
    has_strong = impl->inline_emph_count >= 2 || nested_count >= 2;
    if (has_emphasis && has_strong) {
        style = mdf_theme_emphasis_strong(impl);
    } else if (has_strong) {
        style = mdf_theme_strong(impl);
    } else {
        style = mdf_theme_emphasis(impl);
    }
    if (impl->heading_open && buf == NULL) {
        return ansi_heading_inline_suffix(impl, style);
    }
    if (impl->heading_open) {
        return ansi_heading_inline_style(impl, impl->heading_level, style, buf, buf_cap);
    }
    return style;
}

static int markdown_escapable_char(char c)
{
    switch (c) {
    case '\\':
    case '`':
    case '*':
    case '_':
    case '{':
    case '}':
    case '[':
    case ']':
    case '(':
    case ')':
    case '#':
    case '+':
    case '-':
    case '.':
    case '!':
    case '<':
    case '>':
    case '|':
        return 1;
    }
    return 0;
}

static int ansi_inline_emit_entity(mdf_impl *impl, mdf_sink *sink)
{
    const char *decoded;
    int nbsp_entity;

    decoded = NULL;
    nbsp_entity = (impl->inline_entity_len == 4 && memcmp(impl->inline_entity, "nbsp", 4) == 0) ||
                  (impl->inline_entity_len == 4 && memcmp(impl->inline_entity, "#160", 4) == 0) ||
                  (impl->inline_entity_len == 4 &&
                   impl->inline_entity[0] == '#' &&
                   impl->inline_entity[1] == 'x' &&
                   (impl->inline_entity[2] == 'a' || impl->inline_entity[2] == 'A') &&
                   impl->inline_entity[3] == '0');
    if (impl->format == MDF_FORMAT_HTML &&
        ((impl->inline_entity_len == 4 && memcmp(impl->inline_entity, "#160", 4) == 0) ||
         (impl->inline_entity_len == 4 && memcmp(impl->inline_entity, "nbsp", 4) == 0) ||
         (impl->inline_entity_len == 4 &&
          impl->inline_entity[0] == '#' &&
          impl->inline_entity[1] == 'x' &&
          (impl->inline_entity[2] == 'a' || impl->inline_entity[2] == 'A') &&
          impl->inline_entity[3] == '0'))) {
        impl->inline_html_nbsp_pending = 1;
        impl->inline_html_nbsp_prev_digit = ascii_is_digit_char(impl->ansi_prev_char);
    } else if (nbsp_entity) {
        if (ansi_write_visible_nbsp(impl, sink) != 0) return -1;
    } else if (impl->inline_entity_len == 3 && memcmp(impl->inline_entity, "amp", 3) == 0) {
        decoded = "&";
    } else if (impl->inline_entity_len == 2 && memcmp(impl->inline_entity, "lt", 2) == 0) {
        decoded = "<";
    } else if (impl->inline_entity_len == 2 && memcmp(impl->inline_entity, "gt", 2) == 0) {
        decoded = ">";
    } else if (impl->inline_entity_len == 4 && memcmp(impl->inline_entity, "quot", 4) == 0) {
        decoded = "\"";
    }
    if (decoded != NULL) {
        if (ansi_write_visible_cstr(impl, sink, decoded) != 0) return -1;
    } else if (!nbsp_entity) {
        if (ansi_write_visible_cstr(impl, sink, "&") != 0) return -1;
        if (impl->inline_entity_len > 0 && ansi_write_visible(impl, sink, impl->inline_entity, impl->inline_entity_len) != 0) return -1;
        if (ansi_write_visible_cstr(impl, sink, ";") != 0) return -1;
    }
    impl->inline_mode = 0;
    if (!impl->inline_html_nbsp_pending) {
        impl->inline_entity_len = 0;
    }
    return 0;
}

static int autolink_likely(const char *s, size_t len)
{
    size_t i;
    int has_at;
    int has_dot_after_at;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i + 2 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            return 1;
        }
    }
    has_at = 0;
    has_dot_after_at = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '@') {
            has_at = 1;
        } else if (has_at && s[i] == '.') {
            has_dot_after_at = 1;
        }
    }
    return has_at && has_dot_after_at;
}

static int autolink_is_email(const char *s, size_t len)
{
    size_t i;
    int has_at;
    int has_dot_after_at;

    has_at = 0;
    has_dot_after_at = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == ':' && i + 2 < len && s[i + 1] == '/' && s[i + 2] == '/') {
            return 0;
        }
        if (s[i] == '@') {
            has_at = 1;
        } else if (has_at && s[i] == '.') {
            has_dot_after_at = 1;
        }
    }
    return has_at && has_dot_after_at;
}

static int ansi_inline_emit_autolink(mdf_impl *impl, mdf_sink *sink)
{
    if (autolink_likely(impl->inline_url, impl->inline_url_len)) {
        if (ansi_emit_autolink_text(impl, sink, impl->inline_url, impl->inline_url_len, 0) != 0) return -1;
    } else {
        if (ansi_write_visible_cstr(impl, sink, "<") != 0) return -1;
        if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
        if (ansi_write_visible_cstr(impl, sink, ">") != 0) return -1;
    }
    impl->inline_mode = 0;
    impl->inline_url_len = 0;
    return 0;
}

static int ansi_handle_pending_wrapped_link_punct(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (!impl->pending_wrapped_link_punct_valid ||
        c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        return 0;
    }
    if (ansi_flush_pending_wrapped_link_punct(impl, sink) != 0) return -1;
    return 0;
}

static int ansi_handle_pending_wrapped_link_fallback_char(mdf_impl *impl, mdf_sink *sink, char c, int *consumed)
{
    *consumed = 0;
    if (!impl->pending_wrapped_link_fallback) {
        return 0;
    }
    if (ansi_flush_pending_wrapped_link_fallback(impl, sink) != 0) return -1;
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
        ansi_set_pending_wrapped_link_punct(impl, c);
        *consumed = 1;
    }
    return 0;
}

static int ansi_char_attaches_to_pending_code(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?';
}

static void ansi_begin_inline_link_text(mdf_impl *impl, int outer_paren_pending)
{
    impl->inline_outer_paren_pending = outer_paren_pending;
    impl->inline_mode = 1;
    impl->inline_text_len = 0;
    impl->inline_url_len = 0;
}

static void ansi_begin_inline_code_open(mdf_impl *impl)
{
    impl->inline_mode = 8;
    impl->inline_code_len = 0;
    impl->inline_code_delim_len = 1;
    impl->inline_code_pending_ticks = 0;
}

static void ansi_begin_inline_emphasis(mdf_impl *impl, char delim)
{
    impl->inline_mode = 5;
    impl->inline_emph_delim = delim;
    impl->inline_emph_count = 1;
    impl->inline_emph_close_count = 0;
    impl->inline_emph_len = 0;
    impl->inline_emph_after_word = impl->ansi_word_len > 0;
    impl->inline_emph_streaming = 0;
    impl->inline_emph_nested_delim = 0;
    impl->inline_emph_nested_count = 0;
    impl->inline_emph_nested_close_count = 0;
    impl->inline_emph_nested_saw_space = 0;
}

static void ansi_begin_inline_entity(mdf_impl *impl)
{
    impl->inline_mode = 6;
    impl->inline_entity_len = 0;
}

static void ansi_begin_inline_autolink(mdf_impl *impl)
{
    impl->inline_mode = 7;
    impl->inline_url_len = 0;
}

int ansi_inline_flush_literal_char(mdf_impl *impl, mdf_sink *sink, char c)
{
    if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
    return ansi_write_visible_char(impl, sink, c);
}

static int ansi_handle_plain_visible_char(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len, size_t i, char c)
{
    char next;
    unsigned long next_cp;
    size_t next_adv;

    if (impl->ansi_punct_quote_pending) {
        if (ansi_is_quote_char(c)) {
            impl->ansi_punct_quote_pending = 0;
        } else {
            if (ansi_flush_word(impl, sink) != 0) {
                return -1;
            }
            impl->ansi_punct_quote_pending = 0;
        }
    }
    if (ansi_is_closing_punct_char(c) &&
        impl->ansi_word_len > 0 &&
        (impl->ansi_word[impl->ansi_word_len - 1] == '?' ||
         impl->ansi_word[impl->ansi_word_len - 1] == '!')) {
        if (ansi_flush_word(impl, sink) != 0) {
            return -1;
        }
    }
    if (ansi_write_visible_char(impl, sink, c) != 0) {
        return -1;
    }
    next = (i + 1 < len) ? src[i + 1] : '\0';
    next_cp = 0;
    next_adv = 0;
    if (i + 1 < len) {
        next_adv = utf8_decode_codepoint(src + i + 1, len - (i + 1), &next_cp);
    }
    if ((c == '?' || c == '!') && next_adv > 0 && ansi_is_quote_codepoint(next_cp)) {
        return 0;
    }
    if ((c == '?' || c == '!') && next == '\0') {
        return 0;
    }
    if (ansi_is_punct_boundary_char(c, next)) {
        size_t j;
        size_t tail_cols;

        if (next == '\0') {
            impl->ansi_punct_quote_pending = 1;
            return 0;
        }
        if (c == ':' &&
            impl->ansi_word_len >= 2 &&
            ascii_is_digit_char(impl->ansi_word[impl->ansi_word_len - 2]) &&
            ascii_is_digit_char(next)) {
            return ansi_flush_word(impl, sink);
        }
        if (ansi_is_closing_punct_char(next)) {
            return ansi_flush_word(impl, sink);
        }
        j = i + 1;
        while (j < len && src[j] != ' ' && src[j] != '\t' && src[j] != '\n' && src[j] != '\r') {
            j++;
        }
        tail_cols = visible_cols(src + i + 1, j - (i + 1));
        if (impl->opts.width > 0 &&
                   impl->ansi_col + (impl->ansi_pending_space ? 1 : 0) + (int)impl->ansi_word_cols + (int)tail_cols > impl->opts.width) {
            if (ansi_flush_word(impl, sink) != 0) return -1;
        }
        return ansi_flush_word(impl, sink);
    }
    return 0;
}

static int ansi_process_text(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t i;
    char c;
    int consumed;

    for (i = 0; i < len; i++) {
        c = src[i];
        if (impl->pending_wrapped_link_outer_close) {
            impl->pending_wrapped_link_outer_close = 0;
            if (c == ')') {
                continue;
            }
        }
        if (impl->suppress_list_marker_padding) {
            if (c == ' ' || c == '\t') {
                continue;
            }
            impl->suppress_list_marker_padding = 0;
        }
        if (ansi_handle_pending_wrapped_link_punct(impl, sink, c) != 0) return -1;
        if (impl->inline_html_nbsp_pending) {
            if (ansi_emit_pending_html_nbsp(impl, sink, c) != 0) return -1;
        }
        if (impl->inline_mode == 0) {
            if (impl->ansi_pending_fallback_emit_valid) {
                if (c == ')' || c == '.') {
                    if (ansi_emit_pending_fallback_with_char(impl, sink, c) != 0) return -1;
                    continue;
                }
                if (ansi_flush_pending_fallback_emit(impl, sink) != 0) return -1;
            }
            if (impl->ansi_pending_autolink_emit_valid) {
                if (ansi_char_attaches_to_pending_code(c)) {
                    if (ansi_emit_pending_autolink_with_punct(impl, sink, c) != 0) return -1;
                    continue;
                }
                if (ansi_flush_pending_autolink_emit(impl, sink) != 0) return -1;
            }
            if (impl->ansi_pending_emit_valid) {
                if (ansi_char_attaches_to_pending_code(c)) {
                    if (ansi_emit_pending_code_with_punct(impl, sink, c) != 0) return -1;
                    continue;
                }
                if (ansi_flush_pending_code_emit(impl, sink) != 0) return -1;
            }
            if (ansi_handle_pending_wrapped_link_fallback_char(impl, sink, c, &consumed) != 0) return -1;
            if (consumed) {
                continue;
            }
            if (c == '.') {
                if (ansi_emit_pending_exact_fallback_with_period(impl, sink, &consumed) != 0) return -1;
                if (consumed) {
                    continue;
                }
            }
            if (c == ')') {
                if (ansi_emit_pending_exact_fallback_with_close(impl, sink, &consumed) != 0) return -1;
                if (consumed) {
                    continue;
                }
            }
            if (ansi_flush_pending_exact_fallback(impl, sink, c) != 0) return -1;
        }
        switch (impl->inline_mode) {
        case 0:
reprocess_inline_char:
            if (impl->inline_outer_paren_candidate) {
                if (c == '[') {
                    impl->inline_outer_paren_candidate = 0;
                    ansi_begin_inline_link_text(impl, 1);
                    break;
                }
                if (c == '`') {
                    impl->inline_outer_paren_candidate = 0;
                    impl->inline_outer_paren_pending = 1;
                    goto reprocess_inline_char;
                }
                if (ansi_write_visible_cstr(impl, sink, "(") != 0) return -1;
                impl->inline_outer_paren_candidate = 0;
                goto reprocess_inline_char;
            }
            if (impl->inline_emph_pending) {
                if (!impl->inline_emph_after_word && c != ' ' && c != '\n' && c != '[' && c != '`' && c != '*' && c != '_') {
                    if (ansi_inline_append(impl, &impl->ansi_word, &impl->ansi_word_len, &impl->ansi_word_cap, &c, 1) != 0) return -1;
                    if (!utf8_continuation_byte(c)) {
                        impl->ansi_word_cols++;
                    }
                    impl->ansi_prev_char = c;
                    continue;
                }
                if (ansi_inline_emit_pending_emphasis(impl, sink, impl->inline_emph_after_word ? 0 : impl->ansi_word_cols) != 0) return -1;
                if (impl->inline_mode == 0 && c != ' ' && c != '\n') {
                    impl->ansi_active_inline_style = NULL;
                }
            }
            if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_write_visible_char(impl, sink, c) != 0) {
                    return -1;
                }
                break;
            } else if (c == '(') {
                impl->inline_outer_paren_candidate = 1;
            } else if (c == '[') {
                ansi_begin_inline_link_text(impl, 0);
            } else if (c == '`') {
                ansi_begin_inline_code_open(impl);
            } else if (c == '_' &&
                       (!impl->ansi_pending_space || impl->ansi_word_len > 0) &&
                       ascii_is_alnum_char(impl->ansi_prev_char)) {
                if (ansi_write_visible_nbsp(impl, sink) != 0) {
                    return -1;
                }
            } else if (c == '*' || c == '_') {
                if (ansi_word_is_opening_punct_only(impl->ansi_word, impl->ansi_word_len)) {
                    if (ansi_flush_word(impl, sink) != 0) return -1;
                }
                ansi_begin_inline_emphasis(impl, c);
            } else if (c == '&') {
                ansi_begin_inline_entity(impl);
            } else if (c == '<') {
                ansi_begin_inline_autolink(impl);
            } else {
                if (ansi_handle_plain_visible_char(impl, sink, src, len, i, c) != 0) return -1;
            }
            break;
        case 1:
            if (c == ']') {
                impl->inline_mode = 2;
            } else if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_inline_append(impl, &impl->inline_text, &impl->inline_text_len, &impl->inline_text_cap, &c, 1) != 0) {
                    return -1;
                }
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_text, &impl->inline_text_len, &impl->inline_text_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 2:
            if (c == '(') {
                impl->inline_mode = 3;
            } else {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            }
            break;
        case 3:
            if (c == ')') {
                if (ansi_inline_emit_link(impl, sink) != 0) return -1;
            } else if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                    return -1;
                }
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 4:
            if (c == '`') {
                impl->inline_mode = 9;
                impl->inline_code_pending_ticks = 1;
                if (impl->inline_code_pending_ticks == impl->inline_code_delim_len) {
                    if (ansi_inline_emit_code(impl, sink) != 0) return -1;
                }
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_code, &impl->inline_code_len, &impl->inline_code_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 8:
            if (c == '`') {
                impl->inline_code_delim_len++;
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else {
                impl->inline_mode = 4;
                if (ansi_inline_append(impl, &impl->inline_code, &impl->inline_code_len, &impl->inline_code_cap, &c, 1) != 0) {
                    return -1;
                }
            }
            break;
        case 9:
            if (c == '`') {
                impl->inline_code_pending_ticks++;
                if (impl->inline_code_pending_ticks == impl->inline_code_delim_len) {
                    if (ansi_inline_emit_code(impl, sink) != 0) return -1;
                }
            } else if (c == '\n') {
                if (ansi_write_backticks(impl, sink, impl->inline_code_pending_ticks) != 0) return -1;
                impl->inline_code_pending_ticks = 0;
                impl->inline_mode = 4;
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else {
                while (impl->inline_code_pending_ticks > 0) {
                    if (ansi_inline_append(impl, &impl->inline_code, &impl->inline_code_len, &impl->inline_code_cap, "`", 1) != 0) {
                        return -1;
                    }
                    impl->inline_code_pending_ticks--;
                }
                impl->inline_mode = 4;
                if (ansi_inline_append(impl, &impl->inline_code, &impl->inline_code_len, &impl->inline_code_cap, &c, 1) != 0) {
                    return -1;
                }
            }
            break;
        case 5:
ansi_case5_reprocess:
            if (impl->inline_emph_nested_delim != 0 &&
                impl->inline_emph_nested_count == 0 &&
                impl->inline_emph_nested_close_count < 0) {
                if (c == impl->inline_emph_nested_delim &&
                    c != impl->inline_emph_delim) {
                    break;
                }
                impl->inline_emph_nested_delim = 0;
                impl->inline_emph_nested_close_count = 0;
                impl->inline_emph_nested_saw_space = 0;
                goto ansi_case5_reprocess;
            }
            if (impl->inline_emph_nested_delim != 0 &&
                impl->inline_emph_nested_count == 0 &&
                impl->inline_emph_nested_close_count > 0) {
                if (c == impl->inline_emph_nested_delim &&
                    impl->inline_emph_nested_close_count < 3) {
                    impl->inline_emph_nested_close_count++;
                    break;
                }
                if (c == ' ' || c == '\t' || c == '\n') {
                    while (impl->inline_emph_nested_close_count > 0) {
                        if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, impl->inline_emph_nested_delim) != 0) return -1;
                        impl->inline_emph_nested_close_count--;
                    }
                    impl->inline_emph_nested_delim = 0;
                    impl->inline_emph_nested_saw_space = 0;
                    goto ansi_case5_reprocess;
                }
                impl->inline_emph_nested_count = impl->inline_emph_nested_close_count;
                impl->inline_emph_nested_close_count = 0;
                impl->inline_emph_nested_saw_space = 0;
                impl->ansi_prev_char = 0;
                if (!impl->opts.boring) {
                    const char *nested_style;
                    char nested_style_buf[160];

                    nested_style = ansi_inline_emphasis_prefix(impl,
                                                               0,
                                                               nested_style_buf,
                                                               sizeof(nested_style_buf));
                    if (impl->heading_open) {
                        nested_style = ansi_store_owned_inline_style(impl, nested_style);
                        if (nested_style == NULL) return -1;
                    }
                    impl->ansi_pending_inline_style = nested_style;
                    impl->ansi_active_inline_style = NULL;
                    impl->ansi_pending_style_reset = 1;
                }
                goto ansi_case5_reprocess;
            }
            if (impl->inline_emph_close_count >= impl->inline_emph_count &&
                c != impl->inline_emph_delim) {
                if (impl->inline_emph_close_count > impl->inline_emph_count ||
                    (impl->inline_emph_streaming >= 2 &&
                     impl->ansi_pending_space &&
                     impl->ansi_word_len == 0)) {
                    const char *nested_style;
                    char nested_style_buf[160];

                    if (ansi_flush_word(impl, sink) != 0) return -1;
                    if (ansi_flush_pending_space_only_for(impl, sink, 0) != 0) return -1;
                    impl->inline_emph_nested_delim = impl->inline_emph_delim;
                    impl->inline_emph_nested_count = impl->inline_emph_close_count;
                    impl->inline_emph_nested_close_count = 0;
                    impl->inline_emph_nested_saw_space = 0;
                    impl->inline_emph_close_count = 0;
                    impl->inline_emph_len = 0;
                    if (!impl->opts.boring) {
                        nested_style = ansi_inline_emphasis_prefix(impl,
                                                                   0,
                                                                   nested_style_buf,
                                                                   sizeof(nested_style_buf));
                        if (impl->heading_open) {
                            nested_style = ansi_store_owned_inline_style(impl, nested_style);
                            if (nested_style == NULL) return -1;
                        }
                        impl->ansi_pending_inline_style = nested_style;
                        impl->ansi_active_inline_style = NULL;
                        impl->ansi_pending_style_reset = 1;
                    }
                    goto ansi_case5_reprocess;
                }
                if (ansi_inline_close_streamed_emphasis(impl, sink) != 0) return -1;
                goto reprocess_inline_char;
            }
            if (impl->inline_emph_nested_delim == 0 &&
                impl->inline_emph_close_count > 0 &&
                impl->inline_emph_close_count <= impl->inline_emph_count &&
                c != impl->inline_emph_delim &&
                c != ' ' &&
                c != '\t' &&
                c != '\n') {
                const char *nested_style;
                char nested_style_buf[160];

                if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 0) != 0) return -1;
                if (ansi_flush_word(impl, sink) != 0) return -1;
                if (ansi_flush_pending_space_only_for(impl, sink, 0) != 0) return -1;
                impl->inline_emph_nested_delim = impl->inline_emph_delim;
                impl->inline_emph_nested_count = impl->inline_emph_close_count;
                impl->inline_emph_nested_close_count = 0;
                impl->inline_emph_nested_saw_space = 0;
                impl->inline_emph_close_count = 0;
                if (!impl->opts.boring) {
                    nested_style = ansi_inline_emphasis_prefix(impl,
                                                               0,
                                                               nested_style_buf,
                                                               sizeof(nested_style_buf));
                    if (impl->heading_open) {
                        nested_style = ansi_store_owned_inline_style(impl, nested_style);
                        if (nested_style == NULL) return -1;
                    }
                    impl->ansi_pending_inline_style = nested_style;
                    impl->ansi_active_inline_style = NULL;
                    impl->ansi_pending_style_reset = 1;
                }
                goto ansi_case5_reprocess;
            }
            if (impl->inline_emph_nested_delim != 0 && impl->inline_emph_nested_count > 0) {
                if (markdown_escapable_char(c) && ansi_inline_pop_pending_word_backslash(impl)) {
                    if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                    if (ansi_write_visible_char(impl, sink, c) != 0) return -1;
                    break;
                } else if (impl->inline_emph_nested_close_count == impl->inline_emph_nested_count &&
                    c != impl->inline_emph_nested_delim &&
                    !isalnum((unsigned char)c)) {
                    const char *closed_style;
                    const char *outer_style;
                    char outer_style_buf[160];

                    closed_style = NULL;
                    outer_style = NULL;
                    if (ansi_flush_word(impl, sink) != 0) return -1;
                    if (!impl->opts.boring) {
                        closed_style = ansi_inline_nested_emphasis_style(impl,
                                                                         impl->inline_emph_nested_count,
                                                                         NULL,
                                                                         0);
                    }
                    impl->inline_emph_nested_delim = 0;
                    impl->inline_emph_nested_count = 0;
                    impl->inline_emph_nested_close_count = 0;
                    impl->inline_emph_nested_saw_space = 0;
                    if (!impl->opts.boring) {
                        outer_style = ansi_inline_outer_emphasis_prefix(impl,
                                                                        0,
                                                                        outer_style_buf,
                                                                        sizeof(outer_style_buf));
                        if (impl->heading_open) {
                            outer_style = ansi_store_owned_inline_style(impl, outer_style);
                            if (outer_style == NULL) return -1;
                        }
                        impl->ansi_pending_inline_style = outer_style;
                        impl->ansi_active_inline_style = NULL;
                        if (closed_style != NULL && closed_style[0] != '\0') {
                            impl->ansi_pending_style_reset = 1;
                        }
                    }
                    goto ansi_case5_reprocess;
                } else if (c == impl->inline_emph_nested_delim) {
                    char nested_delim;

                    nested_delim = c;
                    if (ansi_inline_emit_streamed_emphasis_char(impl, sink, c) != 0) return -1;
                    if (impl->inline_emph_nested_delim == 0) {
                        while (i + 1 < len && src[i + 1] == nested_delim) {
                            i++;
                        }
                    }
                    break;
                } else if (c == impl->inline_emph_delim) {
                    if (ansi_inline_flush_unmatched_nested_emphasis(impl, sink) != 0) return -1;
                    if (ansi_inline_handle_streamed_emphasis_delim(impl, sink) != 0) return -1;
                    break;
                }
            }
            if (markdown_escapable_char(c) && ansi_inline_pop_pending_word_backslash(impl)) {
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_write_visible_char(impl, sink, c) != 0) return -1;
                break;
            }
            if (c == impl->inline_emph_delim) {
                if (ansi_inline_handle_streamed_emphasis_delim(impl, sink) != 0) return -1;
            } else if ((c == '*' || c == '_') &&
                       c != impl->inline_emph_delim &&
                       impl->inline_emph_count <= 3 &&
                       !(impl->inline_emph_count == 2 &&
                         impl->inline_emph_nested_delim == c &&
                         impl->inline_emph_nested_count == 0)) {
                size_t word_i;

                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (i + 1 < len && (src[i + 1] == ' ' || src[i + 1] == '\t' || src[i + 1] == '\n')) {
                    if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                    break;
                }
                if (impl->inline_emph_streaming < 2) {
                    for (word_i = 0; word_i < impl->inline_emph_len; word_i++) {
                        if (impl->inline_emph[word_i] == c) {
                            if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                            break;
                        }
                    }
                    if (word_i < impl->inline_emph_len) {
                        break;
                    }
                }
                for (word_i = 0; word_i < impl->ansi_word_len; word_i++) {
                    if (impl->ansi_word[word_i] == c) {
                        if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                        break;
                    }
                }
                if (word_i < impl->ansi_word_len) {
                    break;
                }
                if (c == '_' && ansi_inline_underscore_is_intraword(impl, src, i, len)) {
                    if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                    while (i + 1 < len && src[i + 1] == '_') {
                        i++;
                        if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                    }
                    break;
                }
                if (c != '_' && ansi_inline_underscore_is_intraword(impl, src, i, len)) {
                    if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
                    break;
                }
                if (ansi_inline_emit_streamed_emphasis_char(impl, sink, c) != 0) return -1;
            } else if (c == '[') {
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 0) != 0) return -1;
                impl->inline_mode = 10;
                impl->inline_text_len = 0;
                impl->inline_url_len = 0;
            } else if (c == '<') {
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_inline_emit_streamed_emphasis_word(impl, sink, 0) != 0) return -1;
                impl->inline_mode = 13;
                impl->inline_url_len = 0;
            } else if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_inline_emit_streamed_emphasis_literal_char(impl, sink, c) != 0) return -1;
            } else if (c == '\n') {
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_inline_emit_streamed_emphasis_char(impl, sink, ' ') != 0) return -1;
                impl->inline_emph_skip_spaces = 1;
            } else {
                if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
                if (ansi_inline_emit_streamed_emphasis_char(impl, sink, c) != 0) return -1;
            }
            break;
        case 10:
            if (c == ']') {
                impl->inline_mode = 11;
            } else if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_inline_append(impl, &impl->inline_text, &impl->inline_text_len, &impl->inline_text_cap, &c, 1) != 0) {
                    return -1;
                }
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_text, &impl->inline_text_len, &impl->inline_text_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 11:
            if (c == '(') {
                impl->inline_mode = 12;
            } else {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            }
            break;
        case 12:
            if (c == ')') {
                const char *label_prefix;
                const char *resume_prefix;
                char resume_style_buf[160];

                if (impl->inline_emph_nested_delim != 0 &&
                    impl->inline_emph_nested_count > 0) {
                    label_prefix = ansi_inline_emphasis_prefix(impl,
                                                               1,
                                                               resume_style_buf,
                                                               sizeof(resume_style_buf));
                    resume_prefix = ansi_store_owned_inline_style(
                        impl,
                        label_prefix);
                    if (resume_prefix == NULL) return -1;
                } else {
                    label_prefix = ansi_inline_emphasis_prefix(impl, 1, NULL, 0);
                    if (impl->heading_open) {
                        resume_prefix = ansi_inline_emphasis_prefix(impl,
                                                                   0,
                                                                   resume_style_buf,
                                                                   sizeof(resume_style_buf));
                        resume_prefix = ansi_store_owned_inline_style(impl, resume_prefix);
                        if (resume_prefix == NULL) return -1;
                    } else {
                        resume_prefix = NULL;
                    }
                }
                if (ansi_emit_link_parts_ex(impl, sink,
                        impl->inline_text,
                        impl->inline_text_len,
                        impl->inline_url,
                        impl->inline_url_len,
                        label_prefix,
                        resume_prefix) != 0) return -1;
                impl->inline_mode = 5;
                impl->inline_emph_streaming = 2;
                impl->inline_text_len = 0;
                impl->inline_url_len = 0;
            } else if (c == '\\' && i + 1 < len && markdown_escapable_char(src[i + 1])) {
                c = src[++i];
                if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                    return -1;
                }
            } else if (c == '\n') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 13:
            if (c == '>') {
                if (autolink_likely(impl->inline_url, impl->inline_url_len)) {
                    if (ansi_emit_autolink_text(impl, sink, impl->inline_url, impl->inline_url_len, 1) != 0) return -1;
                } else {
                    if (ansi_write_visible_cstr(impl, sink, "<") != 0) return -1;
                    if (impl->inline_url_len > 0 && ansi_write_visible(impl, sink, impl->inline_url, impl->inline_url_len) != 0) return -1;
                    if (ansi_write_visible_cstr(impl, sink, ">") != 0) return -1;
                }
                impl->inline_mode = 5;
                impl->inline_url_len = 0;
            } else if (c == ' ' || c == '\t') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (c == '\n' || impl->inline_url_len >= 2048) {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 6:
            if (c == ';') {
                if (ansi_inline_emit_entity(impl, sink) != 0) return -1;
            } else if (c == ' ' || c == '\t') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (c == '\n' || impl->inline_entity_len >= 32) {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_entity, &impl->inline_entity_len, &impl->inline_entity_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        case 7:
            if (c == '>') {
                if (ansi_inline_emit_autolink(impl, sink) != 0) return -1;
            } else if (c == ' ' || c == '\t') {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
                if (impl->ansi_pending_space) {
                    impl->ansi_pending_space_no_split = 1;
                }
            } else if (c == '\n' || impl->inline_url_len >= 256) {
                if (ansi_inline_flush_literal_char(impl, sink, c) != 0) return -1;
            } else if (ansi_inline_append(impl, &impl->inline_url, &impl->inline_url_len, &impl->inline_url_cap, &c, 1) != 0) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

int ansi_flush_pending_breaks(mdf_impl *impl, mdf_sink *sink)
{
    int emitted_break;

    emitted_break = 0;
    if (impl->pending_quote_paragraph_separator) {
        if (impl->pending_quote_paragraph_separator == 2) {
            if (ansi_emit_resumed_quote_paragraph(impl, sink, 0, &emitted_break) != 0) {
                return -1;
            }
            return 0;
        }
        impl->pending_quote_paragraph_separator = 0;
        if (impl->pending_quote_end && impl->ansi_col > 0) {
            if (ansi_emit_plain_newline(impl, sink) != 0) {
                return -1;
            }
            emitted_break = 1;
        }
        ansi_clear_pending_quote_end_state(impl);
        ansi_raise_pending_breaks(impl, 1);
    }
    if (impl->pending_quote_end) {
        if (ansi_write_newline(impl, sink) != 0) {
            return -1;
        }
        emitted_break = 1;
        ansi_clear_pending_quote_end_state(impl);
    }
    while (impl->pending_breaks > 0) {
        if (ansi_write_newline(impl, sink) != 0) {
            return -1;
        }
        emitted_break = 1;
        impl->pending_breaks--;
    }
    (void)emitted_break;
    return 0;
}

int ansi_flush_pending_list_marker(mdf_impl *impl, mdf_sink *sink)
{
    size_t marker_start;
    int marker_indent;
    int ordered_marker;

    if (!impl->pending_list_marker) {
        return 0;
    }
    if (ansi_flush_pending_breaks(impl, sink) != 0) return -1;
    if (ansi_flush_pending_space(impl, sink) != 0) return -1;
    marker_start = 0;
    while (marker_start < impl->pending_list_marker_len &&
           (impl->pending_list_marker_text[marker_start] == ' ' ||
            impl->pending_list_marker_text[marker_start] == '\t')) {
        marker_start++;
    }
    if (marker_start > 0) {
        if (ansi_write_visible(impl, sink, impl->pending_list_marker_text, marker_start) != 0) return -1;
        if (ansi_flush_word(impl, sink) != 0) return -1;
    }
    if (!impl->opts.boring) {
        if (ansi_emit_styled_visible_chunk(impl, sink, mdf_theme_list_marker(impl),
                impl->pending_list_marker_text + marker_start,
                impl->pending_list_marker_len - marker_start) != 0) return -1;
    } else if (ansi_emit_visible_chunk(impl, sink,
            impl->pending_list_marker_text + marker_start,
            impl->pending_list_marker_len - marker_start) != 0) return -1;
    ordered_marker = ansi_is_ordered_list_marker(impl->pending_list_marker_text + marker_start,
                                                 impl->pending_list_marker_len - marker_start);
    marker_indent = ordered_marker ?
                        ((int)(impl->pending_list_marker_len - marker_start) < 3 ? 3 : (int)(impl->pending_list_marker_len - marker_start)) :
                        (int)(impl->pending_list_marker_len - marker_start + 1);
    impl->ansi_wrap_indent = (int)marker_start + marker_indent;
    impl->ansi_wrap_indent_in_quote = impl->quote_open;
    if (!impl->opts.boring && ordered_marker && impl->pending_list_marker_quote_context) {
        if (mdf_emit_cstr(impl, sink, "\033[0m ") != 0) return -1;
        impl->ansi_col++;
        impl->ansi_prev_char = ' ';
        impl->ansi_line_has_space = 1;
    } else if (!impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        if (impl->pending_list_marker_quote_context) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            impl->ansi_col++;
            impl->ansi_prev_char = ' ';
            impl->ansi_line_has_space = 1;
        } else if (ansi_write_visible_cstr(impl, sink, " ") != 0) return -1;
    } else {
        if (ansi_write_visible_cstr(impl, sink, " ") != 0) return -1;
    }
    impl->suppress_list_marker_padding = ordered_marker && impl->pending_list_marker_quote_context;
    ansi_clear_pending_list_marker_state(impl);
    return 0;
}

int ansi_finish_heading_end(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->in_pre && ansi_flush_pre_code_buffer(impl, sink) != 0) return -1;
    if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
    if (ansi_flush_word(impl, sink) != 0) return -1;
    impl->heading_open = 0;
    impl->heading_level = 0;
    impl->heading_style_suspended = 0;
    if (!impl->opts.boring) {
        if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    }
    impl->ansi_active_inline_style = NULL;
    impl->ansi_pending_inline_style = NULL;
    impl->ansi_pending_style_reset = 0;
    if (!impl->quote_open) {
        ansi_raise_pending_breaks(impl, 1);
    }
    return ansi_write_newline(impl, sink);
}

static int ansi_handle_token_newline(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->inline_mode == 5 && !impl->in_pre) {
        if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
        if (ansi_inline_emit_streamed_emphasis_char(impl, sink, ' ') != 0) return -1;
        impl->inline_emph_skip_spaces = 1;
        return 0;
    }
    if (impl->pending_list_quote_blank_indent > 0 && !impl->in_pre) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        return ansi_emit_list_quote_blank(impl, sink);
    }
    if (impl->quote_wrap_active && !impl->in_pre) {
        if (ansi_flush_word(impl, sink) != 0) return -1;
        if (impl->heading_open) {
            return ansi_emit_newline(impl, sink);
        }
        return ansi_emit_quote_newline(impl, sink);
    }
    if (impl->list_item_open && !impl->in_pre) {
        if (impl->ansi_col == 0) {
            if (impl->list_blank_newline_emitted) {
                return 0;
            }
            impl->list_blank_newline_emitted = 1;
        } else {
            impl->list_blank_newline_emitted = 0;
        }
        impl->suppress_list_hardbreak_space = 1;
    }
    if (impl->in_pre) {
        ansi_pre_code_reset_line_state(impl);
    }
    return ansi_write_newline(impl, sink);
}

int ansi_write_token(mdf_renderer *self, const mdf_token *token, mdf_sink *sink)
{
    mdf_impl *impl;
    int i;
    int handled;
    int prior_quote_prefix_indent;
    int was_quote_open;
    int preflight;

    impl = (mdf_impl *)self->impl;
    preflight = ansi_prepare_for_token(impl, sink, token);
    if (preflight != 0) {
        return preflight < 0 ? -1 : 0;
    }
    prior_quote_prefix_indent = 0;
    was_quote_open = 0;
    switch (token->type) {
    case MDF_TOKEN_TEXT:
    case MDF_TOKEN_SPACE:
    case MDF_TOKEN_CODE_TEXT:
        if (ansi_flush_pending_list_marker_if_any(impl, sink) != 0) return -1;
        if (impl->suppress_list_hardbreak_space) {
            impl->suppress_list_hardbreak_space = 0;
            if (token->type == MDF_TOKEN_SPACE) {
                return 0;
            }
        }
        if (!impl->quote_open && !impl->pending_quote_end) {
            impl->quote_prefix_indent = 0;
        }
        if (ansi_flush_pending_breaks(impl, sink) != 0) return -1;
        if (!impl->heading_open && !impl->list_item_open && !impl->quote_open && !impl->in_pre) {
            impl->in_paragraph = 1;
        }
        if (token->type == MDF_TOKEN_CODE_TEXT) {
            if (impl->in_pre && impl->opts.width > 0) {
                if (ansi_inline_append(impl, &impl->pre_code_buf, &impl->pre_code_len, &impl->pre_code_cap, token->text, token->len) != 0) return -1;
                return 0;
            }
            if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
            if (ansi_flush_pending_space(impl, sink) != 0) return -1;
            if (impl->in_pre && impl->list_item_open && impl->ansi_col == 0 && impl->ansi_wrap_indent > 0) {
                for (i = 0; i < impl->ansi_wrap_indent; i++) {
                    if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
                    impl->ansi_col++;
                }
            }
            if (impl->in_pre && impl->code_style_open && token->len > 0 && token->text[0] == ' ' &&
                (impl->ansi_prev_char == '-' || impl->ansi_prev_char == '#')) {
                if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
                impl->code_style_open = 0;
            }
            if (impl->in_pre && !impl->opts.boring) {
                if (!impl->code_style_open) {
                    if (mdf_emit_cstr(impl, sink, mdf_theme_code_block(impl)) != 0) return -1;
                    impl->code_style_open = 1;
                }
            }
            impl->ansi_pending_space = 0;
            impl->ansi_pending_space_no_split = 0;
            impl->ansi_pending_space_plain = 0;
            if (mdf_emit_all(impl, sink, token->text, token->len) != 0) return -1;
            for (i = 0; i < (int)token->len; i++) {
                if (!utf8_continuation_byte(token->text[i])) {
                    impl->ansi_col++;
                }
                impl->ansi_prev_char = token->text[i];
            }
            return 0;
        }
        return ansi_process_text(impl, sink, token->text, token->len);
    case MDF_TOKEN_NEWLINE:
        if (ansi_flush_pending_list_marker_if_any(impl, sink) != 0) return -1;
        if (token->level < 0 && impl->list_item_open && !impl->in_pre) {
            return 0;
        }
        if (impl->inline_mode == 5 && !impl->in_pre) {
            return ansi_handle_token_newline(impl, sink);
        }
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
        if (impl->code_style_open) {
            if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            impl->code_style_open = 0;
        }
        return ansi_handle_token_newline(impl, sink);
    case MDF_TOKEN_PARAGRAPH_END:
        return ansi_end_paragraph(impl, sink);
    case MDF_TOKEN_HEADING_START:
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        if (ansi_flush_pending_breaks(impl, sink) != 0) return -1;
        if (ansi_flush_pending_space(impl, sink) != 0) return -1;
        impl->in_paragraph = 0;
        impl->heading_open = 1;
        impl->heading_level = token->level;
        impl->heading_style_suspended = 0;
        if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
        if (!impl->opts.boring) {
            const char *style;
            size_t style_len;
            char marker[32];
            size_t marker_len;

            style = ansi_heading_style(impl, token->level);
            style_len = strlen(style);
            marker_len = style_len + (size_t)token->level;
            if (marker_len > sizeof(marker)) {
                return -1;
            }
            memcpy(marker, style, style_len);
            for (i = 0; i < token->level; i++) {
                marker[style_len + (size_t)i] = '#';
            }
            if (mdf_emit_all(impl, sink, marker, marker_len) != 0) return -1;
            impl->ansi_col += token->level;
            impl->ansi_prev_char = '#';
        } else {
            for (i = 0; i < token->level; i++) {
                if (ansi_write_visible_cstr(impl, sink, "#") != 0) return -1;
            }
        }
        if (ansi_write_visible_cstr(impl, sink, " ") != 0) return -1;
        if (ansi_flush_pending_space(impl, sink) != 0) return -1;
        if (!impl->opts.boring) {
            if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            impl->heading_style_pending_prefix = 1;
        }
        return 0;
    case MDF_TOKEN_HEADING_END:
        return ansi_finish_heading_end(impl, sink);
    case MDF_TOKEN_BLOCKQUOTE_START:
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        prior_quote_prefix_indent = impl->quote_prefix_indent;
        was_quote_open = impl->quote_open;
        if (impl->pending_quote_paragraph_separator) {
            impl->pending_quote_paragraph_separator = 2;
            impl->quote_open = 1;
            impl->quote_wrap_active = 1;
            return 0;
        }
        if (impl->pending_quote_end) {
            handled = ansi_handle_pending_quote_block_start(impl, sink, token);
            if (handled != 0) {
                return handled < 0 ? -1 : 0;
            }
        }
        return ansi_begin_blockquote(impl, sink, prior_quote_prefix_indent, was_quote_open);
    case MDF_TOKEN_BLOCKQUOTE_END:
        return ansi_end_blockquote(impl, sink);
    case MDF_TOKEN_LIST_ITEM_START:
        return ansi_start_list_item(impl, sink, token);
    case MDF_TOKEN_LIST_ITEM_END:
        return ansi_finish_list_item(impl, sink);
    case MDF_TOKEN_TASK_UNCHECKED:
        return ansi_write_task_token(impl, sink, token, 0);
    case MDF_TOKEN_TASK_CHECKED:
        return ansi_write_task_token(impl, sink, token, 1);
    case MDF_TOKEN_CODE_BLOCK_START:
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
        if (ansi_flush_pending_breaks(impl, sink) != 0) return -1;
        ansi_pre_code_reset_line_state(impl);
        impl->in_pre = 1;
        return 0;
    case MDF_TOKEN_CODE_BLOCK_END:
        if (ansi_flush_pre_code_buffer(impl, sink) != 0) return -1;
        if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
        if (impl->code_style_open) {
            if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
            impl->code_style_open = 0;
        }
        impl->in_pre = 0;
        ansi_pre_code_reset_line_state(impl);
        ansi_raise_pending_breaks(impl, 1);
        return 0;
    case MDF_TOKEN_THEMATIC_BREAK:
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        return ansi_flush_word(impl, sink);
    case MDF_TOKEN_DOCUMENT_END:
        if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
        if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
        return ansi_finish_document_end(impl, sink);
    }
    return 0;
}

mdf_status mdf_renderer_write_token_internal(mdf_renderer *self, const mdf_token *token, mdf_sink *sink)
{
    mdf_impl *impl;
    int rc;

    impl = mdf_renderer_require_sink(self, sink, "write_token requires renderer, token, and sink");
    if (impl == NULL || token == NULL) {
        if (impl == NULL) {
            return MDF_ERROR_INVALID;
        }
        mdf_set_error(self, "write_token requires renderer, token, and sink");
        return MDF_ERROR_INVALID;
    }
    if (mdf_token_has_missing_text(token)) {
        mdf_set_error(self, "text token missing text");
        return MDF_ERROR_INVALID;
    }
    rc = impl->format == MDF_FORMAT_HTML ? html_write_token(self, token, sink) : ansi_write_token(self, token, sink);
    if (rc != 0) {
        return mdf_renderer_fail_from_impl(self, impl, "sink write failed");
    }
    return MDF_OK;
}

mdf_status mdf_renderer_begin_internal(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;

    impl = mdf_renderer_require_sink(self, sink, "begin requires renderer and sink");
    if (impl == NULL) {
        return MDF_ERROR_INVALID;
    }
    if (impl->format == MDF_FORMAT_HTML && !impl->html_open && html_start(self, sink) != 0) {
        return mdf_renderer_fail_from_impl(self, impl, "sink write failed");
    }
    return MDF_OK;
}

mdf_status ansi_renderer_finish(mdf_renderer *self, mdf_impl *impl, mdf_sink *sink)
{
    if (impl->ansi_col > 0 || impl->ansi_pending_space || impl->ansi_word_len > 0) {
        if (ansi_write_newline(impl, sink) != 0) {
            return mdf_renderer_fail_sink_write(self);
        }
    }
    return MDF_OK;
}

static mdf_status html_renderer_finish_flush_ansi_tail(mdf_renderer *self, mdf_impl *impl, mdf_sink *sink)
{
    if (!(impl->ansi_col > 0 || impl->ansi_pending_space || impl->ansi_word_len > 0)) {
        return MDF_OK;
    }
    if (html_bridge_run_ansi_newline(self, sink) != 0) {
        return mdf_renderer_fail_sink_write(self);
    }
    return MDF_OK;
}

static mdf_status html_renderer_finish_flush_pending_lines(mdf_renderer *self,
                                                           mdf_impl *impl,
                                                           mdf_sink *sink,
                                                           int *footer_needs_newline)
{
    html_state *state;

    state = html_state_get(impl);
    if (state == NULL) {
        return mdf_renderer_fail_from_impl(self, impl, "out of memory");
    }
    if (state->direct_prefix_open ||
        state->pending_newline ||
        state->line_plain_len > 0 ||
        state->stream_mode != 0) {
        *footer_needs_newline = 1;
    }
    if (state->direct_prefix_open) {
        if (html_direct_close_prefix_line(sink, 1) != 0) {
            return mdf_renderer_fail_sink_write(self);
        }
        state->direct_prefix_open = 0;
    }
    if (state->pending_newline || state->line_plain_len > 0) {
        if (html_flush_pending_lines(self, sink, 1, '\0') != 0) {
            return mdf_renderer_fail_sink_write(self);
        }
    }
    return MDF_OK;
}

mdf_status html_renderer_finish(mdf_renderer *self, mdf_impl *impl, mdf_sink *sink)
{
    int footer_needs_newline;
    mdf_status st;

    if (!impl->html_open && html_start(self, sink) != 0) {
        return mdf_renderer_fail_from_impl(self, impl, "sink write failed");
    }
    footer_needs_newline = !impl->html_body_emitted || impl->html_footer_needs_newline;
    st = html_renderer_finish_flush_ansi_tail(self, impl, sink);
    if (st != MDF_OK) {
        return st;
    }
    st = html_renderer_finish_flush_pending_lines(self, impl, sink, &footer_needs_newline);
    if (st != MDF_OK) {
        return st;
    }
    if (mdf_write_cstr(sink,
                       footer_needs_newline ? "\n</main>\n</body>\n</html>\n"
                                            : "</main>\n</body>\n</html>\n") != 0) {
        return mdf_renderer_fail_sink_write(self);
    }
    return MDF_OK;
}
