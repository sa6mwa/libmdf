#include "render_internal.h"

#define MDF_TABLE_CELL_RENDER_WIDTH 1000000000

struct parse_state {
    char prefix[256];
    size_t prefix_len;
    int at_line_start;
    int decided;
    int mode;
    int heading_level;
    int thematic_possible;
    char thematic_char;
    int thematic_count;
    int list_task_probe;
    int list_item_start_pending;
    int pending_task_space;
    int pending_soft_space;
    int hard_break;
    size_t trailing_spaces;
    char immediate_spaces[64];
    size_t immediate_spaces_len;
    char list_marker[32];
    size_t list_marker_len;
    int list_marker_quote_context;
    char task[4];
    size_t task_len;
    int quote_depth;
    int pending_quote_depth;
};

typedef struct frontmatter_filter {
    int passthrough;
    char *probe;
    size_t len;
    size_t cap;
    mdf_allocator *allocator;
} frontmatter_filter;

static mdf_status feed_parse_bytes(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, const char *buf, size_t n);
static mdf_status flush_pending_list_item_end(mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink);
static mdf_status flush_pending_list_blank_breaks(mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink);
static mdf_status flush_pending_bare_quote_blank(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, int keep_quote);

static void parse_reset_line(parse_state *ps, mdf_parser_impl *impl)
{
    int pending_soft_space;

    impl->prev_hard_break = ps->hard_break;
    pending_soft_space = ps->pending_soft_space;
    memset(ps, 0, sizeof(*ps));
    ps->pending_soft_space = pending_soft_space;
    ps->at_line_start = 1;
    ps->thematic_possible = 1;
}

static int ascii_lower_char(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ascii_token_equal(const char *s, size_t len, const char *want)
{
    size_t i;

    for (i = 0; i < len && want[i] != '\0'; i++) {
        if (ascii_lower_char((unsigned char)s[i]) != want[i]) {
            return 0;
        }
    }
    return i == len && want[i] == '\0';
}

static int chart_option_key_equal(const char *src, size_t len, const char *want)
{
    size_t i;

    for (i = 0; i < len && want[i] != '\0'; i++) {
        int c;

        c = ascii_lower_char((unsigned char)src[i]);
        if (c == '_') {
            c = '-';
        }
        if (c != want[i]) {
            return 0;
        }
    }
    return i == len && want[i] == '\0';
}

static void chart_trim_span(const char *src, size_t len, size_t *start, size_t *end)
{
    size_t a;
    size_t b;

    a = 0;
    while (a < len && (src[a] == ' ' || src[a] == '\t')) {
        a++;
    }
    b = len;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t')) {
        b--;
    }
    *start = a;
    *end = b;
}

static int chart_parse_bool_option(const char *src, size_t len, int *out)
{
    if (ascii_token_equal(src, len, "on") ||
        ascii_token_equal(src, len, "yes") ||
        ascii_token_equal(src, len, "true") ||
        ascii_token_equal(src, len, "1")) {
        *out = 1;
        return 1;
    }
    if (ascii_token_equal(src, len, "off") ||
        ascii_token_equal(src, len, "no") ||
        ascii_token_equal(src, len, "false") ||
        ascii_token_equal(src, len, "0")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int chart_parse_option(const char *src, size_t len, int *flags)
{
    size_t eq;
    size_t key_start;
    size_t key_end;
    size_t val_start;
    size_t val_end;
    int enabled;

    chart_trim_span(src, len, &key_start, &key_end);
    if (key_start == key_end) {
        return 1;
    }
    src += key_start;
    len = key_end - key_start;
    eq = 0;
    while (eq < len && src[eq] != '=') {
        eq++;
    }
    if (eq == len) {
        if (chart_option_key_equal(src, len, "sort")) {
            *flags &= ~MDF_CHART_FLAG_SORT_ASC;
            *flags |= MDF_CHART_FLAG_SORT_DESC;
            return 1;
        }
        if (chart_option_key_equal(src, len, "colored-bars") ||
            chart_option_key_equal(src, len, "color-bars")) {
            *flags &= ~MDF_CHART_FLAG_MONO_BARS;
            return 1;
        }
        return 0;
    }
    chart_trim_span(src, eq, &key_start, &key_end);
    chart_trim_span(src + eq + 1, len - eq - 1, &val_start, &val_end);
    if (chart_option_key_equal(src + key_start, key_end - key_start, "sort")) {
        const char *value;
        size_t value_len;

        value = src + eq + 1 + val_start;
        value_len = val_end - val_start;
        *flags &= ~(MDF_CHART_FLAG_SORT_ASC | MDF_CHART_FLAG_SORT_DESC);
        if (ascii_token_equal(value, value_len, "desc") ||
            ascii_token_equal(value, value_len, "descending")) {
            *flags |= MDF_CHART_FLAG_SORT_DESC;
            return 1;
        }
        if (ascii_token_equal(value, value_len, "asc") ||
            ascii_token_equal(value, value_len, "ascending")) {
            *flags |= MDF_CHART_FLAG_SORT_ASC;
            return 1;
        }
        if (ascii_token_equal(value, value_len, "off") ||
            ascii_token_equal(value, value_len, "none")) {
            return 1;
        }
        return 0;
    }
    if (chart_option_key_equal(src + key_start, key_end - key_start, "colored-bars") ||
        chart_option_key_equal(src + key_start, key_end - key_start, "color-bars")) {
        if (!chart_parse_bool_option(src + eq + 1 + val_start, val_end - val_start, &enabled)) {
            return 0;
        }
        if (enabled) {
            *flags &= ~MDF_CHART_FLAG_MONO_BARS;
        } else {
            *flags |= MDF_CHART_FLAG_MONO_BARS;
        }
        return 1;
    }
    return 0;
}

static int chart_info_kind(const char *src, size_t len)
{
    size_t start;
    size_t end;
    size_t token_start;
    size_t token_end;
    size_t i;
    int kind;
    int flags;

    chart_trim_span(src, len, &start, &end);
    src += start;
    len = end - start;
    token_end = 0;
    while (token_end < len && src[token_end] != ',') {
        token_end++;
    }
    chart_trim_span(src, token_end, &token_start, &end);
    kind = 0;
    flags = 0;
    if (ascii_token_equal(src + token_start, end - token_start, "mdf-bar-chart") ||
        ascii_token_equal(src + token_start, end - token_start, "mdf-horizontal-bar-chart")) {
        kind = MDF_CHART_KIND_HORIZONTAL_BAR;
    } else if (ascii_token_equal(src + token_start, end - token_start, "mdf-vertical-bar-chart")) {
        kind = MDF_CHART_KIND_VERTICAL_BAR;
    } else if (ascii_token_equal(src + token_start, end - token_start, "mdf-tile-chart")) {
        kind = MDF_CHART_KIND_TILE;
    }
    if (kind == 0) {
        return 0;
    }
    i = token_end;
    while (i < len) {
        size_t next;

        if (src[i] == ',') {
            i++;
        }
        next = i;
        while (next < len && src[next] != ',') {
            next++;
        }
        if (!chart_parse_option(src + i, next - i, &flags)) {
            return 0;
        }
        i = next;
    }
    return kind | flags;
}

static int chart_append(mdf_parser_impl *impl, const char *src, size_t len)
{
    char *next;
    size_t need;
    size_t cap;

    if (len == 0) {
        return 0;
    }
    if (impl->chart_len + len + 1 < impl->chart_len) {
        return -1;
    }
    need = impl->chart_len + len + 1;
    if (need > impl->chart_cap) {
        cap = impl->chart_cap == 0 ? 256 : impl->chart_cap;
        while (cap < need) {
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(&impl->allocator, impl->chart_buf, impl->chart_cap, cap);
        if (next == NULL) {
            return -1;
        }
        impl->chart_buf = next;
        impl->chart_cap = cap;
    }
    memcpy(impl->chart_buf + impl->chart_len, src, len);
    impl->chart_len += len;
    impl->chart_buf[impl->chart_len] = '\0';
    return 0;
}

static int fm_append(frontmatter_filter *fm, const char *src, size_t len)
{
    char *next;
    size_t cap;

    if (fm->len + len < fm->len) {
        return -1;
    }
    if (fm->len + len > fm->cap) {
        cap = fm->cap == 0 ? 4096 : fm->cap;
        while (cap < fm->len + len) {
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(fm->allocator, fm->probe, fm->cap, cap);
        if (next == NULL) {
            return -1;
        }
        fm->probe = next;
        fm->cap = cap;
    }
    memcpy(fm->probe + fm->len, src, len);
    fm->len += len;
    return 0;
}

static int frontmatter_opening_prefix_possible(const char *src, size_t len)
{
    size_t i;
    size_t j;
    char ch;

    if (len == 0) {
        return 1;
    }
    if (len < 3 &&
        (unsigned char)src[0] == 0xef &&
        (len == 1 || (unsigned char)src[1] == 0xbb)) {
        return 1;
    }
    i = 0;
    if (len >= 3 &&
        (unsigned char)src[0] == 0xef &&
        (unsigned char)src[1] == 0xbb &&
        (unsigned char)src[2] == 0xbf) {
        i = 3;
    }
    while (i < len && (src[i] == ' ' || src[i] == '\t')) {
        i++;
    }
    if (i == len) {
        return 1;
    }
    ch = src[i];
    if (ch != '-' && ch != '+' && ch != ';') {
        return 0;
    }
    j = i;
    while (j < len && src[j] == ch) {
        j++;
    }
    if (j - i > 3) {
        return 0;
    }
    if (j - i < 3 && j < len) {
        return 0;
    }
    if (j - i < 3) {
        return 1;
    }
    while (j < len) {
        if (src[j] == ' ' || src[j] == '\t' || src[j] == '\r') {
            j++;
            continue;
        }
        if (src[j] == '\n') {
            return 1;
        }
        return 0;
    }
    return 1;
}

#define TABLE_MAX_COLUMNS 32u

typedef struct table_filter {
    mdf_allocator *allocator;
    int state;
    int line_start;
    char *line;
    size_t line_len;
    size_t line_cap;
    size_t *line_chunk_offsets;
    size_t line_chunk_len;
    size_t line_chunk_cap;
    char *header;
    size_t header_len;
    char *delimiter;
    size_t delimiter_len;
    char **rows;
    size_t *row_lens;
    size_t rows_len;
    size_t rows_cap;
    int align[TABLE_MAX_COLUMNS];
    size_t cols;
    int truncated_columns;
    size_t indent_prefix;
    size_t quote_content_indent;
    int quote_prefix;
    int has_header;
    int row_layout_ready;
    size_t row_widths[TABLE_MAX_COLUMNS];
} table_filter;

#define TABLE_FILTER_STATE_PREFIX_PRELUDE 4

static void table_filter_init(table_filter *tf, mdf_allocator *allocator)
{
    memset(tf, 0, sizeof(*tf));
    tf->allocator = allocator;
    tf->line_start = 1;
}

static void table_filter_clear_rows(table_filter *tf)
{
    size_t i;

    for (i = 0; i < tf->rows_len; i++) {
        mdf_free_mem(tf->allocator, tf->rows[i], tf->row_lens[i] + 1);
    }
    tf->rows_len = 0;
}

static void table_filter_destroy(table_filter *tf)
{
    table_filter_clear_rows(tf);
    mdf_free_mem(tf->allocator, tf->line, tf->line_cap);
    mdf_free_mem(tf->allocator, tf->line_chunk_offsets, tf->line_chunk_cap * sizeof(tf->line_chunk_offsets[0]));
    mdf_free_mem(tf->allocator, tf->header, tf->header_len + 1);
    mdf_free_mem(tf->allocator, tf->delimiter, tf->delimiter_len + 1);
    mdf_free_mem(tf->allocator, tf->rows, tf->rows_cap * sizeof(tf->rows[0]));
    mdf_free_mem(tf->allocator, tf->row_lens, tf->rows_cap * sizeof(tf->row_lens[0]));
    memset(tf, 0, sizeof(*tf));
}

static int table_filter_append_mem(table_filter *tf, char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    char *next;
    size_t new_cap;
    size_t need;

    if (n > ((size_t)-1) - *len - 1) {
        return -1;
    }
    need = *len + n + 1;
    if (need > *cap) {
        new_cap = *cap == 0 ? 128 : *cap;
        while (new_cap < need) {
            new_cap *= 2;
        }
        next = (char *)mdf_realloc_mem(tf->allocator, *buf, *cap, new_cap);
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

static int table_filter_append_line(table_filter *tf, const char *src, size_t n)
{
    return table_filter_append_mem(tf, &tf->line, &tf->line_len, &tf->line_cap, src, n);
}

static void table_filter_reset_line(table_filter *tf)
{
    tf->line_len = 0;
    tf->line_chunk_len = 0;
}

static int table_filter_note_chunk_offset(table_filter *tf)
{
    size_t *next;
    size_t new_cap;

    if (tf->line_len == 0) {
        return 0;
    }
    if (tf->line_chunk_len > 0 && tf->line_chunk_offsets[tf->line_chunk_len - 1] == tf->line_len) {
        return 0;
    }
    if (tf->line_chunk_len == tf->line_chunk_cap) {
        new_cap = tf->line_chunk_cap == 0 ? 8 : tf->line_chunk_cap * 2;
        next = (size_t *)mdf_realloc_mem(tf->allocator,
                                         tf->line_chunk_offsets,
                                         tf->line_chunk_cap * sizeof(tf->line_chunk_offsets[0]),
                                         new_cap * sizeof(tf->line_chunk_offsets[0]));
        if (next == NULL) {
            return -1;
        }
        tf->line_chunk_offsets = next;
        tf->line_chunk_cap = new_cap;
    }
    tf->line_chunk_offsets[tf->line_chunk_len++] = tf->line_len;
    return 0;
}

static int table_filter_has_chunk_offset(const table_filter *tf, size_t offset)
{
    size_t lo;
    size_t hi;

    lo = 0;
    hi = tf->line_chunk_len;
    while (lo < hi) {
        size_t mid;
        size_t value;

        mid = lo + (hi - lo) / 2;
        value = tf->line_chunk_offsets[mid];
        if (value == offset) {
            return 1;
        }
        if (value < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/*
 * pkt.systems/mdf v0.7.0 still drops every second contiguous wide rune when
 * adjacent 3-byte UTF-8 runes are split 2+1 across successive chunks inside an
 * active table row. The parity oracle is the Go module, so match that behavior
 * here until upstream changes again.
 */
static void table_filter_apply_go_chunk_utf8_loss(table_filter *tf)
{
    size_t i;
    size_t out_len;
    int prev_split_kind;

    if (tf->line_chunk_len == 0 || tf->line_len == 0) {
        return;
    }
    out_len = 0;
    prev_split_kind = 0;
    i = 0;
    while (i < tf->line_len) {
        unsigned long cp;
        size_t adv;
        int start_boundary;
        int split12;
        int split21;

        adv = utf8_decode_codepoint(tf->line + i, tf->line_len - i, &cp);
        if (adv == 0) {
            adv = 1;
            start_boundary = 0;
            split12 = 0;
            split21 = 0;
        } else {
            start_boundary = table_filter_has_chunk_offset(tf, i);
            split12 = adv == 3 &&
                      utf8_display_width(cp) == 2 &&
                      !start_boundary &&
                      table_filter_has_chunk_offset(tf, i + 1) &&
                      !table_filter_has_chunk_offset(tf, i + 2);
            split21 = adv == 3 &&
                      utf8_display_width(cp) == 2 &&
                      !start_boundary &&
                      !table_filter_has_chunk_offset(tf, i + 1) &&
                      table_filter_has_chunk_offset(tf, i + 2);
        }
        if (adv == 3 &&
            utf8_display_width(cp) == 2 &&
            ((prev_split_kind == 2 && !start_boundary) ||
             (prev_split_kind == 1 && !start_boundary && !split12 && !split21))) {
            prev_split_kind = 0;
            i += adv;
            continue;
        }
        if (out_len != i) {
            memmove(tf->line + out_len, tf->line + i, adv);
        }
        out_len += adv;
        if (split21) {
            prev_split_kind = 2;
        } else if (split12) {
            prev_split_kind = 1;
        } else {
            prev_split_kind = 0;
        }
        i += adv;
    }
    tf->line_len = out_len;
    tf->line[out_len] = '\0';
}

static int table_filter_set_copy(table_filter *tf, char **dst, size_t *dst_len, const char *src, size_t n)
{
    char *copy;

    copy = (char *)mdf_alloc(tf->allocator, n + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, src, n);
    copy[n] = '\0';
    mdf_free_mem(tf->allocator, *dst, *dst_len + 1);
    *dst = copy;
    *dst_len = n;
    return 0;
}

static int table_filter_add_row(table_filter *tf, const char *src, size_t n)
{
    char **next_rows;
    size_t *next_lens;
    char *copy;
    size_t new_cap;

    if (tf->rows_len + 1 > tf->rows_cap) {
        new_cap = tf->rows_cap == 0 ? 8 : tf->rows_cap * 2;
        next_rows = (char **)mdf_realloc_mem(tf->allocator, tf->rows, tf->rows_cap * sizeof(tf->rows[0]), new_cap * sizeof(tf->rows[0]));
        if (next_rows == NULL) {
            return -1;
        }
        tf->rows = next_rows;
        next_lens = (size_t *)mdf_realloc_mem(tf->allocator, tf->row_lens, tf->rows_cap * sizeof(tf->row_lens[0]), new_cap * sizeof(tf->row_lens[0]));
        if (next_lens == NULL) {
            return -1;
        }
        tf->row_lens = next_lens;
        tf->rows_cap = new_cap;
    }
    copy = (char *)mdf_alloc(tf->allocator, n + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, src, n);
    copy[n] = '\0';
    tf->rows[tf->rows_len] = copy;
    tf->row_lens[tf->rows_len] = n;
    tf->rows_len++;
    return 0;
}

static size_t table_trim_left(const char *s, size_t start, size_t end)
{
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) {
        start++;
    }
    return start;
}

static size_t table_trim_right(const char *s, size_t start, size_t end)
{
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        end--;
    }
    return end;
}

static int table_row_bounds(const char *line, size_t len, size_t *start, size_t *end)
{
    size_t s;
    size_t e;
    size_t i;

    s = table_trim_left(line, 0, len);
    e = table_trim_right(line, s, len);
    if (s >= e) {
        return 0;
    }
    if (line[s] == '|') {
        s++;
    } else if (s >= 4) {
        return 0;
    }
    if (e > s && line[e - 1] == '|') {
        e--;
    }
    for (i = s; i < e; i++) {
        if (line[i] == '|') {
            *start = s;
            *end = e;
            return 1;
        }
    }
    if (line[table_trim_left(line, 0, len)] == '|' || (e > s && line[e - 1] == '|')) {
        *start = s;
        *end = e;
        return e > s;
    }
    return 0;
}

static int table_delimited_bounds(const char *line, size_t len, size_t *start, size_t *end)
{
    size_t s;
    size_t e;

    if (!table_row_bounds(line, len, &s, &e)) {
        return 0;
    }
    *start = s;
    *end = e;
    return 1;
}

static int table_is_ascii_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int table_is_ascii_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int table_is_ascii_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static size_t table_count_backtick_run(const char *s, size_t len)
{
    size_t count;

    count = 0;
    while (count < len && s[count] == '`') {
        count++;
    }
    return count;
}

static int table_has_closing_backtick_run(const char *s, size_t len, size_t ticks)
{
    size_t i;
    size_t run;

    i = 0;
    while (i < len) {
        if (s[i] != '`') {
            i++;
            continue;
        }
        run = table_count_backtick_run(s + i, len - i);
        if (run == ticks) {
            return 1;
        }
        i += run;
    }
    return 0;
}

static int table_has_closing_bracket(const char *s, size_t len)
{
    int depth;
    int escaped;
    size_t i;

    depth = 1;
    escaped = 0;
    for (i = 0; i < len; i++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (s[i] == '\\') {
            escaped = 1;
            continue;
        }
        if (s[i] == '[') {
            depth++;
        } else if (s[i] == ']') {
            depth--;
            if (depth == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int table_has_closing_link_destination(const char *s, size_t len)
{
    int depth;
    int escaped;
    char quote;
    size_t i;

    depth = 1;
    escaped = 0;
    quote = '\0';
    for (i = 0; i < len; i++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (s[i] == '\\') {
            escaped = 1;
            continue;
        }
        if (quote != '\0') {
            if (s[i] == quote) {
                quote = '\0';
            }
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            quote = s[i];
            continue;
        }
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            depth--;
            if (depth == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int table_angle_span_end(const char *s, size_t len)
{
    size_t i;
    char quote;

    if (len < 3 || s[0] != '<') {
        return -1;
    }
    quote = '\0';
    for (i = 1; i < len; i++) {
        if (quote != '\0') {
            if (s[i] == quote) {
                quote = '\0';
            }
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            quote = s[i];
            continue;
        }
        if (s[i] == '>') {
            return (int)i;
        }
    }
    return -1;
}

static int table_is_autolink_angle_body(const char *body, size_t len)
{
    size_t i;
    size_t at;
    size_t colon;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (table_is_ascii_whitespace(body[i])) {
            return 0;
        }
    }
    at = len;
    for (i = 0; i < len; i++) {
        if (body[i] == '@') {
            at = i;
            break;
        }
    }
    if (at > 0 && at < len - 1) {
        for (i = 0; i < len; i++) {
            if (body[i] == '<' || body[i] == '>') {
                return 0;
            }
        }
        return 1;
    }
    colon = len;
    for (i = 0; i < len; i++) {
        if (body[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon == 0 || colon >= len) {
        return 0;
    }
    for (i = 0; i < colon; i++) {
        if (i == 0) {
            if (!table_is_ascii_letter(body[i])) {
                return 0;
            }
        } else if (!table_is_ascii_letter(body[i]) &&
                   !table_is_ascii_digit(body[i]) &&
                   body[i] != '+' && body[i] != '.' && body[i] != '-') {
            return 0;
        }
    }
    for (i = colon + 1; i < len; i++) {
        if (body[i] == '<' || body[i] == '>') {
            return 0;
        }
    }
    return 1;
}

static int table_is_html_tag_angle_body(const char *body, size_t len)
{
    size_t i;
    char quote;

    if (len == 0) {
        return 0;
    }
    if (len >= 3 && memcmp(body, "!--", 3) == 0) {
        for (i = 3; i + 1 < len; i++) {
            if (body[i] == '-' && body[i + 1] == '-') {
                return 1;
            }
        }
        return 0;
    }
    if (body[0] == '/') {
        body++;
        len--;
    }
    if (len == 0 || !table_is_ascii_letter(body[0])) {
        return 0;
    }
    i = 1;
    while (i < len && (table_is_ascii_letter(body[i]) || table_is_ascii_digit(body[i]) || body[i] == '-')) {
        i++;
    }
    if (i == len) {
        return 1;
    }
    if (body[i] != '/' && !table_is_ascii_whitespace(body[i])) {
        return 0;
    }
    quote = '\0';
    for (; i < len; i++) {
        if (quote != '\0') {
            if (body[i] == quote) {
                quote = '\0';
            }
            continue;
        }
        if (body[i] == '"' || body[i] == '\'') {
            quote = body[i];
        }
    }
    return quote == '\0';
}

static int table_protected_angle_mode(const char *s, size_t len)
{
    size_t end;

    if (len < 3 || s[0] != '<') {
        return 0;
    }
    end = 1;
    while (end < len && s[end] != '>') {
        end++;
    }
    if (end >= len) {
        return 0;
    }
    if (table_is_autolink_angle_body(s + 1, end - 1)) {
        return 1;
    }
    if (table_is_html_tag_angle_body(s + 1, end - 1)) {
        return 2;
    }
    return 0;
}

static size_t table_next_pipe(const char *line, size_t start, size_t end)
{
    size_t i;
    size_t ticks;
    int escape;
    int bracket_depth;
    int paren_depth;
    int angle_mode;
    int after_bracket;
    char angle_quote;

    escape = 0;
    bracket_depth = 0;
    paren_depth = 0;
    angle_mode = 0;
    after_bracket = 0;
    angle_quote = '\0';
    i = start;
    while (i < end) {
        if (escape) {
            escape = 0;
            i++;
            continue;
        }
        if (angle_mode == 1) {
            if (line[i] == '>') {
                angle_mode = 0;
            }
            i++;
            continue;
        }
        if (angle_mode == 2) {
            if (angle_quote != '\0') {
                if (line[i] == angle_quote) {
                    angle_quote = '\0';
                }
                i++;
                continue;
            }
            if (line[i] == '"' || line[i] == '\'') {
                angle_quote = line[i];
                i++;
                continue;
            }
            if (line[i] == '>') {
                angle_mode = 0;
            }
            i++;
            continue;
        }
        if (line[i] == '\\') {
            escape = 1;
            i++;
            continue;
        }
        if (line[i] == '`') {
            ticks = table_count_backtick_run(line + i, end - i);
            if (!table_has_closing_backtick_run(line + i + ticks, end - i - ticks, ticks)) {
                i += ticks;
                after_bracket = 0;
                continue;
            }
            i += ticks;
            while (i < end) {
                if (line[i] == '`') {
                    size_t close_ticks;

                    close_ticks = table_count_backtick_run(line + i, end - i);
                    if (close_ticks == ticks) {
                        i += close_ticks;
                        break;
                    }
                    i += close_ticks;
                    continue;
                }
                i++;
            }
            after_bracket = 0;
            continue;
        }
        if (line[i] == '[') {
            if (!table_has_closing_bracket(line + i + 1, end - i - 1)) {
                i++;
                continue;
            }
            bracket_depth++;
            i++;
            continue;
        }
        if (line[i] == ']') {
            if (bracket_depth > 0) {
                bracket_depth--;
                after_bracket = bracket_depth == 0;
            }
            i++;
            continue;
        }
        if (after_bracket) {
            after_bracket = 0;
            if (line[i] == '(' && table_has_closing_link_destination(line + i + 1, end - i - 1)) {
                paren_depth = 1;
                i++;
                continue;
            }
        }
        if (line[i] == '(' && bracket_depth == 0) {
            paren_depth++;
            i++;
            continue;
        }
        if (line[i] == ')' && paren_depth > 0) {
            paren_depth--;
            i++;
            continue;
        }
        if (line[i] == '<') {
            int angle_end;

            angle_end = table_angle_span_end(line + i, end - i);
            if (angle_end > 0) {
                angle_mode = table_protected_angle_mode(line + i, (size_t)angle_end + 1);
            } else {
                angle_mode = 0;
            }
            if (angle_mode != 0) {
                i++;
                after_bracket = 0;
                continue;
            }
        }
        if (line[i] == '|' && bracket_depth == 0 && paren_depth == 0) {
            return i;
        }
        after_bracket = 0;
        i++;
    }
    return end;
}

static size_t table_count_cells(const char *line, size_t len)
{
    size_t start;
    size_t end;
    size_t i;
    size_t count;

    if (!table_delimited_bounds(line, len, &start, &end)) {
        return 0;
    }
    count = 1;
    i = start;
    while (i < end) {
        i = table_next_pipe(line, i, end);
        if (i < end) {
            count++;
            i++;
        }
    }
    return count;
}

static int table_quote_content(const char *line, size_t len, const char **out, size_t *out_len)
{
    size_t s;
    size_t e;

    s = table_trim_left(line, 0, len);
    e = table_trim_right(line, s, len);
    if (s >= e || line[s] != '>') {
        return 0;
    }
    s++;
    if (s < e && line[s] == ' ') {
        s++;
    }
    *out = line + s;
    *out_len = e - s;
    return 1;
}

static void table_cell_bounds_clamped(const char *line, size_t len, size_t cell, int clamp_tail, size_t *out_start, size_t *out_end)
{
    size_t start;
    size_t end;
    size_t i;
    size_t c;
    size_t seg_start;

    *out_start = 0;
    *out_end = 0;
    if (!table_delimited_bounds(line, len, &start, &end)) {
        return;
    }
    c = 0;
    seg_start = start;
    i = start;
    while (i <= end) {
        size_t sep;

        sep = i < end ? table_next_pipe(line, i, end) : end;
        if (sep > end) {
            sep = end;
        }
        if (c == cell) {
            if (clamp_tail) {
                sep = end;
            }
            *out_start = table_trim_left(line, seg_start, sep);
            *out_end = table_trim_right(line, *out_start, sep);
            return;
        }
        if (sep == end) {
            return;
        }
        c++;
        seg_start = sep + 1;
        i = sep + 1;
    }
}

static void table_cell_bounds(const char *line, size_t len, size_t cell, size_t *out_start, size_t *out_end)
{
    table_cell_bounds_clamped(line, len, cell, 0, out_start, out_end);
}

static int table_parse_delimiter(table_filter *tf, const char *line, size_t len, size_t cols)
{
    size_t i;
    size_t parse_cols;
    size_t s;
    size_t e;
    int left;
    int right;

    if (cols == 0 || table_count_cells(line, len) != cols) {
        return 0;
    }
    parse_cols = cols > TABLE_MAX_COLUMNS ? TABLE_MAX_COLUMNS : cols;
    for (i = 0; i < parse_cols; i++) {
        table_cell_bounds(line, len, i, &s, &e);
        if (s >= e) {
            return 0;
        }
        left = line[s] == ':';
        right = e > s && line[e - 1] == ':';
        if (left) {
            s++;
        }
        if (right) {
            e--;
        }
        if (s >= e) {
            return 0;
        }
        while (s < e) {
            if (line[s] != '-') {
                return 0;
            }
            s++;
        }
        tf->align[i] = left && right ? 2 : right ? 1 : 0;
    }
    tf->cols = parse_cols;
    tf->truncated_columns = cols > TABLE_MAX_COLUMNS;
    return 1;
}

static size_t table_wire_overhead(size_t cols, mdf_table_wire_mode wire)
{
    if (cols == 0) {
        return 0;
    }
    if (wire == MDF_TABLE_WIRE_SPACE) {
        return (cols - 1) * 2;
    }
    return 3 * cols + 1;
}

static size_t table_total_width(const size_t *widths, size_t cols, mdf_table_wire_mode wire)
{
    size_t i;
    size_t total;

    total = table_wire_overhead(cols, wire);
    for (i = 0; i < cols; i++) {
        total += widths[i];
    }
    return total;
}

static void table_fit_widths(size_t *widths, const size_t *mins, size_t cols, int max_width, mdf_table_wire_mode wire)
{
    size_t i;
    size_t widest;
    size_t widest_width;

    if (max_width <= 0) {
        return;
    }
    while (table_total_width(widths, cols, wire) > (size_t)max_width) {
        widest = cols;
        widest_width = 0;
        for (i = 0; i < cols; i++) {
            if (widths[i] > mins[i] && widths[i] > widest_width) {
                widest = i;
                widest_width = widths[i];
            }
        }
        if (widest == cols) {
            break;
        }
        widths[widest]--;
    }
}

static int table_render_cell_split_cost(mdf_renderer *renderer, const char *line, size_t len, size_t cell, size_t width, int *out_cost);

static int table_column_split_cost(table_filter *tf, mdf_renderer *renderer, size_t col, size_t width)
{
    size_t i;
    int cost;
    int cell_cost;

    cost = 0;
    if (tf->has_header) {
        if (table_render_cell_split_cost(renderer, tf->header, tf->header_len, col, width, &cell_cost) != 0) {
            return -1;
        }
        cost += cell_cost;
    }
    for (i = 0; i < tf->rows_len; i++) {
        if (table_render_cell_split_cost(renderer, tf->rows[i], tf->row_lens[i], col, width, &cell_cost) != 0) {
            return -1;
        }
        cost += cell_cost;
    }
    return cost;
}

typedef struct table_mem_sink {
    mdf_allocator *allocator;
    char *buf;
    size_t len;
    size_t cap;
} table_mem_sink;

typedef struct table_text_piece {
    const char *text;
    size_t len;
    char *style;
    size_t style_len;
    size_t width;
    int space;
    int wrapped_split;
    int wrapped_word;
} table_text_piece;

typedef struct table_text_line {
    table_text_piece *pieces;
    size_t piece_count;
    size_t piece_cap;
    size_t width;
} table_text_line;

typedef struct table_rendered_cell {
    char *buf;
    size_t len;
    char *plain;
    size_t plain_len;
    size_t plain_cap;
    table_text_piece *pieces;
    size_t piece_count;
    size_t piece_cap;
    table_text_line *lines;
    size_t line_count;
    size_t line_cap;
    size_t *line_widths;
} table_rendered_cell;

static int table_buf_append(mdf_allocator *allocator, char **buf, size_t *len, size_t *cap, const char *src, size_t n);
static int table_ansi_seq_is_reset(const char *s, size_t len);

static char *table_strdup_span(mdf_allocator *allocator, const char *src, size_t len)
{
    char *out;

    out = (char *)mdf_alloc(allocator, len + 1);
    if (out == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src, len);
    }
    out[len] = '\0';
    return out;
}

static int table_piece_append(mdf_allocator *allocator, table_text_piece **pieces, size_t *len, size_t *cap, const table_text_piece *piece)
{
    table_text_piece *next;
    size_t new_cap;

    if (*len == *cap) {
        new_cap = *cap == 0 ? 8 : *cap * 2;
        next = (table_text_piece *)mdf_realloc_mem(allocator, *pieces, *cap * sizeof((*pieces)[0]), new_cap * sizeof((*pieces)[0]));
        if (next == NULL) {
            return -1;
        }
        *pieces = next;
        *cap = new_cap;
    }
    (*pieces)[*len] = *piece;
    (*len)++;
    return 0;
}

static int table_line_append_piece(mdf_allocator *allocator, table_text_line *line, const table_text_piece *piece)
{
    if (table_piece_append(allocator, &line->pieces, &line->piece_count, &line->piece_cap, piece) != 0) {
        return -1;
    }
    line->width += piece->width;
    return 0;
}

static int table_cell_append_line(mdf_allocator *allocator, table_rendered_cell *cell, table_text_line *line)
{
    table_text_line *next;
    size_t new_cap;

    if (cell->line_count == cell->line_cap) {
        new_cap = cell->line_cap == 0 ? 4 : cell->line_cap * 2;
        next = (table_text_line *)mdf_realloc_mem(allocator, cell->lines, cell->line_cap * sizeof(cell->lines[0]), new_cap * sizeof(cell->lines[0]));
        if (next == NULL) {
            return -1;
        }
        cell->lines = next;
        cell->line_cap = new_cap;
    }
    cell->lines[cell->line_count] = *line;
    cell->line_count++;
    line->pieces = NULL;
    line->piece_count = 0;
    line->piece_cap = 0;
    line->width = 0;
    return 0;
}

static void table_text_line_destroy(mdf_allocator *allocator, table_text_line *line)
{
    mdf_free_mem(allocator, line->pieces, line->piece_cap * sizeof(line->pieces[0]));
    memset(line, 0, sizeof(*line));
}

static void table_cell_pop_last_line(mdf_allocator *allocator, table_rendered_cell *cell, table_text_line *out)
{
    table_text_line *line;

    if (cell->line_count == 0) {
        return;
    }
    table_text_line_destroy(allocator, out);
    cell->line_count--;
    line = &cell->lines[cell->line_count];
    *out = *line;
    memset(line, 0, sizeof(*line));
}

static int table_is_space_codepoint(unsigned long cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
}

static int table_mem_write(void *userdata, const char *src, size_t len)
{
    table_mem_sink *m;
    char *next;
    size_t cap;

    m = (table_mem_sink *)userdata;
    if (len == 0) {
        return 0;
    }
    if (m->len + len + 1 < m->len) {
        return -1;
    }
    if (m->len + len + 1 > m->cap) {
        cap = m->cap == 0 ? 128 : m->cap;
        while (cap < m->len + len + 1) {
            cap *= 2;
        }
        next = (char *)mdf_realloc_mem(m->allocator, m->buf, m->cap, cap);
        if (next == NULL) {
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

static void table_mem_sink_init(table_mem_sink *mem, mdf_allocator *allocator)
{
    mem->allocator = allocator;
    mem->buf = NULL;
    mem->len = 0;
    mem->cap = 0;
}

static void table_mem_sink_release(table_mem_sink *mem)
{
    mdf_free_mem(mem->allocator, mem->buf, mem->cap);
    mem->buf = NULL;
    mem->len = 0;
    mem->cap = 0;
}

static int table_nbsp_entity_len(const char *s, size_t len)
{
    if (len >= 6 && memcmp(s, "&nbsp;", 6) == 0) {
        return 6;
    }
    if (len >= 6 && memcmp(s, "&#160;", 6) == 0) {
        return 6;
    }
    if (len >= 6 &&
        s[0] == '&' && s[1] == '#' && s[2] == 'x' &&
        (s[3] == 'a' || s[3] == 'A') &&
        s[4] == '0' && s[5] == ';') {
        return 6;
    }
    return 0;
}

static int table_cell_needs_autolink_close(const char *src, size_t len)
{
    size_t i;
    size_t ticks;
    int escape;
    int in_code;
    int link_text_depth;
    int link_url_depth;
    int link_url_quote;
    int pending_link_close;
    int in_autolink;

    escape = 0;
    in_code = 0;
    link_text_depth = 0;
    link_url_depth = 0;
    link_url_quote = 0;
    pending_link_close = 0;
    in_autolink = 0;
    i = 0;
    while (i < len) {
        if (escape) {
            escape = 0;
            i++;
            continue;
        }
        if (src[i] == '\\') {
            escape = 1;
            i++;
            continue;
        }
        if (in_code) {
            if (src[i] == '`') {
                ticks = table_count_backtick_run(src + i, len - i);
                if (ticks == 1) {
                    in_code = 0;
                }
                i += ticks;
                continue;
            }
            i++;
            continue;
        }
        if (src[i] == '`') {
            ticks = table_count_backtick_run(src + i, len - i);
            if (ticks == 1) {
                in_code = 1;
            }
            i += ticks;
            continue;
        }
        if (in_autolink) {
            if (src[i] == '>') {
                in_autolink = 0;
            } else if (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') {
                in_autolink = 0;
            }
            i++;
            continue;
        }
        if (link_url_depth > 0) {
            if (link_url_quote != 0) {
                if (src[i] == link_url_quote) {
                    link_url_quote = 0;
                }
                i++;
                continue;
            }
            if (src[i] == '"' || src[i] == '\'') {
                link_url_quote = (unsigned char)src[i];
                i++;
                continue;
            }
            if (src[i] == '(') {
                link_url_depth++;
            } else if (src[i] == ')') {
                link_url_depth--;
            }
            i++;
            continue;
        }
        if (link_text_depth > 0) {
            if (src[i] == '[') {
                link_text_depth++;
            } else if (src[i] == ']') {
                link_text_depth--;
                if (link_text_depth == 0) {
                    pending_link_close = 1;
                }
            }
            i++;
            continue;
        }
        if (pending_link_close) {
            pending_link_close = 0;
            if (src[i] == '(') {
                link_url_depth = 1;
                i++;
                continue;
            }
        }
        if (src[i] == '[') {
            link_text_depth = 1;
            i++;
            continue;
        }
        if (src[i] == '<') {
            in_autolink = 1;
            i++;
            continue;
        }
        i++;
    }
    return in_autolink;
}

static int table_copy_cell_markdown(mdf_allocator *allocator, const char *src, size_t len, char **out, size_t *out_len)
{
    char *buf;
    size_t cap;
    size_t dst_len;
    size_t i;
    int ent_len;
    const char nbsp[] = "\302\240";

    buf = NULL;
    cap = 0;
    dst_len = 0;
    for (i = 0; i < len; ) {
        ent_len = table_nbsp_entity_len(src + i, len - i);
        if (ent_len > 0) {
            if (table_buf_append(allocator, &buf, &dst_len, &cap, nbsp, 2) != 0) {
                mdf_free_mem(allocator, buf, cap);
                return -1;
            }
            i += (size_t)ent_len;
            continue;
        }
        if (table_buf_append(allocator, &buf, &dst_len, &cap, src + i, 1) != 0) {
            mdf_free_mem(allocator, buf, cap);
            return -1;
        }
        i++;
    }
    if (buf == NULL) {
        buf = (char *)mdf_alloc(allocator, 1);
        if (buf == NULL) {
            return -1;
        }
        buf[0] = '\0';
        cap = 1;
    }
    if (table_cell_needs_autolink_close(buf, dst_len)) {
        if (table_buf_append(allocator, &buf, &dst_len, &cap, ">", 1) != 0) {
            mdf_free_mem(allocator, buf, cap);
            return -1;
        }
    }
    *out = buf;
    *out_len = dst_len;
    return 0;
}

static int table_render_cell_markdown_to_ansi(mdf_impl *impl,
                                              const char *src,
                                              size_t len,
                                              int osc8,
                                              table_mem_sink *mem)
{
    mdf_options opts;
    mdf_renderer *tmp;
    mdf_sink sink;
    mdf_token tok;
    mdf_status st;
    char *cell_src;
    size_t cell_src_len;

    opts = impl->opts;
    opts.width = MDF_TABLE_CELL_RENDER_WIDTH;
    opts.osc8 = osc8;
    memset(&opts.emission_buffer, 0, sizeof(opts.emission_buffer));
    opts.write_trace.userdata = NULL;
    opts.write_trace.emit = NULL;
    tmp = NULL;
    cell_src = NULL;
    cell_src_len = 0;
    table_mem_sink_init(mem, &impl->allocator);
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &tmp);
    if (st != MDF_OK || tmp == NULL) {
        return -1;
    }
    ((mdf_impl *)tmp->impl)->table_cell_mode = 1;
    if (table_copy_cell_markdown(&impl->allocator, src, len, &cell_src, &cell_src_len) != 0) {
        tmp->destroy(tmp);
        return -1;
    }
    sink.userdata = mem;
    sink.write = table_mem_write;
    memset(&tok, 0, sizeof(tok));
    tok.type = MDF_TOKEN_TEXT;
    tok.text = cell_src;
    tok.len = cell_src_len;
    st = tmp->write_token(tmp, &tok, &sink);
    if (st == MDF_OK) {
        tok.type = MDF_TOKEN_DOCUMENT_END;
        tok.text = NULL;
        tok.len = 0;
        st = tmp->write_token(tmp, &tok, &sink);
    }
    if (st == MDF_OK) {
        st = tmp->finish(tmp, &sink);
    }
    tmp->destroy(tmp);
    mdf_free_mem(&impl->allocator, cell_src, cell_src_len + 1);
    if (st != MDF_OK) {
        table_mem_sink_release(mem);
        return -1;
    }
    while (mem->len > 0 && mem->buf[mem->len - 1] == '\n') {
        mem->len--;
        mem->buf[mem->len] = '\0';
    }
    return 0;
}

static int table_parse_text_pieces(mdf_allocator *allocator, table_rendered_cell *cell, const char *src, size_t len, const char *style, size_t style_len)
{
    size_t i;
    size_t part_start;
    size_t part_width;
    int in_space;

    if (len == 0) {
        return 0;
    }
    i = 0;
    part_start = 0;
    part_width = 0;
    in_space = -1;
    while (i < len) {
        size_t adv;
        unsigned long cp;
        int space;

        if ((unsigned char)src[i] == 0x1b) {
            if (i + 1 < len && src[i + 1] == '[') {
                i += 2;
                while (i < len && (src[i] < '@' || src[i] > '~')) {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                continue;
            }
            if (i + 1 < len && src[i + 1] == ']') {
                i += 2;
                while (i < len) {
                    if (src[i] == '\a') {
                        i++;
                        break;
                    }
                    if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }
        adv = utf8_decode_codepoint(src + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        space = table_is_space_codepoint(cp) && cp != 0x00a0;
        if (in_space < 0) {
            in_space = space;
            part_start = i;
            part_width = 0;
        } else if (space != in_space) {
            table_text_piece piece;

            piece.text = src + part_start;
            piece.len = i - part_start;
            piece.style = table_strdup_span(allocator, style, style_len);
            if (piece.style == NULL) {
                return -1;
            }
            piece.style_len = style_len;
            piece.width = part_width;
            piece.space = in_space;
            piece.wrapped_split = 0;
            piece.wrapped_word = 0;
            if (table_piece_append(allocator, &cell->pieces, &cell->piece_count, &cell->piece_cap, &piece) != 0) {
                mdf_free_mem(allocator, piece.style, style_len + 1);
                return -1;
            }
            if (table_buf_append(allocator, &cell->plain, &cell->plain_len, &cell->plain_cap, piece.text, piece.len) != 0) {
                return -1;
            }
            in_space = space;
            part_start = i;
            part_width = 0;
        }
        part_width += utf8_display_width(cp);
        i += adv;
    }
    if (i > part_start || (len == 0 && in_space >= 0)) {
        table_text_piece piece;

        piece.text = src + part_start;
        piece.len = i - part_start;
        piece.style = table_strdup_span(allocator, style, style_len);
        if (piece.style == NULL) {
            return -1;
        }
        piece.style_len = style_len;
        piece.width = part_width;
        piece.space = in_space > 0;
        piece.wrapped_split = 0;
        piece.wrapped_word = 0;
        if (table_piece_append(allocator, &cell->pieces, &cell->piece_count, &cell->piece_cap, &piece) != 0) {
            mdf_free_mem(allocator, piece.style, style_len + 1);
            return -1;
        }
        if (table_buf_append(allocator, &cell->plain, &cell->plain_len, &cell->plain_cap, piece.text, piece.len) != 0) {
            return -1;
        }
    }
    return 0;
}

static size_t table_osc_seq_len(const char *s, size_t len)
{
    size_t i;

    if (len < 2 || (unsigned char)s[0] != 0x1b || s[1] != ']') {
        return 0;
    }
    i = 2;
    while (i < len) {
        if (s[i] == '\a') {
            return i + 1;
        }
        if ((unsigned char)s[i] == 0x1b && i + 1 < len && s[i + 1] == '\\') {
            return i + 2;
        }
        i++;
    }
    return 0;
}

static int table_osc_seq_is_close(const char *s, size_t len)
{
    return len >= 7 &&
           (unsigned char)s[0] == 0x1b &&
           s[1] == ']' &&
           s[2] == '8' &&
           s[3] == ';' &&
           s[4] == ';' &&
           ((s[5] == '\a' && len == 6) ||
            ((unsigned char)s[5] == 0x1b && s[6] == '\\'));
}

static int table_parse_styled_cell(mdf_allocator *allocator, table_rendered_cell *cell)
{
    char *style;
    size_t style_len;
    size_t style_cap;
    size_t i;
    size_t text_start;

    style = NULL;
    style_len = 0;
    style_cap = 0;
    i = 0;
    text_start = 0;
    while (i < cell->len) {
        if ((unsigned char)cell->buf[i] == 0x1b && i + 1 < cell->len && cell->buf[i + 1] == '[') {
            size_t seq_end;
            size_t seq_len;

            if (i > text_start) {
                if (table_parse_text_pieces(allocator, cell, cell->buf + text_start, i - text_start, style != NULL ? style : "", style_len) != 0) {
                    mdf_free_mem(allocator, style, style_cap);
                    return -1;
                }
            }
            seq_end = i + 2;
            while (seq_end < cell->len && (cell->buf[seq_end] < '@' || cell->buf[seq_end] > '~')) {
                seq_end++;
            }
            if (seq_end >= cell->len) {
                break;
            }
            seq_len = seq_end - i + 1;
            if (table_ansi_seq_is_reset(cell->buf + i, seq_len)) {
                style_len = 0;
                if (style != NULL) {
                    style[0] = '\0';
                }
            } else if (table_buf_append(allocator, &style, &style_len, &style_cap, cell->buf + i, seq_len) != 0) {
                mdf_free_mem(allocator, style, style_cap);
                return -1;
            }
            i = seq_end + 1;
            text_start = i;
            continue;
        }
        if ((unsigned char)cell->buf[i] == 0x1b && i + 1 < cell->len && cell->buf[i + 1] == ']') {
            size_t seq_len;

            seq_len = table_osc_seq_len(cell->buf + i, cell->len - i);
            if (seq_len == 0) {
                break;
            }
            if (table_osc_seq_is_close(cell->buf + i, seq_len)) {
                if (i > text_start) {
                    if (table_parse_text_pieces(allocator,
                                                cell,
                                                cell->buf + text_start,
                                                i - text_start,
                                                style != NULL ? style : "",
                                                style_len) != 0) {
                        mdf_free_mem(allocator, style, style_cap);
                        return -1;
                    }
                }
            } else {
                if (i > text_start) {
                    if (table_parse_text_pieces(allocator, cell, cell->buf + text_start, i - text_start, style != NULL ? style : "", style_len) != 0) {
                        mdf_free_mem(allocator, style, style_cap);
                        return -1;
                    }
                }
                if (table_buf_append(allocator, &style, &style_len, &style_cap, cell->buf + i, seq_len) != 0) {
                    mdf_free_mem(allocator, style, style_cap);
                    return -1;
                }
            }
            i += seq_len;
            text_start = i;
            continue;
        }
        i++;
    }
    if (i > text_start) {
        if (table_parse_text_pieces(allocator, cell, cell->buf + text_start, i - text_start, style != NULL ? style : "", style_len) != 0) {
            mdf_free_mem(allocator, style, style_cap);
            return -1;
        }
    }
    mdf_free_mem(allocator, style, style_cap);
    return 0;
}

static int table_wrap_piece_word(mdf_allocator *allocator, table_rendered_cell *cell, table_text_piece *pieces, size_t piece_count, size_t width)
{
    table_text_line current;
    size_t i;
    int rc;

    memset(&current, 0, sizeof(current));
    rc = -1;
    for (i = 0; i < piece_count; i++) {
        size_t off;
        size_t run_start;
        size_t run_width;
        table_text_piece *piece;

        piece = &pieces[i];
        off = 0;
        run_start = 0;
        run_width = 0;
        while (off < piece->len) {
            size_t adv;
            unsigned long cp;
            size_t w;

            adv = utf8_decode_codepoint(piece->text + off, piece->len - off, &cp);
            if (adv == 0) {
                break;
            }
            w = utf8_display_width(cp);
            if (width > 0 &&
                current.width + run_width + w > width &&
                (current.width > 0 || run_width > 0)) {
                table_text_piece chunk;

                if (off > run_start) {
                    chunk = *piece;
                    chunk.text = piece->text + run_start;
                    chunk.len = off - run_start;
                    chunk.width = run_width;
                    chunk.wrapped_split = chunk.len != piece->len;
                    chunk.wrapped_word = 1;
                    if (table_line_append_piece(allocator, &current, &chunk) != 0) {
                        goto done;
                    }
                }
                if (table_cell_append_line(allocator, cell, &current) != 0) {
                    goto done;
                }
                run_start = off;
                run_width = 0;
            }
            run_width += w;
            off += adv;
        }
        if (off > run_start) {
            table_text_piece chunk;

            chunk = *piece;
            chunk.text = piece->text + run_start;
            chunk.len = off - run_start;
            chunk.width = run_width;
            chunk.wrapped_split = chunk.len != piece->len;
            chunk.wrapped_word = 1;
            if (table_line_append_piece(allocator, &current, &chunk) != 0) {
                goto done;
            }
        }
    }
    if (current.piece_count > 0 || cell->line_count == 0) {
        if (table_cell_append_line(allocator, cell, &current) != 0) {
            goto done;
        }
    }
    rc = 0;

done:
    table_text_line_destroy(allocator, &current);
    return rc;
}

static int table_wrap_styled_cell(mdf_allocator *allocator, table_rendered_cell *cell, size_t width)
{
    table_text_piece *separator;
    table_text_piece *word;
    size_t sep_count;
    size_t sep_cap;
    size_t word_count;
    size_t word_cap;
    size_t sep_width;
    size_t word_width;
    table_text_line current;
    size_t i;
    int saw_word;
    int rc;

    separator = NULL;
    word = NULL;
    sep_count = 0;
    sep_cap = 0;
    word_count = 0;
    word_cap = 0;
    sep_width = 0;
    word_width = 0;
    saw_word = 0;
    rc = -1;
    memset(&current, 0, sizeof(current));
    for (i = 0; i < cell->piece_count; i++) {
        table_text_piece *piece;

        piece = &cell->pieces[i];
        if (piece->space) {
            if (word_count > 0) {
                size_t preserve_separator;
                size_t j;

                preserve_separator = !saw_word && sep_count > 0;
                if (width == 0) {
                    if ((current.width > 0 || preserve_separator) && sep_count > 0) {
                        for (j = 0; j < sep_count; j++) {
                            if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
                        }
                    }
                    for (j = 0; j < word_count; j++) {
                        if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
                    }
                } else if (current.width > 0 && current.width + sep_width + word_width <= width) {
                    for (j = 0; j < sep_count; j++) {
                        if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
                    }
                    for (j = 0; j < word_count; j++) {
                        if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
                    }
                } else {
                    if (current.width > 0) {
                        if (table_cell_append_line(allocator, cell, &current) != 0) goto done;
                    }
                    if (preserve_separator) {
                        for (j = 0; j < sep_count; j++) {
                            if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
                        }
                    }
                    if (word_width <= width || width == 0) {
                        for (j = 0; j < word_count; j++) {
                            if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
                        }
                    } else {
                        if (current.width > 0) {
                            if (table_cell_append_line(allocator, cell, &current) != 0) goto done;
                        }
                        if (table_wrap_piece_word(allocator, cell, word, word_count, width) != 0) goto done;
                        table_cell_pop_last_line(allocator, cell, &current);
                    }
                }
                saw_word = 1;
                sep_count = 0;
                sep_width = 0;
                word_count = 0;
                word_width = 0;
            }
            if (table_piece_append(allocator, &separator, &sep_count, &sep_cap, piece) != 0) goto done;
            sep_width += piece->width;
            continue;
        }
        if (table_piece_append(allocator, &word, &word_count, &word_cap, piece) != 0) goto done;
        word_width += piece->width;
    }
    if (word_count > 0) {
        size_t preserve_separator;
        size_t j;

        preserve_separator = !saw_word && sep_count > 0;
        if (width == 0) {
            if ((current.width > 0 || preserve_separator) && sep_count > 0) {
                for (j = 0; j < sep_count; j++) {
                    if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
                }
            }
            for (j = 0; j < word_count; j++) {
                if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
            }
        } else if (current.width > 0 && current.width + sep_width + word_width <= width) {
            for (j = 0; j < sep_count; j++) {
                if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
            }
            for (j = 0; j < word_count; j++) {
                if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
            }
        } else {
            if (current.width > 0) {
                if (table_cell_append_line(allocator, cell, &current) != 0) goto done;
            }
            if (preserve_separator) {
                for (j = 0; j < sep_count; j++) {
                    if (table_line_append_piece(allocator, &current, &separator[j]) != 0) goto done;
                }
            }
            if (word_width <= width || width == 0) {
                for (j = 0; j < word_count; j++) {
                    if (table_line_append_piece(allocator, &current, &word[j]) != 0) goto done;
                }
            } else {
                if (current.width > 0) {
                    if (table_cell_append_line(allocator, cell, &current) != 0) goto done;
                }
                if (table_wrap_piece_word(allocator, cell, word, word_count, width) != 0) goto done;
                table_cell_pop_last_line(allocator, cell, &current);
            }
        }
        saw_word = 1;
    }
    if (current.piece_count > 0 || (cell->line_count == 0 && !saw_word)) {
        if (table_cell_append_line(allocator, cell, &current) != 0) goto done;
    }
    rc = 0;

done:
    table_text_line_destroy(allocator, &current);
    mdf_free_mem(allocator, separator, sep_cap * sizeof(separator[0]));
    mdf_free_mem(allocator, word, word_cap * sizeof(word[0]));
    return rc;
}

static void table_rendered_cell_destroy(mdf_allocator *allocator, table_rendered_cell *cell)
{
    size_t i;

    mdf_free_mem(allocator, cell->buf, cell->len + 1);
    mdf_free_mem(allocator, cell->plain, cell->plain_len + 1);
    for (i = 0; i < cell->piece_count; i++) {
        mdf_free_mem(allocator, cell->pieces[i].style, cell->pieces[i].style_len + 1);
    }
    mdf_free_mem(allocator, cell->pieces, cell->piece_cap * sizeof(cell->pieces[0]));
    for (i = 0; i < cell->line_count; i++) {
        mdf_free_mem(allocator, cell->lines[i].pieces, cell->lines[i].piece_cap * sizeof(cell->lines[i].pieces[0]));
    }
    mdf_free_mem(allocator, cell->lines, cell->line_cap * sizeof(cell->lines[0]));
    mdf_free_mem(allocator, cell->line_widths, cell->line_count * sizeof(cell->line_widths[0]));
    memset(cell, 0, sizeof(*cell));
}

static int table_ansi_seq_is_reset(const char *s, size_t len)
{
    return len == 4 && s[0] == '\033' && s[1] == '[' && s[2] == '0' && s[3] == 'm';
}

static int table_buf_append(mdf_allocator *allocator, char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    char *next;
    size_t new_cap;
    size_t need;

    if (n > ((size_t)-1) - *len - 1) {
        return -1;
    }
    need = *len + n + 1;
    if (need > *cap) {
        new_cap = *cap == 0 ? 128 : *cap;
        while (new_cap < need) {
            new_cap *= 2;
        }
        next = (char *)mdf_realloc_mem(allocator, *buf, *cap, new_cap);
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

static int table_normalize_ansi_lines(mdf_allocator *allocator, char **buf, size_t *len)
{
    char *out;
    size_t out_len;
    size_t out_cap;
    char *active;
    size_t active_len;
    size_t active_cap;
    size_t i;

    out = NULL;
    out_len = 0;
    out_cap = 0;
    active = NULL;
    active_len = 0;
    active_cap = 0;
    i = 0;
    while (i < *len) {
        if ((unsigned char)(*buf)[i] == 0x1b && i + 1 < *len && (*buf)[i + 1] == '[') {
            size_t seq_end;
            size_t seq_len;

            seq_end = i + 2;
            while (seq_end < *len && ((*buf)[seq_end] < '@' || (*buf)[seq_end] > '~')) {
                seq_end++;
            }
            if (seq_end >= *len) {
                break;
            }
            seq_len = seq_end - i + 1;
            if (table_buf_append(allocator, &out, &out_len, &out_cap, *buf + i, seq_len) != 0) {
                goto nomem;
            }
            if ((*buf)[seq_end] == 'm') {
                if (table_ansi_seq_is_reset(*buf + i, seq_len)) {
                    active_len = 0;
                    if (active != NULL) {
                        active[0] = '\0';
                    }
                } else if (table_buf_append(allocator, &active, &active_len, &active_cap, *buf + i, seq_len) != 0) {
                    goto nomem;
                }
            }
            i = seq_end + 1;
            continue;
        }
        if ((*buf)[i] == '\n') {
            if (active_len > 0 && table_buf_append(allocator, &out, &out_len, &out_cap, "\033[0m", 4) != 0) {
                goto nomem;
            }
            if (table_buf_append(allocator, &out, &out_len, &out_cap, *buf + i, 1) != 0) {
                goto nomem;
            }
            if (active_len > 0 && table_buf_append(allocator, &out, &out_len, &out_cap, active, active_len) != 0) {
                goto nomem;
            }
            i++;
            continue;
        }
        if (table_buf_append(allocator, &out, &out_len, &out_cap, *buf + i, 1) != 0) {
            goto nomem;
        }
        i++;
    }
    if (out == NULL) {
        out = (char *)mdf_alloc(allocator, 1);
        if (out == NULL) {
            goto nomem;
        }
        out[0] = '\0';
        out_len = 0;
        out_cap = 1;
    }
    mdf_free_mem(allocator, *buf, *len + 1);
    *buf = out;
    *len = out_len;
    mdf_free_mem(allocator, active, active_cap);
    return 0;

nomem:
    mdf_free_mem(allocator, out, out_cap);
    mdf_free_mem(allocator, active, active_cap);
    return -1;
}

static size_t table_visible_word_min_width_ansi(const char *s, size_t len)
{
    size_t i;
    size_t word;
    size_t max_word;
    size_t adv;
    unsigned long cp;

    max_word = 1;
    word = 0;
    i = 0;
    while (i < len) {
        if ((unsigned char)s[i] == 0x1b) {
            if (i + 1 < len && s[i + 1] == '[') {
                i += 2;
                while (i < len && (s[i] < '@' || s[i] > '~')) {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                continue;
            }
            if (i + 1 < len && s[i + 1] == ']') {
                i += 2;
                while (i < len) {
                    if (s[i] == '\a') {
                        i++;
                        break;
                    }
                    if ((unsigned char)s[i] == 0x1b && i + 1 < len && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') {
            if (word > max_word) {
                max_word = word;
            }
            word = 0;
            i++;
            continue;
        }
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        word += utf8_display_width(cp);
        i += adv;
    }
    if (word > max_word) {
        max_word = word;
    }
    return max_word;
}

static int table_piece_contains_nbsp(const table_text_piece *piece)
{
    size_t i;

    for (i = 0; i + 1 < piece->len; i++) {
        if ((unsigned char)piece->text[i] == 0xc2 &&
            (unsigned char)piece->text[i + 1] == 0xa0) {
            return 1;
        }
    }
    return 0;
}

static size_t table_piece_max_rune_width(const table_text_piece *piece)
{
    size_t off;
    size_t max_width;

    off = 0;
    max_width = 0;
    while (off < piece->len) {
        size_t adv;
        unsigned long cp;
        size_t w;

        adv = utf8_decode_codepoint(piece->text + off, piece->len - off, &cp);
        if (adv == 0) {
            break;
        }
        w = utf8_display_width(cp);
        if (w > max_width) {
            max_width = w;
        }
        off += adv;
    }
    return max_width;
}

static size_t table_rendered_cell_break_min_width(const table_rendered_cell *cell)
{
    size_t i;
    size_t width;
    size_t word_width;
    size_t word_rune_width;
    int word_has_nbsp;

    width = 0;
    word_width = 0;
    word_rune_width = 0;
    word_has_nbsp = 0;
    for (i = 0; i < cell->piece_count; i++) {
        const table_text_piece *piece;
        size_t piece_rune_width;

        piece = &cell->pieces[i];
        if (piece->space) {
            if (word_has_nbsp && word_width > width) {
                width = word_width;
            }
            if (word_rune_width > width) {
                width = word_rune_width;
            }
            word_width = 0;
            word_rune_width = 0;
            word_has_nbsp = 0;
            continue;
        }
        word_width += piece->width;
        if (table_piece_contains_nbsp(piece)) {
            word_has_nbsp = 1;
        }
        piece_rune_width = table_piece_max_rune_width(piece);
        if (piece_rune_width > word_rune_width) {
            word_rune_width = piece_rune_width;
        }
    }
    if (word_has_nbsp && word_width > width) {
        width = word_width;
    }
    if (word_rune_width > width) {
        width = word_rune_width;
    }
    return width;
}

static int table_collect_word_widths_ansi(mdf_allocator *allocator, const char *s, size_t len, size_t **out_widths, size_t *out_count, size_t *out_cap)
{
    size_t *widths;
    size_t count;
    size_t cap;
    size_t i;
    size_t word;
    size_t adv;
    unsigned long cp;

    widths = NULL;
    count = 0;
    cap = 0;
    word = 0;
    i = 0;
    while (i < len) {
        if ((unsigned char)s[i] == 0x1b) {
            if (i + 1 < len && s[i + 1] == '[') {
                i += 2;
                while (i < len && (s[i] < '@' || s[i] > '~')) {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                continue;
            }
            if (i + 1 < len && s[i + 1] == ']') {
                i += 2;
                while (i < len) {
                    if (s[i] == '\a') {
                        i++;
                        break;
                    }
                    if ((unsigned char)s[i] == 0x1b && i + 1 < len && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') {
            if (word > 0) {
                size_t *next;
                size_t new_cap;

                if (count == cap) {
                    new_cap = cap == 0 ? 8 : cap * 2;
                    next = (size_t *)mdf_realloc_mem(allocator, widths, cap * sizeof(widths[0]), new_cap * sizeof(widths[0]));
                    if (next == NULL) {
                        mdf_free_mem(allocator, widths, cap * sizeof(widths[0]));
                        return -1;
                    }
                    widths = next;
                    cap = new_cap;
                }
                widths[count++] = word;
                word = 0;
            }
            i++;
            continue;
        }
        adv = utf8_decode_codepoint(s + i, len - i, &cp);
        if (adv == 0) {
            break;
        }
        word += utf8_display_width(cp);
        i += adv;
    }
    if (word > 0) {
        size_t *next;
        size_t new_cap;

        if (count == cap) {
            new_cap = cap == 0 ? 8 : cap * 2;
            next = (size_t *)mdf_realloc_mem(allocator, widths, cap * sizeof(widths[0]), new_cap * sizeof(widths[0]));
            if (next == NULL) {
                mdf_free_mem(allocator, widths, cap * sizeof(widths[0]));
                return -1;
            }
            widths = next;
            cap = new_cap;
        }
        widths[count++] = word;
    }
    *out_widths = widths;
    *out_count = count;
    *out_cap = cap;
    return 0;
}

static int table_render_cell_ansi(mdf_renderer *renderer, const char *line, size_t len, size_t cell, size_t width, int clamp_tail, table_rendered_cell *out)
{
    mdf_impl *impl;
    table_mem_sink mem;
    size_t s;
    size_t e;
    size_t *widths;

    memset(out, 0, sizeof(*out));
    table_cell_bounds_clamped(line, len, cell, clamp_tail, &s, &e);
    if (e <= s) {
        out->buf = (char *)mdf_alloc(&((mdf_impl *)renderer->impl)->allocator, 1);
        if (out->buf == NULL) {
            return -1;
        }
        out->buf[0] = '\0';
        out->len = 0;
        out->plain = (char *)mdf_alloc(&((mdf_impl *)renderer->impl)->allocator, 1);
        if (out->plain == NULL) {
            mdf_free_mem(&((mdf_impl *)renderer->impl)->allocator, out->buf, 1);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->plain[0] = '\0';
        out->plain_len = 0;
        out->plain_cap = 1;
        out->line_count = 1;
        widths = (size_t *)mdf_alloc(&((mdf_impl *)renderer->impl)->allocator, sizeof(size_t));
        if (widths == NULL) {
            mdf_free_mem(&((mdf_impl *)renderer->impl)->allocator, out->buf, 1);
            mdf_free_mem(&((mdf_impl *)renderer->impl)->allocator, out->plain, 1);
            mdf_free_mem(&((mdf_impl *)renderer->impl)->allocator, widths, sizeof(size_t));
            memset(out, 0, sizeof(*out));
            return -1;
        }
        widths[0] = 0;
        out->line_widths = widths;
        out->lines = (table_text_line *)mdf_alloc(&((mdf_impl *)renderer->impl)->allocator, sizeof(table_text_line));
        if (out->lines == NULL) {
            table_rendered_cell_destroy(&((mdf_impl *)renderer->impl)->allocator, out);
            return -1;
        }
        memset(out->lines, 0, sizeof(table_text_line));
        out->line_cap = 1;
        return 0;
    }

    impl = (mdf_impl *)renderer->impl;
    if (table_render_cell_markdown_to_ansi(impl, line + s, e - s, impl->opts.osc8, &mem) != 0) {
        return -1;
    }
    if (table_normalize_ansi_lines(&impl->allocator, &mem.buf, &mem.len) != 0) {
        table_mem_sink_release(&mem);
        return -1;
    }
    out->buf = mem.buf;
    out->len = mem.len;
    if (table_parse_styled_cell(&impl->allocator, out) != 0) {
        table_rendered_cell_destroy(&impl->allocator, out);
        return -1;
    }
    if (table_wrap_styled_cell(&impl->allocator, out, width) != 0) {
        table_rendered_cell_destroy(&impl->allocator, out);
        return -1;
    }
    widths = (size_t *)mdf_alloc(&impl->allocator, out->line_count * sizeof(size_t));
    if (widths == NULL) {
        table_rendered_cell_destroy(&impl->allocator, out);
        return -1;
    }
    for (e = 0; e < out->line_count; e++) {
        widths[e] = out->lines[e].width;
    }
    out->line_widths = widths;
    return 0;
}

static void table_html_apply_quote_text(mdf_impl *impl, html_state *state)
{
    html_state quote;
    size_t i;

    if (impl->opts.boring) {
        if (impl->theme != NULL && strcmp(impl->theme->name, "default") == 0) {
            return;
        }
        html_state_reset(&quote);
        quote.fg = 0;
    } else {
        html_parse_style_prefix(&quote, mdf_theme_quote_text(impl));
    }
    for (i = 0; i < state->segment_count; i++) {
        html_segment *seg;

        seg = &state->segments[i];
        if (seg->fg == -1 &&
            !seg->bold &&
            !seg->italic &&
            !seg->underline &&
            seg->link_len == 0) {
            seg->fg = quote.fg;
            seg->bold = quote.bold;
            seg->italic = quote.italic;
            seg->underline = quote.underline;
        }
    }
}

static int table_render_cell_html(mdf_renderer *renderer, const char *line, size_t len, size_t cell, int header, int clamp_tail, int quote_prefix, mdf_sink *sink)
{
    mdf_impl *impl;
    table_mem_sink mem;
    size_t s;
    size_t e;
    html_state state;
    int save_boring;

    table_cell_bounds_clamped(line, len, cell, clamp_tail, &s, &e);
    if (e <= s) {
        if ((s != 0 || e != 0) && mdf_write_all(sink, "", 0) != 0) {
            return -1;
        }
        return 0;
    }
    impl = (mdf_impl *)renderer->impl;
    save_boring = impl->opts.boring;
    impl->opts.boring = 0;
    if (table_render_cell_markdown_to_ansi(impl, line + s, e - s, 1, &mem) != 0) {
        impl->opts.boring = save_boring;
        return -1;
    }
    impl->opts.boring = save_boring;
    html_state_reset(&state);
    if (html_parse_ansi_inline(impl, &state, mem.buf, mem.len) != 0) {
        html_state_release(impl, &state);
        table_mem_sink_release(&mem);
        return -1;
    }
    if (!header && quote_prefix) {
        table_html_apply_quote_text(impl, &state);
    }
    if (html_emit_inline_state(impl, &state, sink, header) != 0) {
        html_state_release(impl, &state);
        table_mem_sink_release(&mem);
        return -1;
    }
    html_state_release(impl, &state);
    table_mem_sink_release(&mem);
    return 0;
}

static int table_render_cell_metrics(mdf_renderer *renderer, const char *line, size_t len, size_t cell, size_t *natural, size_t *preferred_min, size_t *break_min)
{
    mdf_impl *impl;
    table_rendered_cell rendered;
    size_t i;
    size_t max_width;
    size_t max_word;
    size_t min_break;

    impl = (mdf_impl *)renderer->impl;
    if (table_render_cell_ansi(renderer, line, len, cell, 0, 0, &rendered) != 0) {
        return -1;
    }
    max_width = 1;
    for (i = 0; i < rendered.line_count; i++) {
        if (rendered.line_widths[i] > max_width) {
            max_width = rendered.line_widths[i];
        }
    }
    max_word = table_visible_word_min_width_ansi(rendered.plain, rendered.plain_len);
    min_break = table_rendered_cell_break_min_width(&rendered);
    table_rendered_cell_destroy(&impl->allocator, &rendered);
    *natural = max_width;
    *preferred_min = max_word > 0 ? max_word : 1;
    *break_min = min_break > 0 ? min_break : 1;
    return 0;
}

static int table_render_cell_split_cost(mdf_renderer *renderer, const char *line, size_t len, size_t cell, size_t width, int *out_cost)
{
    mdf_impl *impl;
    table_rendered_cell rendered;
    size_t *word_widths;
    size_t word_count;
    size_t word_cap;
    size_t i;
    int cost;

    impl = (mdf_impl *)renderer->impl;
    if (table_render_cell_ansi(renderer, line, len, cell, 0, 0, &rendered) != 0) {
        return -1;
    }
    word_widths = NULL;
    word_count = 0;
    word_cap = 0;
    if (table_collect_word_widths_ansi(&impl->allocator, rendered.plain, rendered.plain_len, &word_widths, &word_count, &word_cap) != 0) {
        table_rendered_cell_destroy(&impl->allocator, &rendered);
        return -1;
    }
    cost = 0;
    for (i = 0; i < word_count; i++) {
        if (word_widths[i] > width) {
            size_t overflow;

            overflow = word_widths[i] - width;
            cost += 1000 + (int)(overflow * overflow);
        }
    }
    mdf_free_mem(&impl->allocator, word_widths, word_cap * sizeof(word_widths[0]));
    table_rendered_cell_destroy(&impl->allocator, &rendered);
    *out_cost = cost;
    return 0;
}

static int table_allocate_constrained_widths(table_filter *tf, mdf_renderer *renderer, size_t *widths, const size_t *natural, const size_t *break_mins, size_t cols, int max_width)
{
    size_t i;
    size_t budget;
    size_t total;
    size_t remaining;
    size_t idx;
    int best_reduction;
    int best_current_cost;
    int current_cost;
    int next_cost;
    int reduction;
    size_t best_target;

    if (max_width <= 0 || cols == 0) {
        return 0;
    }
    for (i = 0; i < cols; i++) {
        widths[i] = break_mins[i] > 1 ? break_mins[i] : 1;
    }
    budget = (size_t)max_width > table_wire_overhead(cols, ((mdf_impl *)renderer->impl)->opts.table_wire_mode) ?
             (size_t)max_width - table_wire_overhead(cols, ((mdf_impl *)renderer->impl)->opts.table_wire_mode) :
             0;
    total = 0;
    for (i = 0; i < cols; i++) {
        total += widths[i];
    }
    if (budget <= total) {
        return 0;
    }
    remaining = budget - total;
    while (remaining > 0) {
        idx = cols;
        best_reduction = 0;
        best_current_cost = 0;
        for (i = 0; i < cols; i++) {
            if (widths[i] >= natural[i]) {
                continue;
            }
            current_cost = table_column_split_cost(tf, renderer, i, widths[i]);
            next_cost = table_column_split_cost(tf, renderer, i, widths[i] + 1);
            if (current_cost < 0 || next_cost < 0) {
                return -1;
            }
            reduction = current_cost - next_cost;
            if (reduction > 0 &&
                (idx == cols || reduction > best_reduction ||
                 (reduction == best_reduction && current_cost > best_current_cost))) {
                idx = i;
                best_reduction = reduction;
                best_current_cost = current_cost;
            }
        }
        if (idx == cols) {
            best_target = 0;
            for (i = 0; i < cols; i++) {
                if (widths[i] < natural[i] && natural[i] > best_target) {
                    idx = i;
                    best_target = natural[i];
                }
            }
        }
        if (idx == cols) {
            break;
        }
        widths[idx]++;
        remaining--;
    }
    return 0;
}

static int table_write_repeat(mdf_impl *impl, mdf_sink *sink, const char *s, size_t n)
{
    char buf[256];
    size_t slen;

    slen = strlen(s);
    if (slen == 0 || n == 0) {
        return 0;
    }
    while (n > 0) {
        size_t count;
        size_t i;
        size_t off;

        count = sizeof(buf) / slen;
        if (count > n) {
            count = n;
        }
        off = 0;
        for (i = 0; i < count; i++) {
            memcpy(buf + off, s, slen);
            off += slen;
        }
        if (mdf_emit_all(impl, sink, buf, off) != 0) {
            return -1;
        }
        n -= count;
    }
    return 0;
}

static int table_write_runes(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len)
{
    size_t i;

    i = 0;
    while (i < len) {
        size_t adv;
        unsigned long cp;

        adv = utf8_decode_codepoint(src + i, len - i, &cp);
        if (adv == 0) {
            adv = 1;
        }
        if (mdf_emit_all(impl, sink, src + i, adv) != 0) {
            return -1;
        }
        i += adv;
    }
    return 0;
}

typedef struct table_wire_chars {
    const char *top_left;
    const char *top_mid;
    const char *top_right;
    const char *mid_left;
    const char *mid_mid;
    const char *mid_right;
    const char *bottom_left;
    const char *bottom_mid;
    const char *bottom_right;
    const char *horizontal;
    const char *vertical;
} table_wire_chars;

static table_wire_chars table_wire_chars_for(mdf_table_wire_mode wire)
{
    table_wire_chars chars;

    if (wire == MDF_TABLE_WIRE_ASCII) {
        chars.top_left = "+";
        chars.top_mid = "+";
        chars.top_right = "+";
        chars.mid_left = "+";
        chars.mid_mid = "+";
        chars.mid_right = "+";
        chars.bottom_left = "+";
        chars.bottom_mid = "+";
        chars.bottom_right = "+";
        chars.horizontal = "-";
        chars.vertical = "|";
        return chars;
    }
    chars.top_left = "\342\224\214";
    chars.top_mid = "\342\224\254";
    chars.top_right = "\342\224\220";
    chars.mid_left = "\342\224\234";
    chars.mid_mid = "\342\224\274";
    chars.mid_right = "\342\224\244";
    chars.bottom_left = "\342\224\224";
    chars.bottom_mid = "\342\224\264";
    chars.bottom_right = "\342\224\230";
    chars.horizontal = "\342\224\200";
    chars.vertical = "\342\224\202";
    return chars;
}

static int table_render_prefix(mdf_impl *impl, mdf_sink *sink, size_t indent_prefix, size_t quote_content_indent, int quote_prefix)
{
    if (ansi_ensure_left_margin(impl, sink) != 0) {
        return -1;
    }
    if (indent_prefix > 0 && !quote_prefix) {
        if (table_write_repeat(impl, sink, " ", indent_prefix) != 0) {
            return -1;
        }
    }
    if (!quote_prefix) {
        return 0;
    }
    if (!impl->opts.boring && mdf_emit_cstr(impl, sink, mdf_theme_quote(impl)) != 0) return -1;
    if (mdf_emit_cstr(impl, sink, ">") != 0) return -1;
    if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
    if (quote_content_indent > 0) {
        if (table_write_repeat(impl, sink, " ", quote_content_indent) != 0) {
            return -1;
        }
    }
    return 0;
}

static int table_render_border(mdf_impl *impl, mdf_allocator *allocator, mdf_sink *sink, const size_t *widths, size_t cols, int kind, size_t indent_prefix, size_t quote_content_indent, int quote_prefix)
{
    size_t i;
    const char *left;
    const char *mid;
    const char *right;
    const char *horizontal;
    char *buf;
    size_t len;
    size_t cap;
    table_wire_chars chars;

    if (impl->opts.table_wire_mode == MDF_TABLE_WIRE_SPACE) {
        return 0;
    }
    chars = table_wire_chars_for(impl->opts.table_wire_mode);
    left = kind == 0 ? chars.top_left : kind == 1 ? chars.mid_left : chars.bottom_left;
    mid = kind == 0 ? chars.top_mid : kind == 1 ? chars.mid_mid : chars.bottom_mid;
    right = kind == 0 ? chars.top_right : kind == 1 ? chars.mid_right : chars.bottom_right;
    horizontal = chars.horizontal;
    if (table_render_prefix(impl, sink, indent_prefix, quote_content_indent, quote_prefix) != 0) return -1;
    if (!impl->opts.boring && mdf_emit_cstr(impl, sink, mdf_theme_table_wire(impl)) != 0) return -1;
    buf = NULL;
    len = 0;
    cap = 0;
    if (table_buf_append(allocator, &buf, &len, &cap, left, strlen(left)) != 0) {
        mdf_free_mem(allocator, buf, cap);
        return -1;
    }
    for (i = 0; i < cols; i++) {
        size_t j;

        for (j = 0; j < widths[i] + 2; j++) {
            if (table_buf_append(allocator, &buf, &len, &cap, horizontal, strlen(horizontal)) != 0) {
                mdf_free_mem(allocator, buf, cap);
                return -1;
            }
        }
        if (table_buf_append(allocator, &buf, &len, &cap, i + 1 == cols ? right : mid, strlen(i + 1 == cols ? right : mid)) != 0) {
            mdf_free_mem(allocator, buf, cap);
            return -1;
        }
    }
    if (mdf_emit_all(impl, sink, buf, len) != 0) {
        mdf_free_mem(allocator, buf, cap);
        return -1;
    }
    mdf_free_mem(allocator, buf, cap);
    if (!impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    if (mdf_emit_cstr(impl, sink, "\n") != 0) return -1;
    ansi_reset_line_output_state(impl);
    return 0;
}

static int table_style_switch(mdf_impl *impl, mdf_sink *sink, const char **current_style, size_t *current_len, const char *style, size_t style_len)
{
    const char *logical_style;
    size_t logical_style_len;
    size_t osc_prefix_len;
    size_t current_osc_len;

    if (*current_len == style_len &&
        ((*current_len == 0) || memcmp(*current_style, style, style_len) == 0)) {
        return 0;
    }
    logical_style = style;
    logical_style_len = style_len;
    if (impl->opts.boring) {
        style = "";
        style_len = 0;
        logical_style = "";
        logical_style_len = 0;
    }
    osc_prefix_len = table_osc_seq_len(style, style_len);
    current_osc_len = *current_len > 0 ? table_osc_seq_len(*current_style, *current_len) : 0;
    if (current_osc_len > 0) {
        if (mdf_emit_cstr(impl, sink, "\033]8;;\033\\") != 0) {
            return -1;
        }
    }
    if (*current_len > 0 && mdf_emit_cstr(impl, sink, "\033[0m") != 0) {
        return -1;
    }
    if (osc_prefix_len > 0) {
        if (mdf_emit_all(impl, sink, style, osc_prefix_len) != 0) {
            return -1;
        }
        style += osc_prefix_len;
        style_len -= osc_prefix_len;
    }
    if (style_len > 0 && mdf_emit_all(impl, sink, style, style_len) != 0) {
        return -1;
    }
    *current_style = logical_style;
    *current_len = logical_style_len;
    return 0;
}

static int table_piece_is_gray(const table_text_piece *piece)
{
    return piece != NULL &&
           piece->style_len == 5 &&
           memcmp(piece->style, "\033[90m", 5) == 0;
}

static int table_piece_is_code(const table_text_piece *piece)
{
    return piece != NULL &&
           piece->style_len == 5 &&
           memcmp(piece->style, "\033[35m", 5) == 0;
}

static int table_piece_uses_grouped_text_writes(const table_text_piece *piece)
{
    if (piece == NULL || piece->wrapped_split) {
        return 0;
    }
    if (table_piece_is_code(piece)) {
        return 1;
    }
    return table_piece_is_gray(piece) && !piece->wrapped_word;
}

static int table_piece_is_bracket_body(const table_text_line *line, size_t idx)
{
    const table_text_piece *piece;
    const table_text_piece *prev;
    const table_text_piece *next;

    if (line == NULL || idx >= line->piece_count || idx == 0 || idx + 1 >= line->piece_count) {
        return 0;
    }
    piece = &line->pieces[idx];
    prev = &line->pieces[idx - 1];
    next = &line->pieces[idx + 1];
    if (piece->space || piece->wrapped_split || piece->style_len != 0) {
        return 0;
    }
    if (prev->len != 1 || next->len != 1) {
        return 0;
    }
    return prev->text[0] == '[' && next->text[0] == ']';
}

static int table_piece_is_bracket_literal(const table_text_piece *piece)
{
    if (piece == NULL || piece->space || piece->wrapped_split || piece->style_len != 0 || piece->len < 3) {
        return 0;
    }
    return piece->text[0] == '[' && piece->text[piece->len - 1] == ']';
}

static int table_piece_has_style(const table_text_piece *piece, const char *style)
{
    size_t style_len;

    if (piece == NULL || style == NULL) {
        return 0;
    }
    style_len = strlen(style);
    return piece->style_len == style_len &&
           style_len > 0 &&
           memcmp(piece->style, style, style_len) == 0;
}

static int table_piece_is_link_fallback_delim(mdf_impl *impl, const table_rendered_cell *cell, size_t line_idx, size_t piece_idx)
{
    const table_text_line *line;
    const table_text_piece *piece;
    const char *url_style;
    size_t i;

    if (cell == NULL || line_idx >= cell->line_count) {
        return 0;
    }
    line = &cell->lines[line_idx];
    if (piece_idx >= line->piece_count) {
        return 0;
    }
    piece = &line->pieces[piece_idx];
    if (piece->space) {
        int prev_styled;

        prev_styled = 0;
        i = piece_idx;
        while (i > 0) {
            i--;
            if (line->pieces[i].space) {
                continue;
            }
            prev_styled = line->pieces[i].style_len > 0;
            break;
        }
        for (i = piece_idx + 1; i < line->piece_count; i++) {
            if (line->pieces[i].space) {
                continue;
            }
            if (prev_styled &&
                line->pieces[i].style_len == 0 &&
                line->pieces[i].len == 1 &&
                line->pieces[i].text[0] == '(') {
                return 1;
            }
            return table_piece_is_link_fallback_delim(impl, cell, line_idx, i);
        }
        return 0;
    }
    if (piece->space || piece->style_len != 0 || piece->len != 1 ||
        (piece->text[0] != '(' && piece->text[0] != ')')) {
        return 0;
    }
    url_style = mdf_theme_link_url(impl);
    if (piece->text[0] == '(') {
        for (i = piece_idx + 1; i < line->piece_count; i++) {
            if (line->pieces[i].space) {
                continue;
            }
            return table_piece_has_style(&line->pieces[i], url_style);
        }
        for (i = line_idx + 1; i < cell->line_count; i++) {
            size_t j;

            for (j = 0; j < cell->lines[i].piece_count; j++) {
                if (cell->lines[i].pieces[j].space) {
                    continue;
                }
                return table_piece_has_style(&cell->lines[i].pieces[j], url_style);
            }
        }
    } else {
        i = piece_idx;
        while (i > 0) {
            i--;
            if (line->pieces[i].space) {
                continue;
            }
            return table_piece_has_style(&line->pieces[i], url_style);
        }
        i = line_idx;
        while (i > 0) {
            size_t j;

            i--;
            j = cell->lines[i].piece_count;
            while (j > 0) {
                j--;
                if (cell->lines[i].pieces[j].space) {
                    continue;
                }
                return table_piece_has_style(&cell->lines[i].pieces[j], url_style);
            }
        }
    }
    return 0;
}

static int table_render_row_line(mdf_impl *impl, mdf_sink *sink, table_rendered_cell *cells, const size_t *widths, const int *align, size_t cols, int header, size_t line_idx, size_t indent_prefix, size_t quote_content_indent, int quote_prefix)
{
    size_t i;
    size_t w;
    size_t pad;
    size_t left;
    size_t right;
    const char *current_style;
    size_t current_len;
    const char *wire_style;
    const char *header_style;
    const char *quote_text_style;
    size_t wire_style_len;
    size_t header_style_len;
    size_t quote_text_style_len;
    table_wire_chars chars;
    int space_wire;
    char combo_styles[2][128];
    int combo_style_idx;

    if (table_render_prefix(impl, sink, indent_prefix, quote_content_indent, quote_prefix) != 0) return -1;
    wire_style = mdf_theme_table_wire(impl);
    header_style = mdf_theme_table_header(impl);
    quote_text_style = (!impl->opts.boring && quote_prefix) ? mdf_theme_quote_text(impl) : "";
    wire_style_len = strlen(wire_style);
    header_style_len = strlen(header_style);
    quote_text_style_len = strlen(quote_text_style);
    chars = table_wire_chars_for(impl->opts.table_wire_mode);
    space_wire = impl->opts.table_wire_mode == MDF_TABLE_WIRE_SPACE;
    current_style = NULL;
    current_len = 0;
    combo_style_idx = 0;
    for (i = 0; i < cols; i++) {
        table_text_line *line;
        size_t j;

        line = NULL;
        w = 0;
        if (line_idx < cells[i].line_count) {
            line = &cells[i].lines[line_idx];
            w = cells[i].line_widths[line_idx];
        }
        pad = widths[i] > w ? widths[i] - w : 0;
        left = 0;
        right = pad;
        if (align[i] == 1) {
            left = pad;
            right = 0;
        } else if (align[i] == 2) {
            left = pad / 2;
            right = pad - left;
        }
        if (space_wire) {
            if (i > 0) {
                if (table_style_switch(impl, sink, &current_style, &current_len, wire_style, wire_style_len) != 0) return -1;
                if (mdf_emit_cstr(impl, sink, "  ") != 0) return -1;
            }
        } else {
            if (table_style_switch(impl, sink, &current_style, &current_len, wire_style, wire_style_len) != 0) return -1;
            if (i == 0) {
                if (mdf_emit_cstr(impl, sink, chars.vertical) != 0) return -1;
                if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            } else {
                if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
                if (mdf_emit_cstr(impl, sink, chars.vertical) != 0) return -1;
                if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            }
        }
        if (header) {
            if (table_style_switch(impl, sink, &current_style, &current_len, header_style, header_style_len) != 0) return -1;
        } else if (!(line != NULL && left == 0 && line->piece_count > 0 &&
                     (table_piece_is_gray(&line->pieces[0]) ||
                      table_osc_seq_len(line->pieces[0].style, line->pieces[0].style_len) > 0))) {
            if (table_style_switch(impl, sink, &current_style, &current_len, "", 0) != 0) return -1;
        }
        if (table_write_repeat(impl, sink, " ", left) != 0) return -1;
        if (line != NULL) {
            for (j = 0; j < line->piece_count; j++) {
                const char *style;
                size_t style_len;

                style = line->pieces[j].style;
                style_len = line->pieces[j].style_len;
                if (!header && quote_text_style_len > 0 &&
                    table_piece_is_link_fallback_delim(impl, &cells[i], line_idx, j)) {
                    style = "";
                    style_len = 0;
                } else if (header && style_len > 0) {
                    if (header_style_len + style_len >= sizeof(combo_styles[0])) {
                        return -1;
                    }
                    combo_style_idx = 1 - combo_style_idx;
                    memcpy(combo_styles[combo_style_idx], header_style, header_style_len);
                    memcpy(combo_styles[combo_style_idx] + header_style_len, style, style_len);
                    style = combo_styles[combo_style_idx];
                    style_len += header_style_len;
                } else if (header && style_len == 0) {
                    style = header_style;
                    style_len = header_style_len;
                } else if (!header && style_len == 0 && quote_text_style_len > 0) {
                    style = quote_text_style;
                    style_len = quote_text_style_len;
                }
                if (table_style_switch(impl, sink, &current_style, &current_len, style, style_len) != 0) return -1;
                if (table_piece_uses_grouped_text_writes(&line->pieces[j]) ||
                    table_piece_is_bracket_body(line, j)) {
                    if (mdf_emit_all(impl, sink, line->pieces[j].text, line->pieces[j].len) != 0) return -1;
                } else if (table_piece_is_bracket_literal(&line->pieces[j])) {
                    if (mdf_emit_all(impl, sink, line->pieces[j].text, 1) != 0) return -1;
                    if (mdf_emit_all(impl, sink, line->pieces[j].text + 1, line->pieces[j].len - 2) != 0) return -1;
                    if (mdf_emit_all(impl, sink, line->pieces[j].text + line->pieces[j].len - 1, 1) != 0) return -1;
                } else {
                    if (table_write_runes(impl, sink, line->pieces[j].text, line->pieces[j].len) != 0) return -1;
                }
            }
        }
        if (right > 0) {
            if (header) {
                if (table_style_switch(impl, sink, &current_style, &current_len, header_style, header_style_len) != 0) return -1;
            } else {
                if (table_style_switch(impl, sink, &current_style, &current_len, "", 0) != 0) return -1;
            }
        }
        if (table_write_repeat(impl, sink, " ", right) != 0) return -1;
    }
    if (!space_wire) {
        if (table_style_switch(impl, sink, &current_style, &current_len, wire_style, wire_style_len) != 0) return -1;
        if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
        if (mdf_emit_cstr(impl, sink, chars.vertical) != 0) return -1;
    }
    if (current_len > 0 && !impl->opts.boring && mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
    if (mdf_emit_cstr(impl, sink, "\n") != 0) return -1;
    ansi_reset_line_output_state(impl);
    return 0;
}

static int table_render_row(mdf_renderer *renderer, mdf_sink *sink, const char *line, size_t len, const size_t *widths, const int *align, size_t cols, int header, int clamp_tail, size_t indent_prefix, size_t quote_content_indent, int quote_prefix)
{
    mdf_impl *impl;
    table_rendered_cell cells[TABLE_MAX_COLUMNS];
    size_t i;
    size_t height;

    impl = (mdf_impl *)renderer->impl;
    memset(cells, 0, sizeof(cells));

    height = 1;
    for (i = 0; i < cols; i++) {
        if (table_render_cell_ansi(renderer, line, len, i, widths[i], clamp_tail && i + 1 == cols, &cells[i]) != 0) {
            size_t j;

            for (j = 0; j < i; j++) {
                table_rendered_cell_destroy(&impl->allocator, &cells[j]);
            }
            return -1;
        }
        if (cells[i].line_count > height) {
            height = cells[i].line_count;
        }
    }
    for (i = 0; i < height; i++) {
        if (table_render_row_line(impl, sink, cells, widths, align, cols, header, i, indent_prefix, quote_content_indent, quote_prefix) != 0) {
            size_t j;

            for (j = 0; j < cols; j++) {
                table_rendered_cell_destroy(&impl->allocator, &cells[j]);
            }
            return -1;
        }
    }
    for (i = 0; i < cols; i++) {
        table_rendered_cell_destroy(&impl->allocator, &cells[i]);
    }
    return 0;
}

static int table_available_width(mdf_impl *impl, table_filter *tf)
{
    int prefix_width;

    if (impl->opts.width <= 0) {
        return impl->opts.width;
    }
    prefix_width = impl->opts.margin_left;
    if (tf->quote_prefix) {
        prefix_width += 2;
        prefix_width += (int)tf->quote_content_indent;
    } else {
        prefix_width += (int)tf->indent_prefix;
    }
    if (prefix_width >= impl->opts.width) {
        return 1;
    }
    return impl->opts.width - prefix_width;
}

static mdf_status table_compute_ansi_widths(table_filter *tf, mdf_renderer *renderer, size_t *widths)
{
    mdf_impl *impl;
    size_t natural[TABLE_MAX_COLUMNS];
    size_t preferred_mins[TABLE_MAX_COLUMNS];
    size_t break_mins[TABLE_MAX_COLUMNS];
    size_t i;
    size_t c;
    size_t w;
    int avail_width;

    impl = (mdf_impl *)renderer->impl;
    memset(widths, 0, TABLE_MAX_COLUMNS * sizeof(widths[0]));
    memset(natural, 0, sizeof(natural));
    memset(preferred_mins, 0, sizeof(preferred_mins));
    memset(break_mins, 0, sizeof(break_mins));
    for (c = 0; c < tf->cols; c++) {
        size_t break_min;

        if (table_render_cell_metrics(renderer, tf->header, tf->header_len, c, &widths[c], &preferred_mins[c], &break_min) != 0) {
            return MDF_ERROR_NOMEM;
        }
        break_mins[c] = break_min;
        for (i = 0; i < tf->rows_len; i++) {
            size_t min_w;
            size_t row_break_min;

            if (table_render_cell_metrics(renderer, tf->rows[i], tf->row_lens[i], c, &w, &min_w, &row_break_min) != 0) {
                return MDF_ERROR_NOMEM;
            }
            if (w > widths[c]) {
                widths[c] = w;
            }
            if (min_w > preferred_mins[c]) {
                preferred_mins[c] = min_w;
            }
            if (row_break_min > break_mins[c]) {
                break_mins[c] = row_break_min;
            }
        }
        if (widths[c] == 0) {
            widths[c] = 1;
        }
        natural[c] = widths[c];
        if (preferred_mins[c] == 0) {
            preferred_mins[c] = 1;
        }
    }
    avail_width = table_available_width(impl, tf);
    table_fit_widths(widths, preferred_mins, tf->cols, avail_width, impl->opts.table_wire_mode);
    if (avail_width > 0 && table_total_width(widths, tf->cols, impl->opts.table_wire_mode) > (size_t)avail_width) {
        if (table_allocate_constrained_widths(tf, renderer, widths, natural, break_mins, tf->cols, avail_width) != 0) {
            return MDF_ERROR_NOMEM;
        }
    }
    return MDF_OK;
}

static void table_finish_ansi_state(mdf_impl *impl)
{
    impl->quote_open = 0;
    impl->quote_wrap_active = 0;
    impl->quote_prefix_indent = 0;
    ansi_reset_line_output_state(impl);
    impl->ansi_word_len = 0;
    impl->pending_breaks = 1;
}

static mdf_status table_render_ansi(table_filter *tf, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_impl *impl;
    size_t widths[TABLE_MAX_COLUMNS];
    size_t i;
    mdf_status st;

    impl = (mdf_impl *)renderer->impl;
    if (ansi_flush_pending_breaks(impl, sink) != 0) return MDF_ERROR_IO;
    if (ansi_flush_pending_space(impl, sink) != 0) return MDF_ERROR_IO;
    st = table_compute_ansi_widths(tf, renderer, widths);
    if (st != MDF_OK) return st;
    if (table_render_border(impl, &impl->allocator, sink, widths, tf->cols, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    if (tf->has_header) {
        if (table_render_row(renderer, sink, tf->header, tf->header_len, widths, tf->align, tf->cols, 1, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
        if (table_render_border(impl, &impl->allocator, sink, widths, tf->cols, 1, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    }
    for (i = 0; i < tf->rows_len; i++) {
        if (table_render_row(renderer, sink, tf->rows[i], tf->row_lens[i], widths, tf->align, tf->cols, 0, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    }
    if (table_render_border(impl, &impl->allocator, sink, widths, tf->cols, 2, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    table_finish_ansi_state(impl);
    return MDF_OK;
}

static mdf_status table_render_ansi_row_open(table_filter *tf, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_impl *impl;
    size_t i;
    mdf_status st;

    impl = (mdf_impl *)renderer->impl;
    if (ansi_flush_pending_breaks(impl, sink) != 0) return MDF_ERROR_IO;
    if (ansi_flush_pending_space(impl, sink) != 0) return MDF_ERROR_IO;
    st = table_compute_ansi_widths(tf, renderer, tf->row_widths);
    if (st != MDF_OK) return st;
    if (table_render_border(impl, &impl->allocator, sink, tf->row_widths, tf->cols, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    if (tf->has_header) {
        if (table_render_row(renderer, sink, tf->header, tf->header_len, tf->row_widths, tf->align, tf->cols, 1, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
        if (table_render_border(impl, &impl->allocator, sink, tf->row_widths, tf->cols, 1, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    }
    for (i = 0; i < tf->rows_len; i++) {
        if (table_render_row(renderer, sink, tf->rows[i], tf->row_lens[i], tf->row_widths, tf->align, tf->cols, 0, 0, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) return MDF_ERROR_IO;
    }
    table_filter_clear_rows(tf);
    tf->row_layout_ready = 1;
    return MDF_OK;
}

static mdf_status table_render_ansi_row_continue(table_filter *tf, mdf_renderer *renderer, mdf_sink *sink, const char *row, size_t row_len)
{
    if (table_render_row(renderer, sink, row, row_len, tf->row_widths, tf->align, tf->cols, 0, !tf->has_header && !tf->truncated_columns, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) {
        return MDF_ERROR_IO;
    }
    return MDF_OK;
}

static mdf_status table_render_ansi_row_finish(table_filter *tf, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_impl *impl;

    impl = (mdf_impl *)renderer->impl;
    if (!tf->row_layout_ready) {
        return table_render_ansi(tf, renderer, sink);
    }
    if (table_render_border(impl, &impl->allocator, sink, tf->row_widths, tf->cols, 2, tf->indent_prefix, tf->quote_content_indent, tf->quote_prefix) != 0) {
        return MDF_ERROR_IO;
    }
    table_finish_ansi_state(impl);
    return MDF_OK;
}

static int table_render_html_spaces(table_filter *tf, mdf_impl *impl, mdf_sink *sink, size_t count)
{
    html_buf_sink buf;
    mdf_sink buf_sink;
    size_t i;

    if (count == 0) {
        return 0;
    }
    memset(&buf, 0, sizeof(buf));
    buf.allocator = tf->allocator;
    buf_sink.userdata = &buf;
    buf_sink.write = html_buf_sink_write;
    if (mdf_write_cstr(&buf_sink, impl->opts.boring ? "<span style=\"color:rgb(0,0,0);\">" : "<span style=\"color:rgb(220,220,220);\">") != 0) {
        mdf_free_mem(tf->allocator, buf.buf, buf.cap);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (mdf_write_cstr(&buf_sink, " ") != 0) {
            mdf_free_mem(tf->allocator, buf.buf, buf.cap);
            return -1;
        }
    }
    if (mdf_write_cstr(&buf_sink, "</span>") != 0 ||
        mdf_write_all(sink, buf.buf, buf.len) != 0) {
        mdf_free_mem(tf->allocator, buf.buf, buf.cap);
        return -1;
    }
    mdf_free_mem(tf->allocator, buf.buf, buf.cap);
    return 0;
}

static int table_render_html_prefix(table_filter *tf, mdf_impl *impl, mdf_sink *sink)
{
    if (tf->quote_prefix) {
        if (impl->opts.boring) {
            if (mdf_write_cstr(sink, "<span style=\"color:rgb(0,0,0);\">&gt;</span>") != 0) return -1;
            if (mdf_write_cstr(sink, "<span style=\"color:rgb(0,0,0);\"> </span>") != 0) return -1;
        } else {
            char rgb_buf[32];
            const char *quote_rgb;

            quote_rgb = html_theme_quote_rgb(impl, rgb_buf, sizeof(rgb_buf));
            if (mdf_write_cstr(sink, "<span style=\"color:rgb(") != 0 ||
                mdf_write_cstr(sink, quote_rgb) != 0 ||
                mdf_write_cstr(sink, ");\">&gt;</span>") != 0) return -1;
            if (mdf_write_cstr(sink, "<span style=\"color:rgb(220,220,220);\"> </span>") != 0) return -1;
        }
        if (table_render_html_spaces(tf, impl, sink, tf->quote_content_indent) != 0) return -1;
    } else if (tf->indent_prefix > 0) {
        if (table_render_html_spaces(tf, impl, sink, tf->indent_prefix) != 0) return -1;
    }
    if (mdf_write_cstr(sink, "</span>") != 0) return -1;
    return 0;
}

static const char *table_html_align_style(int align)
{
    if (align == 1) {
        return "text-align:right;";
    }
    if (align == 2) {
        return "text-align:center;";
    }
    return "text-align:left;";
}

static int table_html_write_grouped(mdf_allocator *allocator, mdf_sink *sink, const char *a, const char *b, const char *c, const char *d, const char *e)
{
    html_buf_sink buf;
    mdf_sink buf_sink;

    memset(&buf, 0, sizeof(buf));
    buf.allocator = allocator;
    buf_sink.userdata = &buf;
    buf_sink.write = html_buf_sink_write;
    if ((a != NULL && mdf_write_cstr(&buf_sink, a) != 0) ||
        (b != NULL && mdf_write_cstr(&buf_sink, b) != 0) ||
        (c != NULL && mdf_write_cstr(&buf_sink, c) != 0) ||
        (d != NULL && mdf_write_cstr(&buf_sink, d) != 0) ||
        (e != NULL && mdf_write_cstr(&buf_sink, e) != 0) ||
        mdf_write_all(sink, buf.buf, buf.len) != 0) {
        mdf_free_mem(allocator, buf.buf, buf.cap);
        return -1;
    }
    mdf_free_mem(allocator, buf.buf, buf.cap);
    return 0;
}

static int table_render_html_row(table_filter *tf, mdf_renderer *renderer, mdf_sink *sink, const char *line, size_t len, int header)
{
    mdf_impl *impl;
    size_t i;
    const char *tag;

    impl = (mdf_impl *)renderer->impl;
    tag = header ? "th" : "td";
    if (mdf_write_cstr(sink, "<tr>") != 0) return -1;
    for (i = 0; i < tf->cols; i++) {
        if (table_html_write_grouped(&impl->allocator,
                                     sink,
                                     "<",
                                     tag,
                                     " style=\"",
                                     table_html_align_style(tf->align[i]),
                                     "\">") != 0) return -1;
        if (table_render_cell_html(renderer,
                                   line,
                                   len,
                                   i,
                                   header,
                                   !tf->has_header &&
                                   !tf->truncated_columns &&
                                   impl->opts.table_buffer_mode == MDF_TABLE_BUFFER_ROW &&
                                   i + 1 == tf->cols,
                                   tf->quote_prefix,
                                   sink) != 0) return -1;
        if (table_html_write_grouped(&impl->allocator, sink, "</", tag, ">", NULL, NULL) != 0) return -1;
    }
    if (mdf_write_cstr(sink, "</tr>") != 0) return -1;
    return 0;
}

static mdf_status table_render_html(table_filter *tf, mdf_parser_impl *parser_impl, mdf_renderer *renderer, mdf_sink *sink, int followed_by_content)
{
    mdf_impl *impl;
    html_state *state;
    html_bridge_sink bridge;
    mdf_sink bridge_sink;
    int save_width;
    int save_osc8;
    int save_boring;
    size_t i;

    impl = (mdf_impl *)renderer->impl;
    if (html_start(renderer, sink) != 0) {
        return MDF_ERROR_IO;
    }
    bridge.renderer = renderer;
    bridge.sink = sink;
    bridge.boring = impl->opts.boring;
    bridge_sink.userdata = &bridge;
    bridge_sink.write = html_bridge_write;
    save_width = impl->opts.width;
    save_osc8 = impl->opts.osc8;
    save_boring = impl->opts.boring;
    impl->opts.width = 0;
    impl->opts.osc8 = 1;
    impl->opts.boring = 0;
    if (ansi_flush_pending_breaks(impl, &bridge_sink) != 0) {
        impl->opts.width = save_width;
        impl->opts.osc8 = save_osc8;
        impl->opts.boring = save_boring;
        return MDF_ERROR_IO;
    }
    if (impl->ansi_col > 0 || impl->ansi_pending_space || impl->ansi_word_len > 0) {
        if (ansi_write_newline(impl, &bridge_sink) != 0) {
            impl->opts.width = save_width;
            impl->opts.osc8 = save_osc8;
            impl->opts.boring = save_boring;
            return MDF_ERROR_IO;
        }
    }
    impl->opts.width = save_width;
    impl->opts.osc8 = save_osc8;
    impl->opts.boring = save_boring;
    state = html_state_get(impl);
    if (state == NULL) {
        return MDF_ERROR_NOMEM;
    }
    if (html_flush_pending_lines(renderer, sink, 0, '\0') != 0) {
        return MDF_ERROR_IO;
    }
    if (impl->html_footer_needs_newline) {
        if (mdf_write_cstr(sink, "\n") != 0) return MDF_ERROR_IO;
        impl->html_footer_needs_newline = 0;
    }
    if (tf->quote_prefix || tf->indent_prefix > 0) {
        if (table_html_write_grouped(&impl->allocator,
                                     sink,
                                     "<div class=\"mdf-table-block\">",
                                     "<span class=\"mdf-prefix\">",
                                     NULL,
                                     NULL,
                                     NULL) != 0) return MDF_ERROR_IO;
        if (table_render_html_prefix(tf, impl, sink) != 0) return MDF_ERROR_IO;
    }
    if (impl->opts.table_wire_mode == MDF_TABLE_WIRE_SPACE) {
        if (table_html_write_grouped(&impl->allocator,
                                     sink,
                                     "<table class=\"mdf-table ",
                                     "mdf-table-space\">",
                                     NULL,
                                     NULL,
                                     NULL) != 0) return MDF_ERROR_IO;
    } else {
        if (table_html_write_grouped(&impl->allocator,
                                     sink,
                                     "<table class=\"mdf-table ",
                                     "mdf-table-bordered\">",
                                     NULL,
                                     NULL,
                                     NULL) != 0) return MDF_ERROR_IO;
    }
    if (tf->has_header) {
        if (mdf_write_cstr(sink, "<thead>") != 0) return MDF_ERROR_IO;
        if (table_render_html_row(tf, renderer, sink, tf->header, tf->header_len, 1) != 0) return MDF_ERROR_IO;
        if (mdf_write_cstr(sink, "</thead>") != 0) return MDF_ERROR_IO;
    }
    if (mdf_write_cstr(sink, "<tbody>") != 0) return MDF_ERROR_IO;
    for (i = 0; i < tf->rows_len; i++) {
        if (table_render_html_row(tf, renderer, sink, tf->rows[i], tf->row_lens[i], 0) != 0) return MDF_ERROR_IO;
    }
    if (mdf_write_cstr(sink, "</tbody>") != 0) return MDF_ERROR_IO;
    if (tf->quote_prefix || tf->indent_prefix > 0) {
        if (mdf_write_cstr(sink, "</table>\n") != 0) return MDF_ERROR_IO;
        if (mdf_write_cstr(sink, "</div>") != 0) return MDF_ERROR_IO;
        if (!followed_by_content && mdf_write_cstr(sink, "\n") != 0) return MDF_ERROR_IO;
        if (tf->quote_prefix) {
            parser_impl->prev_quote_depth = 0;
            parser_impl->prev_quote_line_text = 0;
            parser_impl->pending_quoted_list_blank_depth = 0;
        }
    } else {
        if (mdf_write_cstr(sink, "</table>\n") != 0) return MDF_ERROR_IO;
        impl->html_footer_needs_newline = followed_by_content ? 0 : 1;
    }
    impl->quote_open = 0;
    impl->quote_wrap_active = 0;
    impl->quote_prefix_indent = 0;
    ansi_reset_line_output_state(impl);
    impl->ansi_word_len = 0;
    impl->pending_breaks = 0;
    return MDF_OK;
}

static mdf_status table_filter_replay_line(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, const char *line, size_t len, int newline)
{
    mdf_status st;
    char nl;

    if (len > 0) {
        st = feed_parse_bytes(ps, impl, renderer, sink, line, len);
        if (st != MDF_OK) return st;
    }
    if (newline) {
        nl = '\n';
        st = feed_parse_bytes(ps, impl, renderer, sink, &nl, 1);
        if (st != MDF_OK) return st;
    }
    return MDF_OK;
}

static mdf_status table_filter_flush_before_render(table_filter *tf, parse_state *ps, mdf_parser_impl *parser_impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    if (parser_impl->pending_list_blank_breaks > 0) {
        st = flush_pending_list_blank_breaks(parser_impl, renderer, sink);
        if (st != MDF_OK) return st;
    }
    if (parser_impl->pending_bare_quote_blank_depth > 0) {
        st = flush_pending_bare_quote_blank(ps, parser_impl, renderer, sink, tf->quote_prefix ? 1 : 0);
        if (st != MDF_OK) return st;
    }
    st = flush_pending_list_item_end(parser_impl, renderer, sink);
    if (st != MDF_OK) return st;
    return MDF_OK;
}

static mdf_status table_filter_render(table_filter *tf, parse_state *ps, mdf_parser_impl *parser_impl, mdf_renderer *renderer, mdf_sink *sink, int followed_by_content)
{
    mdf_impl *impl;
    mdf_status st;

    impl = (mdf_impl *)renderer->impl;
    st = table_filter_flush_before_render(tf, ps, parser_impl, renderer, sink);
    if (st != MDF_OK) return st;
    if (impl->format == MDF_FORMAT_ANSI) {
        if (tf->quote_prefix) {
            parser_impl->prev_quote_depth = 0;
            parser_impl->prev_quote_line_text = 0;
            parser_impl->pending_quoted_list_blank_depth = 0;
        }
        if (impl->opts.table_buffer_mode == MDF_TABLE_BUFFER_ROW) {
            st = table_render_ansi_row_finish(tf, renderer, sink);
        } else {
            st = table_render_ansi(tf, renderer, sink);
        }
        if (st == MDF_OK && followed_by_content) {
            impl->pending_breaks = 0;
        }
        return st;
    }
    if (impl->format == MDF_FORMAT_HTML) {
        return table_render_html(tf, parser_impl, renderer, sink, followed_by_content);
    }
    return MDF_ERROR_PARSE;
}

static void table_filter_reset_candidate(table_filter *tf)
{
    mdf_free_mem(tf->allocator, tf->header, tf->header_len + 1);
    mdf_free_mem(tf->allocator, tf->delimiter, tf->delimiter_len + 1);
    tf->header = NULL;
    tf->header_len = 0;
    tf->delimiter = NULL;
    tf->delimiter_len = 0;
    table_filter_clear_rows(tf);
    tf->cols = 0;
    tf->truncated_columns = 0;
    tf->indent_prefix = 0;
    tf->quote_content_indent = 0;
    tf->quote_prefix = 0;
    tf->has_header = 0;
    tf->row_layout_ready = 0;
    memset(tf->row_widths, 0, sizeof(tf->row_widths));
    tf->state = 0;
    table_filter_reset_line(tf);
}

static size_t table_leading_space_prefix(const char *line, size_t len)
{
    size_t i;

    i = 0;
    while (i < len && line[i] == ' ') {
        i++;
    }
    return i;
}

static size_t table_skip_quote_prefixes(const char *line, size_t len)
{
    size_t i;

    i = 0;
    for (;;) {
        while (i < len && line[i] == ' ') {
            i++;
        }
        if (i >= len || line[i] != '>') {
            break;
        }
        i++;
        if (i < len && line[i] == ' ') {
            i++;
        }
    }
    return i;
}

static size_t chart_skip_container_prefixes(const char *line, size_t len)
{
    return table_skip_quote_prefixes(line, len);
}

static int chart_fence_content_start(mdf_parser_impl *impl, parse_state *ps, size_t *out_start)
{
    size_t i;
    size_t spaces;
    int quoted;

    i = 0;
    quoted = 0;
    for (;;) {
        spaces = 0;
        while (i < ps->prefix_len && ps->prefix[i] == ' ') {
            spaces++;
            i++;
        }
        if (i < ps->prefix_len && ps->prefix[i] == '>') {
            if (spaces >= 4) {
                return -1;
            }
            quoted = 1;
            i++;
            if (i < ps->prefix_len && ps->prefix[i] == ' ') {
                i++;
            }
            continue;
        }
        break;
    }
    if (quoted) {
        if (spaces >= 4) {
            return -1;
        }
    } else if (impl->list_visual_continuation_indent > 0 &&
               spaces >= impl->list_visual_continuation_indent) {
        if (spaces - impl->list_visual_continuation_indent >= 4) {
            return -1;
        }
    } else if (spaces >= 4) {
        return -1;
    }
    *out_start = i;
    return i < ps->prefix_len ? 1 : 0;
}

static int chart_prefix_is_closing_fence_line(mdf_parser_impl *impl, parse_state *ps)
{
    size_t start;
    size_t i;

    if (chart_fence_content_start(impl, ps, &start) <= 0) {
        return 0;
    }
    i = start;
    while (i < ps->prefix_len && ps->prefix[i] == '`') {
        i++;
    }
    if (i - start < 3) {
        return 0;
    }
    for (; i < ps->prefix_len; i++) {
        if (ps->prefix[i] != ' ' && ps->prefix[i] != '\t') {
            return 0;
        }
    }
    return 1;
}

static int chart_prefix_is_partial_fence(mdf_parser_impl *impl, parse_state *ps)
{
    size_t start;
    size_t content_len;

    switch (chart_fence_content_start(impl, ps, &start)) {
    case 0:
        return 1;
    case 1:
        break;
    default:
        return 0;
    }
    content_len = ps->prefix_len - start;
    return ps->prefix[start] == '`' && content_len < 3;
}

static int chart_fence_start_matches(mdf_parser_impl *impl, parse_state *ps)
{
    size_t start;
    size_t leading;
    size_t content_len;

    start = chart_skip_container_prefixes(ps->prefix, ps->prefix_len);
    if (start >= ps->prefix_len) {
        return 0;
    }
    leading = table_leading_space_prefix(ps->prefix, ps->prefix_len);
    if (start == leading &&
        leading >= 4 &&
        impl->list_visual_continuation_indent == 0 &&
        impl->prev_quote_depth == 0 &&
        ps->pending_quote_depth == 0 &&
        ps->quote_depth == 0) {
        return 0;
    }
    content_len = ps->prefix_len - start;
    if (content_len > 3) {
        content_len = 3;
    }
    return memcmp(ps->prefix + start, "```", content_len) == 0 &&
           ps->prefix_len - start >= 3;
}

static int chart_fence_start_is_partial(parse_state *ps)
{
    size_t start;
    size_t content_len;

    start = chart_skip_container_prefixes(ps->prefix, ps->prefix_len);
    if (start >= ps->prefix_len) {
        return 0;
    }
    content_len = ps->prefix_len - start;
    return content_len < 3 &&
           memcmp(ps->prefix + start, "```", content_len) == 0;
}

static size_t table_count_cells(const char *line, size_t len);

static int table_has_pipe_byte(const char *line, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (line[i] == '|') {
            return 1;
        }
    }
    return 0;
}

static int table_prefix_allows_candidate(mdf_parser_impl *impl, const char *line, size_t len)
{
    size_t base_indent;
    size_t after_quote;

    base_indent = table_leading_space_prefix(line, len);
    after_quote = table_skip_quote_prefixes(line, len);
    if (after_quote > base_indent) {
        return 1;
    }
    if (impl->list_visual_continuation_indent > 0) {
        return 1;
    }
    return base_indent < 4;
}

static int table_line_still_prefix_prelude(mdf_parser_impl *impl, const char *line, size_t len)
{
    size_t start;

    start = table_skip_quote_prefixes(line, len);
    if (!table_prefix_allows_candidate(impl, line, len)) {
        return 0;
    }
    while (start < len && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    return start >= len;
}

static int table_should_start_candidate(mdf_parser_impl *impl, const char *line, size_t len)
{
    size_t start;

    start = table_skip_quote_prefixes(line, len);
    if (start >= len) {
        return 0;
    }
    if (!table_prefix_allows_candidate(impl, line, len)) {
        return 0;
    }
    while (start < len && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    if (start >= len || line[start] != '|') {
        return 0;
    }
    return table_has_pipe_byte(line + start, len - start);
}

static int table_list_direct_ready(const char *line, size_t len)
{
    size_t i;
    size_t start;

    if (len < 2) {
        return 0;
    }
    start = table_skip_quote_prefixes(line, len);
    if (start + 1 >= len) {
        return 0;
    }
    if ((line[start] == '-' || line[start] == '*' || line[start] == '+') &&
        start + 1 < len && line[start + 1] == ' ') {
        if (start + 2 < len && line[start + 2] == '[') {
            if (start + 5 <= len &&
                (line[start + 3] == ' ' || line[start + 3] == 'x' || line[start + 3] == 'X') &&
                line[start + 4] == ']') {
                return 1;
            }
            return 0;
        }
        if (start + 5 <= len &&
            line[start + 2] == '[' &&
            (line[start + 3] == ' ' || line[start + 3] == 'x' || line[start + 3] == 'X') &&
            line[start + 4] == ']') {
            return 1;
        }
        return start + 2 < len;
    }
    i = start;
    while (i < len && line[i] >= '0' && line[i] <= '9') {
        i++;
    }
    if (i > start && i + 1 < len &&
        (line[i] == '.' || line[i] == ')') &&
        line[i + 1] == ' ') {
        return i + 2 < len;
    }
    return 0;
}

static mdf_status table_filter_complete_line(table_filter *tf, parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;
    mdf_impl *render_impl;
    size_t cols;
    const char *content;
    size_t content_len;

    render_impl = (mdf_impl *)renderer->impl;
    table_filter_apply_go_chunk_utf8_loss(tf);

    if (tf->state == 1) {
        content = tf->line;
        content_len = tf->line_len;
        tf->indent_prefix = 0;
        tf->quote_content_indent = 0;
        tf->quote_prefix = 0;
        if (impl->list_visual_continuation_indent > 0) {
            size_t indent_prefix;

            indent_prefix = table_leading_space_prefix(tf->line, tf->line_len);
            if (indent_prefix >= impl->list_visual_continuation_indent) {
                tf->indent_prefix = impl->list_visual_continuation_indent;
            }
        }
        if (table_quote_content(tf->line, tf->line_len, &content, &content_len)) {
            tf->quote_prefix = 1;
            tf->indent_prefix = 0;
            tf->quote_content_indent = table_leading_space_prefix(content, content_len);
        }
        if (table_count_cells(content, content_len) == 0) {
            st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 1);
            table_filter_reset_candidate(tf);
            tf->line_start = 1;
            return st;
        }
        if (table_filter_set_copy(tf, &tf->header, &tf->header_len, content, content_len) != 0) return MDF_ERROR_NOMEM;
        tf->state = 2;
        table_filter_reset_line(tf);
        return MDF_OK;
    }
    if (tf->state == 2) {
        content = tf->line;
        content_len = tf->line_len;
        if (tf->quote_prefix && !table_quote_content(tf->line, tf->line_len, &content, &content_len)) {
            content = tf->line;
            content_len = tf->line_len;
        }
        cols = table_count_cells(tf->header, tf->header_len);
        if (table_parse_delimiter(tf, content, content_len, cols)) {
            if (table_filter_set_copy(tf, &tf->delimiter, &tf->delimiter_len, content, content_len) != 0) return MDF_ERROR_NOMEM;
            tf->has_header = 1;
            tf->state = 3;
            table_filter_reset_line(tf);
            return MDF_OK;
        }
        if (table_count_cells(content, content_len) == cols) {
            size_t j;

            tf->has_header = 0;
            tf->cols = cols > TABLE_MAX_COLUMNS ? TABLE_MAX_COLUMNS : cols;
            tf->truncated_columns = cols > TABLE_MAX_COLUMNS;
            for (j = 0; j < tf->cols; j++) {
                tf->align[j] = 0;
            }
            if (table_filter_add_row(tf, tf->header, tf->header_len) != 0) return MDF_ERROR_NOMEM;
            if (table_filter_add_row(tf, content, content_len) != 0) return MDF_ERROR_NOMEM;
            tf->state = 3;
            if (render_impl->format == MDF_FORMAT_ANSI &&
                render_impl->opts.table_buffer_mode == MDF_TABLE_BUFFER_ROW) {
                st = table_filter_flush_before_render(tf, ps, impl, renderer, sink);
                if (st != MDF_OK) return st;
                st = table_render_ansi_row_open(tf, renderer, sink);
                if (st != MDF_OK) return st;
            }
            table_filter_reset_line(tf);
            return MDF_OK;
        }
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->header, tf->header_len, 1);
        if (st != MDF_OK) return st;
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 1);
        table_filter_reset_candidate(tf);
        tf->line_start = 1;
        return st;
    }
    if (tf->state == 3) {
        int terminating_blank;

        content = tf->line;
        content_len = tf->line_len;
        if (tf->quote_prefix && !table_quote_content(tf->line, tf->line_len, &content, &content_len)) {
            content = tf->line;
            content_len = tf->line_len;
        }
        if (table_count_cells(content, content_len) > 0) {
            if (!tf->has_header &&
                render_impl->opts.table_buffer_mode != MDF_TABLE_BUFFER_ROW) {
                size_t row_cols;
                size_t j;

                row_cols = table_count_cells(content, content_len);
                if (row_cols > tf->cols && tf->cols < TABLE_MAX_COLUMNS) {
                    if (row_cols > TABLE_MAX_COLUMNS) {
                        row_cols = TABLE_MAX_COLUMNS;
                        tf->truncated_columns = 1;
                    }
                    for (j = tf->cols; j < row_cols; j++) {
                        tf->align[j] = 0;
                    }
                    tf->cols = row_cols;
                } else if (row_cols > TABLE_MAX_COLUMNS) {
                    tf->truncated_columns = 1;
                }
            }
            if (render_impl->format == MDF_FORMAT_ANSI &&
                render_impl->opts.table_buffer_mode == MDF_TABLE_BUFFER_ROW &&
                tf->row_layout_ready) {
                st = table_render_ansi_row_continue(tf, renderer, sink, content, content_len);
                if (st != MDF_OK) return st;
            } else {
                if (table_filter_add_row(tf, content, content_len) != 0) return MDF_ERROR_NOMEM;
                if (render_impl->format == MDF_FORMAT_ANSI &&
                    render_impl->opts.table_buffer_mode == MDF_TABLE_BUFFER_ROW &&
                    tf->rows_len >= (tf->has_header ? 1u : 2u)) {
                    st = table_filter_flush_before_render(tf, ps, impl, renderer, sink);
                    if (st != MDF_OK) return st;
                    st = table_render_ansi_row_open(tf, renderer, sink);
                    if (st != MDF_OK) return st;
                }
            }
            table_filter_reset_line(tf);
            return MDF_OK;
        }
        terminating_blank = tf->line_len == 0;
        st = table_filter_render(tf, ps, impl, renderer, sink, terminating_blank ? 0 : 1);
        if (st != MDF_OK) return st;
        if (terminating_blank) {
            table_filter_reset_candidate(tf);
            tf->line_start = 1;
            return MDF_OK;
        }
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 1);
        table_filter_reset_candidate(tf);
        tf->line_start = 1;
        return st;
    }
    return MDF_OK;
}

static mdf_status table_filter_feed(table_filter *tf, parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, const char *buf, size_t n)
{
    size_t i;
    char ch;
    mdf_status st;

    for (i = 0; i < n; i++) {
        ch = buf[i];
        if (tf->state != 0) {
            if (i == 0 && table_filter_note_chunk_offset(tf) != 0) {
                return MDF_ERROR_NOMEM;
            }
            if (tf->state == TABLE_FILTER_STATE_PREFIX_PRELUDE) {
                if (ch == '\n') {
                    st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 1);
                    if (st != MDF_OK) return st;
                    tf->state = 0;
                    table_filter_reset_line(tf);
                    tf->line_start = 1;
                    continue;
                }
                if (table_filter_append_line(tf, &ch, 1) != 0) {
                    return MDF_ERROR_NOMEM;
                }
                if (table_should_start_candidate(impl, tf->line, tf->line_len)) {
                    tf->state = 1;
                    continue;
                }
                if (table_line_still_prefix_prelude(impl, tf->line, tf->line_len)) {
                    continue;
                }
                st = feed_parse_bytes(ps, impl, renderer, sink, tf->line, tf->line_len);
                if (st != MDF_OK) return st;
                table_filter_reset_line(tf);
                tf->state = 0;
                tf->line_start = 0;
                continue;
            }
            if (ch == '\n') {
                st = table_filter_complete_line(tf, ps, impl, renderer, sink);
                if (st != MDF_OK) return st;
            } else if (table_filter_append_line(tf, &ch, 1) != 0) {
                return MDF_ERROR_NOMEM;
            } else if (tf->state == 1 && table_list_direct_ready(tf->line, tf->line_len)) {
                st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 0);
                if (st != MDF_OK) return st;
                tf->state = 0;
                table_filter_reset_line(tf);
                tf->line_start = 0;
            }
            continue;
        }
        if (tf->line_start) {
            if (ch != '\n') {
                if (table_filter_append_line(tf, &ch, 1) != 0) return MDF_ERROR_NOMEM;
                if (table_should_start_candidate(impl, tf->line, tf->line_len)) {
                    tf->state = 1;
                    continue;
                }
                if (table_line_still_prefix_prelude(impl, tf->line, tf->line_len)) {
                    tf->state = TABLE_FILTER_STATE_PREFIX_PRELUDE;
                    continue;
                }
                st = feed_parse_bytes(ps, impl, renderer, sink, tf->line, tf->line_len);
                if (st != MDF_OK) return st;
                table_filter_reset_line(tf);
                tf->line_start = 0;
                continue;
            }
            if (tf->line_len > 0) {
                st = feed_parse_bytes(ps, impl, renderer, sink, tf->line, tf->line_len);
                if (st != MDF_OK) return st;
                table_filter_reset_line(tf);
            }
            tf->line_start = 0;
        }
        st = feed_parse_bytes(ps, impl, renderer, sink, &ch, 1);
        if (st != MDF_OK) return st;
        if (ch == '\n') {
            tf->line_start = 1;
        }
    }
    return MDF_OK;
}

static mdf_status table_filter_finish(table_filter *tf, parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    if (tf->state == TABLE_FILTER_STATE_PREFIX_PRELUDE) {
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 0);
        table_filter_reset_candidate(tf);
        return st;
    }
    if (tf->state == 1) {
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 0);
        table_filter_reset_candidate(tf);
        return st;
    }
    if (tf->state == 2) {
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->header, tf->header_len, 1);
        if (st != MDF_OK) return st;
        st = table_filter_replay_line(ps, impl, renderer, sink, tf->line, tf->line_len, 0);
        table_filter_reset_candidate(tf);
        return st;
    }
    if (tf->state == 3) {
        st = table_filter_render(tf, ps, impl, renderer, sink, 0);
        table_filter_reset_candidate(tf);
        return st;
    }
    if (tf->line_len > 0) {
        st = feed_parse_bytes(ps, impl, renderer, sink, tf->line, tf->line_len);
        table_filter_reset_line(tf);
        return st;
    }
    return MDF_OK;
}

static size_t trim_left_span(const char *src, size_t start, size_t end)
{
    while (start < end && (src[start] == ' ' || src[start] == '\t' || src[start] == '\r' || src[start] == '\n')) {
        start++;
    }
    return start;
}

static size_t trim_right_span(const char *src, size_t start, size_t end)
{
    (void)start;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '\r' || src[end - 1] == '\n')) {
        end--;
    }
    return end;
}

static int span_equal(const char *src, size_t start, size_t end, const char *want)
{
    size_t n;

    n = strlen(want);
    return end >= start && end - start == n && memcmp(src + start, want, n) == 0;
}

static int next_line_span(const char *src, size_t len, size_t start, int eof, size_t *line_start, size_t *line_end, size_t *next)
{
    size_t i;

    if (start > len) {
        return 0;
    }
    if (start == len) {
        if (!eof) {
            return 0;
        }
        *line_start = start;
        *line_end = start;
        *next = start;
        return 1;
    }
    for (i = start; i < len; i++) {
        if (src[i] == '\n') {
            *line_start = start;
            *line_end = i;
            if (*line_end > *line_start && src[*line_end - 1] == '\r') {
                (*line_end)--;
            }
            *next = i + 1;
            return 1;
        }
    }
    if (!eof) {
        return 0;
    }
    *line_start = start;
    *line_end = len;
    if (*line_end > *line_start && src[*line_end - 1] == '\r') {
        (*line_end)--;
    }
    *next = len;
    return 1;
}

static int parse_opening_frontmatter(const char *src, size_t start, size_t end, char *delim)
{
    if (end - start >= 3 && (unsigned char)src[start] == 0xef && (unsigned char)src[start + 1] == 0xbb && (unsigned char)src[start + 2] == 0xbf) {
        start += 3;
    }
    start = trim_left_span(src, start, end);
    end = trim_right_span(src, start, end);
    if (span_equal(src, start, end, "---")) {
        memcpy(delim, "---", 3);
        return 1;
    }
    if (span_equal(src, start, end, "+++")) {
        memcpy(delim, "+++", 3);
        return 1;
    }
    if (span_equal(src, start, end, ";;;")) {
        memcpy(delim, ";;;", 3);
        return 1;
    }
    return 0;
}

static int frontmatter_metadata_likely(const char *src, size_t start, size_t end)
{
    size_t i;

    start = trim_left_span(src, start, end);
    end = trim_right_span(src, start, end);
    if (start >= end) {
        return 0;
    }
    if (src[start] == '{' || src[start] == '[') {
        return 1;
    }
    for (i = start; i < end; i++) {
        if (src[i] == ':' || src[i] == '=') {
            return 1;
        }
    }
    return 0;
}

static int find_frontmatter_close(const char *src, size_t len, size_t start, const char *delim, int eof, size_t *close_next)
{
    size_t line_start;
    size_t line_end;
    size_t next;
    size_t trim_start;
    size_t trim_end;

    while (start <= len) {
        if (!next_line_span(src, len, start, eof, &line_start, &line_end, &next)) {
            return 0;
        }
        trim_start = trim_left_span(src, line_start, line_end);
        trim_end = trim_right_span(src, trim_start, line_end);
        if (span_equal(src, trim_start, trim_end, delim)) {
            *close_next = next;
            return 1;
        }
        if (next == start) {
            return 0;
        }
        start = next;
        if (start == len && !eof) {
            return 0;
        }
    }
    return 0;
}

static int frontmatter_decide(frontmatter_filter *fm, int eof, const char **out, size_t *out_len, int *decided)
{
    size_t open_start;
    size_t open_end;
    size_t open_next;
    size_t second_start;
    size_t second_end;
    size_t second_next;
    size_t close_next;
    char delim[4];

    *out = NULL;
    *out_len = 0;
    *decided = 0;
    if (!eof && !frontmatter_opening_prefix_possible(fm->probe, fm->len)) {
        *out = fm->probe;
        *out_len = fm->len;
        fm->passthrough = 1;
        *decided = 1;
        return 0;
    }
    if (!next_line_span(fm->probe, fm->len, 0, eof, &open_start, &open_end, &open_next)) {
        return 0;
    }
    if (!parse_opening_frontmatter(fm->probe, open_start, open_end, delim)) {
        *out = fm->probe;
        *out_len = fm->len;
        fm->passthrough = 1;
        *decided = 1;
        return 0;
    }
    delim[3] = '\0';
    if (!next_line_span(fm->probe, fm->len, open_next, eof, &second_start, &second_end, &second_next)) {
        return 0;
    }
    if (!frontmatter_metadata_likely(fm->probe, second_start, second_end)) {
        *out = fm->probe;
        *out_len = fm->len;
        fm->passthrough = 1;
        *decided = 1;
        return 0;
    }
    if (!find_frontmatter_close(fm->probe, fm->len, second_next, delim, eof, &close_next)) {
        if (eof) {
            *out = fm->probe;
            *out_len = fm->len;
            fm->passthrough = 1;
            *decided = 1;
        }
        return 0;
    }
    *out = fm->probe + close_next;
    *out_len = fm->len - close_next;
    fm->passthrough = 1;
    *decided = 1;
    return 0;
}

static mdf_status flush_prefix(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    if (ps->prefix_len == 0) {
        return MDF_OK;
    }
    if (ps->pending_soft_space) {
        st = render_emit(renderer, sink, MDF_TOKEN_SPACE, " ", 1, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->pending_soft_space = 0;
    }
    st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->prefix, ps->prefix_len, 0);
    ps->prefix_len = 0;
    return st;
}

static mdf_status flush_immediate_spaces(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink)
{
    size_t i;
    mdf_status st;

    for (i = 0; i < ps->immediate_spaces_len; i++) {
        st = render_emit(renderer, sink, MDF_TOKEN_SPACE, ps->immediate_spaces + i, 1, 0);
        if (st != MDF_OK) {
            return st;
        }
    }
    ps->immediate_spaces_len = 0;
    return MDF_OK;
}

static mdf_status prepare_fenced_block_boundary(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    if (impl->prev_quote_line_text && (ps->quote_depth > 0 || impl->prev_quote_depth > 0)) {
        st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        impl->prev_quote_line_text = 0;
    }
    if (ps->pending_soft_space) {
        st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->pending_soft_space = 0;
    }
    return MDF_OK;
}

static int prefix_all_blank(parse_state *ps)
{
    size_t i;

    for (i = 0; i < ps->prefix_len; i++) {
        if (ps->prefix[i] != ' ' && ps->prefix[i] != '\t') {
            return 0;
        }
    }
    return 1;
}

static size_t prefix_visual_cols(const char *s, size_t len)
{
    size_t i;
    size_t cols;

    cols = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '\t') {
            cols += 4 - (cols % 4);
        } else {
            cols++;
        }
    }
    return cols;
}

static mdf_status emit_space_prefix(mdf_renderer *renderer, mdf_sink *sink, size_t count)
{
    char spaces[32];
    size_t n;
    mdf_status st;

    while (count > 0) {
        n = count < sizeof(spaces) ? count : sizeof(spaces);
        memset(spaces, ' ', n);
        st = render_emit(renderer, sink, MDF_TOKEN_TEXT, spaces, n, 0);
        if (st != MDF_OK) {
            return st;
        }
        count -= n;
    }
    return MDF_OK;
}

static mdf_status emit_quote_prefix_depth_level(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink, int depth, int level);
static mdf_status emit_quote_suffixes(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink);

static mdf_status flush_pending_list_item_end(mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    if (!impl->pending_list_item_end) {
        return MDF_OK;
    }
    impl->pending_list_item_end = 0;
    return render_emit(renderer, sink, MDF_TOKEN_LIST_ITEM_END, NULL, 0, 0);
}

static mdf_status flush_pending_list_blank_breaks(mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    while (impl->pending_list_blank_breaks > 0) {
        st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        impl->pending_list_blank_breaks--;
    }
    return MDF_OK;
}

static mdf_status emit_pending_list_item_start(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    int quote_level;

    if (!ps->list_item_start_pending) {
        return MDF_OK;
    }
    ps->list_item_start_pending = 0;
    quote_level = ps->quote_depth > 0 ? ps->quote_depth : ps->pending_quote_depth;
    if (quote_level <= 0 && impl != NULL) {
        quote_level = impl->prev_quote_depth;
    }
    if (quote_level <= 0 && ps->list_marker_quote_context) {
        quote_level = 1;
    }
    ps->list_marker_quote_context = 0;
    return render_emit(renderer,
                sink,
                MDF_TOKEN_LIST_ITEM_START,
                ps->list_marker,
                ps->list_marker_len,
                quote_level);
}

static mdf_status flush_pending_quoted_list_blank(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, int keep_quote)
{
    mdf_status st;

    if (impl->pending_quoted_list_blank_depth <= 0) {
        return MDF_OK;
    }
    if (keep_quote) {
        st = emit_quote_prefix_depth_level(ps, renderer, sink, impl->pending_quoted_list_blank_depth, 1);
        impl->pending_quoted_list_blank_depth = 0;
        if (st != MDF_OK) {
            return st;
        }
        return emit_quote_suffixes(ps, impl, renderer, sink);
    }
    impl->pending_quoted_list_blank_depth = 0;
    impl->prev_quote_depth = 0;
    return render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
}

static mdf_status flush_pending_bare_quote_blank(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, int keep_quote)
{
    mdf_status st;

    if (impl->pending_bare_quote_blank_depth <= 0) {
        return MDF_OK;
    }
    if (keep_quote > 0) {
        st = emit_quote_prefix_depth_level(ps, renderer, sink, impl->pending_bare_quote_blank_depth, 1);
        impl->pending_bare_quote_blank_depth = 0;
        if (st != MDF_OK) {
            return st;
        }
        return emit_quote_suffixes(ps, impl, renderer, sink);
    }
    impl->pending_bare_quote_blank_depth = 0;
    impl->prev_quote_depth = 0;
    if (keep_quote == 0) {
        return render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
    }
    return MDF_OK;
}

static int prefix_is_bare_quote(parse_state *ps)
{
    size_t i;

    i = 0;
    while (i < ps->prefix_len && (ps->prefix[i] == ' ' || ps->prefix[i] == '\t')) {
        i++;
    }
    if (i + 1 != ps->prefix_len) {
        return 0;
    }
    return ps->prefix[i] == '>';
}

static mdf_status emit_quote_prefix_depth(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink, int depth)
{
    mdf_status st;

    while (ps->quote_depth < depth) {
        st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_START, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->quote_depth++;
    }
    return MDF_OK;
}

static mdf_status emit_quote_prefix_depth_level(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink, int depth, int level)
{
    mdf_status st;

    while (ps->quote_depth < depth) {
        st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_START, NULL, 0, level);
        if (st != MDF_OK) {
            return st;
        }
        ps->quote_depth++;
    }
    return MDF_OK;
}

static mdf_status flush_pending_quote_prefix(parse_state *ps, mdf_renderer *renderer, mdf_sink *sink, size_t trim)
{
    mdf_status st;

    if (ps->pending_quote_depth <= 0) {
        return MDF_OK;
    }
    st = emit_quote_prefix_depth(ps, renderer, sink, ps->pending_quote_depth);
    if (st != MDF_OK) {
        return st;
    }
    ps->pending_quote_depth = 0;
    if (trim > 0 && trim <= ps->prefix_len) {
        memmove(ps->prefix, ps->prefix + trim, ps->prefix_len - trim);
        ps->prefix_len -= trim;
    }
    return MDF_OK;
}

static mdf_status emit_quote_suffixes(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    mdf_status st;

    if (ps->quote_depth <= 0) {
        impl->prev_quote_depth = 0;
        return MDF_OK;
    }
    impl->prev_quote_depth = ps->quote_depth;
    while (ps->quote_depth > 0) {
        st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_END, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->quote_depth--;
    }
    return MDF_OK;
}

static mdf_status emit_chart_block(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, int kind)
{
    mdf_status st;

    st = render_emit(renderer, sink, MDF_TOKEN_CHART_BLOCK, impl->chart_buf, impl->chart_len, kind);
    impl->chart_len = 0;
    if (impl->chart_buf != NULL) {
        impl->chart_buf[0] = '\0';
    }
    if (st != MDF_OK) {
        return st;
    }
    while (impl->chart_quote_depth > 0) {
        st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_END, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        impl->chart_quote_depth--;
    }
    if (impl->chart_quote_depth == 0) {
        impl->prev_quote_depth = 0;
        if (ps != NULL) {
            ps->quote_depth = 0;
            ps->pending_quote_depth = 0;
        }
    }
    return MDF_OK;
}

static mdf_status end_line(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink)
{
    int had_pending_list_item_end;
    mdf_status st;

    if (ps->mode == 7) {
        int kind;

        kind = chart_info_kind(impl->fence_info, impl->fence_info_len);
        impl->pending_fence = 0;
        impl->fence_info_len = 0;
        impl->fence_info[0] = '\0';
        if (kind != 0) {
            impl->chart_quote_depth = ps->quote_depth;
            st = emit_chart_block(ps, impl, renderer, sink, kind);
        } else {
            impl->in_code_block = 1;
            st = render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_START, NULL, 0, 0);
        }
        goto reset_line;
    }
    if (impl->in_chart_block) {
        if (chart_prefix_is_closing_fence_line(impl, ps)) {
            int kind;

            kind = impl->chart_kind;
            impl->in_chart_block = 0;
            impl->chart_kind = 0;
            ps->prefix_len = 0;
            ps->decided = 1;
            ps->mode = 6;
            st = emit_chart_block(ps, impl, renderer, sink, kind);
            if (st != MDF_OK) {
                return st;
            }
            goto reset_line;
        }
        if (ps->prefix_len > 0) {
            size_t content_start;
            size_t content_len;

            content_start = chart_skip_container_prefixes(ps->prefix, ps->prefix_len);
            content_len = content_start < ps->prefix_len ? ps->prefix_len - content_start : 0;
            if (content_len > 0 && chart_append(impl, ps->prefix + content_start, content_len) != 0) {
                return MDF_ERROR_NOMEM;
            }
        }
        if (chart_append(impl, "\n", 1) != 0) {
            return MDF_ERROR_NOMEM;
        }
        st = MDF_OK;
        goto reset_line;
    }
    if (!ps->decided && prefix_is_bare_quote(ps)) {
        ps->immediate_spaces_len = 0;
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        ps->pending_soft_space = 0;
        ps->prefix_len = 0;
        impl->pending_bare_quote_blank_depth = 1;
        st = MDF_OK;
    } else if (!ps->decided && ps->thematic_possible && ps->thematic_count >= 3) {
        ps->immediate_spaces_len = 0;
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        ps->pending_soft_space = 0;
        ps->prefix_len = 0;
        st = render_emit(renderer, sink, MDF_TOKEN_THEMATIC_BREAK, NULL, 0, 0);
        impl->prev_quote_depth = 0;
        impl->ordered_active = 0;
    } else if (!ps->decided && prefix_all_blank(ps)) {
        ps->immediate_spaces_len = 0;
        if (impl->in_indent_code) {
            st = render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_END, NULL, 0, 0);
            if (st != MDF_OK) return st;
            impl->in_indent_code = 0;
            while (impl->prev_quote_depth > 0) {
                st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_END, NULL, 0, 0);
                if (st != MDF_OK) return st;
                impl->prev_quote_depth--;
            }
            ps->pending_soft_space = 0;
            ps->prefix_len = 0;
            impl->prev_quote_depth = 0;
            goto reset_line;
        }
        had_pending_list_item_end = impl->pending_list_item_end;
        if (had_pending_list_item_end) {
            if (ps->pending_quote_depth > 0) {
                st = flush_pending_list_item_end(impl, renderer, sink);
                if (st != MDF_OK) return st;
                impl->pending_quoted_list_blank_depth = ps->pending_quote_depth;
                ps->pending_quote_depth = 0;
                impl->list_blank_pending = 0;
                impl->ordered_active = 0;
                impl->list_continuation_indent = 0;
                impl->list_visual_continuation_indent = 0;
                impl->list_stack_len = 0;
            } else if (impl->prev_quote_depth > 0) {
                st = flush_pending_list_item_end(impl, renderer, sink);
                if (st != MDF_OK) return st;
                st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
                if (st != MDF_OK) return st;
                impl->prev_quote_depth = 0;
                impl->list_blank_pending = 0;
                impl->ordered_active = 0;
                impl->list_continuation_indent = 0;
                impl->list_visual_continuation_indent = 0;
                impl->list_stack_len = 0;
                ps->pending_soft_space = 0;
                ps->prefix_len = 0;
                goto reset_line;
            } else if (!impl->list_blank_pending) {
                impl->pending_list_blank_breaks = 2;
                impl->list_blank_pending = 1;
                ps->pending_soft_space = 0;
                ps->prefix_len = 0;
                st = MDF_OK;
                goto reset_line;
            } else {
                impl->list_blank_pending = 1;
                ps->pending_soft_space = 0;
                ps->prefix_len = 0;
                st = MDF_OK;
                goto reset_line;
            }
        } else {
            st = flush_pending_list_item_end(impl, renderer, sink);
            if (st != MDF_OK) return st;
            impl->list_continuation_indent = 0;
            impl->list_visual_continuation_indent = 0;
            impl->list_stack_len = 0;
        }
        ps->pending_soft_space = 0;
        ps->prefix_len = 0;
        if (impl->pending_quoted_list_blank_depth > 0) {
            st = MDF_OK;
        } else if (ps->pending_quote_depth > 0) {
            st = emit_quote_prefix_depth_level(ps, renderer, sink, ps->pending_quote_depth, 1);
            ps->pending_quote_depth = 0;
            if (st == MDF_OK) {
                st = emit_quote_suffixes(ps, impl, renderer, sink);
            }
        } else if (ps->quote_depth > 0) {
            st = emit_quote_suffixes(ps, impl, renderer, sink);
        } else if (impl->prev_hard_break) {
            st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
        } else {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            impl->prev_quote_depth = 0;
        }
    } else {
        ps->immediate_spaces_len = 0;
        st = flush_prefix(ps, renderer, sink);
        if (st != MDF_OK) return st;
        if (ps->mode == 6) {
            st = MDF_OK;
        } else if (impl->in_code_block || ps->mode == 3) {
            st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
        } else if (ps->mode == 1) {
            st = render_emit(renderer, sink, MDF_TOKEN_HEADING_END, NULL, 0, ps->heading_level);
        } else if (ps->mode == 2) {
            impl->prev_quote_depth = ps->quote_depth > 0 ? ps->quote_depth : 1;
            impl->prev_quote_line_text = 1;
            st = render_emit(renderer, sink, MDF_TOKEN_BLOCKQUOTE_END, NULL, 0, 0);
        } else if (ps->mode == 4) {
            impl->prev_quote_line_text = 0;
            impl->pending_list_item_end = 1;
            if (ps->hard_break) {
                impl->list_hard_break_pending = 1;
                st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
            } else {
                st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, -1);
            }
        } else {
            if (ps->quote_depth > 0) {
                if (!impl->pending_list_item_end && impl->list_continuation_indent == 0) {
                    impl->prev_quote_line_text = 1;
                }
                ps->pending_soft_space = 0;
                if (ps->hard_break) {
                    st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
                }
            } else if (ps->hard_break) {
                ps->pending_soft_space = 0;
                st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
            } else {
                ps->pending_soft_space = 1;
            }
            if (!ps->hard_break) {
                st = MDF_OK;
            }
        }
        if (st == MDF_OK) {
            if (ps->mode == 4 && ps->quote_depth > 0 && impl->ordered_active) {
                st = flush_pending_list_item_end(impl, renderer, sink);
                if (st != MDF_OK) {
                    return st;
                }
            }
            st = emit_quote_suffixes(ps, impl, renderer, sink);
        }
    }
reset_line:
    parse_reset_line(ps, impl);
    return st;
}

static int prefix_matches(parse_state *ps, const char *s)
{
    size_t n;

    n = strlen(s);
    return ps->prefix_len == n && memcmp(ps->prefix, s, n) == 0;
}

static mdf_status feed_decided(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, char c);

static int parse_ordered_marker(parse_state *ps, size_t start, size_t *after_marker, unsigned long *number, char *delimiter)
{
    size_t i;
    size_t digits;
    unsigned long value;

    i = start;
    digits = 0;
    value = 0;
    while (i < ps->prefix_len && ps->prefix[i] >= '0' && ps->prefix[i] <= '9') {
        if (digits + 2 >= sizeof(ps->list_marker)) {
            return 0;
        }
        value = value * 10 + (unsigned long)(ps->prefix[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= ps->prefix_len) {
        return 0;
    }
    if (ps->prefix[i] != '.' && ps->prefix[i] != ')') {
        return 0;
    }
    *delimiter = ps->prefix[i];
    i++;
    if (i >= ps->prefix_len) {
        return 0;
    }
    if (ps->prefix[i] != ' ') {
        return 0;
    }
    if (start + digits + 1 >= sizeof(ps->list_marker)) {
        return 0;
    }
    ps->list_marker_len = start + digits + 1;
    if (start > 0) {
        memcpy(ps->list_marker, ps->prefix, start);
    }
    memcpy(ps->list_marker + start, ps->prefix + start, digits + 1);
    *after_marker = i + 1;
    *number = value;
    return 1;
}

static size_t list_visual_indent_for_marker(mdf_parser_impl *impl, size_t source_indent)
{
    size_t n;
    size_t i;
    size_t parent_visual;
    size_t visual_indent;

    n = impl->list_stack_len;
    while (n > 0 && impl->list_source_indents[n - 1] > source_indent) {
        n--;
    }
    impl->list_stack_len = n;
    for (i = 0; i < n; i++) {
        if (impl->list_source_indents[i] == source_indent) {
            return impl->list_visual_indents[i];
        }
    }
    parent_visual = 0;
    for (i = 0; i < n; i++) {
        if (impl->list_source_indents[i] < source_indent) {
            parent_visual = impl->list_visual_indents[i];
        }
    }
    visual_indent = source_indent == 0 ? 0 : parent_visual + 2;
    if (n < sizeof(impl->list_source_indents) / sizeof(impl->list_source_indents[0])) {
        impl->list_source_indents[n] = source_indent;
        impl->list_visual_indents[n] = visual_indent;
        impl->list_stack_len = n + 1;
    }
    return visual_indent;
}

static int format_ordered_marker(parse_state *ps, size_t visual_indent, unsigned long number, char delimiter)
{
    char digits[32];
    int digits_len;
    size_t marker_len;

    digits_len = sprintf(digits, "%lu", number);
    if (digits_len <= 0) {
        return 0;
    }
    marker_len = visual_indent + (size_t)digits_len + 1;
    if (marker_len >= sizeof(ps->list_marker)) {
        return 0;
    }
    if (visual_indent > 0) {
        memset(ps->list_marker, ' ', visual_indent);
    }
    memcpy(ps->list_marker + visual_indent, digits, (size_t)digits_len);
    ps->list_marker[visual_indent + (size_t)digits_len] = delimiter;
    ps->list_marker_len = marker_len;
    return 1;
}

static mdf_status decide_prefix(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, char c)
{
    size_t i;
    size_t after_marker;
    size_t marker_indent;
    size_t marker_visual_indent;
    unsigned long ordered_number;
    unsigned long ordered_display;
    char ordered_delimiter;
    int continued_after_blank;
    int indented_list_marker;
    int quoted_list_boundary;
    int hashes;
    mdf_status st;

    if (c == '\r') {
        return MDF_OK;
    }
    if (c == '\n') {
        ps->hard_break = ps->trailing_spaces >= 2;
        if (ps->list_task_probe && ps->task_len > 0) {
            st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->task, ps->task_len, 0);
            if (st != MDF_OK) {
                return st;
            }
            ps->task_len = 0;
            ps->list_task_probe = 0;
        }
        return end_line(ps, impl, renderer, sink);
    }
    if (ps->prefix_len >= sizeof(ps->prefix)) {
        ps->decided = 1;
        st = flush_prefix(ps, renderer, sink);
        if (st != MDF_OK) {
            return st;
        }
        return feed_decided(ps, impl, renderer, sink, c);
    }
    ps->prefix[ps->prefix_len++] = c;
    if (impl->in_chart_block) {
        size_t content_start;
        size_t content_len;

        if (chart_prefix_is_closing_fence_line(impl, ps)) {
            return MDF_OK;
        }
        if (chart_prefix_is_partial_fence(impl, ps)) {
            return MDF_OK;
        }
        ps->decided = 1;
        ps->mode = 8;
        content_start = chart_skip_container_prefixes(ps->prefix, ps->prefix_len);
        content_len = content_start < ps->prefix_len ? ps->prefix_len - content_start : 0;
        if (content_len > 0 && chart_append(impl, ps->prefix + content_start, content_len) != 0) {
            return MDF_ERROR_NOMEM;
        }
        ps->prefix_len = 0;
        return MDF_OK;
    }
    if (impl->in_code_block) {
        if (prefix_matches(ps, "```")) {
            impl->in_code_block = 0;
            ps->prefix_len = 0;
            ps->decided = 1;
            ps->mode = 6;
            return render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_END, NULL, 0, 0);
        }
        if (ps->prefix[0] == '`' && ps->prefix_len < 3) {
            return MDF_OK;
        }
        ps->decided = 1;
        ps->mode = 3;
        st = render_emit(renderer, sink, MDF_TOKEN_CODE_TEXT, ps->prefix, ps->prefix_len, 0);
        ps->prefix_len = 0;
        return st;
    }
    if (ps->prefix[0] == '`' && ps->prefix_len < 3) {
        return MDF_OK;
    }
    i = 0;
    while (i < ps->prefix_len && (ps->prefix[i] == ' ' || ps->prefix[i] == '\t')) i++;
    if (i == ps->prefix_len) {
        if (i < 4) {
            return MDF_OK;
        }
        return MDF_OK;
    }
    if (impl->pending_list_blank_breaks > 0) {
        st = flush_pending_list_blank_breaks(impl, renderer, sink);
        if (st != MDF_OK) {
            return st;
        }
    }
    if (impl->pending_bare_quote_blank_depth > 0) {
        if (ps->prefix[i] == '>' && (i + 1 == ps->prefix_len || ps->prefix[i + 1] == ' ')) {
            if (i + 1 == ps->prefix_len) {
                return MDF_OK;
            }
            st = flush_pending_bare_quote_blank(ps, impl, renderer, sink, 1);
            if (st != MDF_OK) {
                return st;
            }
        } else {
            st = flush_pending_bare_quote_blank(ps, impl, renderer, sink, 0);
            if (st != MDF_OK) {
                return st;
            }
        }
    }
    if (impl->pending_quoted_list_blank_depth > 0) {
        if (ps->prefix[i] == '>' && (i + 1 == ps->prefix_len || ps->prefix[i + 1] == ' ')) {
            if (i + 1 == ps->prefix_len) {
                return MDF_OK;
            }
            st = flush_pending_quoted_list_blank(ps, impl, renderer, sink, 1);
            if (st != MDF_OK) {
                return st;
            }
        } else {
            st = flush_pending_quoted_list_blank(ps, impl, renderer, sink, 0);
            if (st != MDF_OK) {
                return st;
            }
        }
    }
    if (ps->pending_quote_depth > 0 && ps->prefix[i] != '>') {
        if (impl->pending_list_item_end) {
            if (ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') {
                if (i + 1 == ps->prefix_len) {
                    return MDF_OK;
                }
                if (ps->prefix[i + 1] == ' ') {
                    st = flush_pending_list_item_end(impl, renderer, sink);
                    if (st != MDF_OK) {
                        return st;
                    }
                    st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                    if (st != MDF_OK) {
                        return st;
                    }
                    ps->list_marker_quote_context = 1;
                    i = 0;
                    while (i < ps->prefix_len && (ps->prefix[i] == ' ' || ps->prefix[i] == '\t')) i++;
                } else {
                    st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                    if (st != MDF_OK) {
                        return st;
                    }
                    if (i > 0) {
                        memmove(ps->prefix, ps->prefix + i, ps->prefix_len - i);
                        ps->prefix_len -= i;
                    }
                    ps->pending_soft_space = 0;
                    ps->decided = 1;
                    return flush_prefix(ps, renderer, sink);
                }
            } else if (ps->prefix[i] >= '0' && ps->prefix[i] <= '9') {
                size_t j;

                j = i;
                while (j < ps->prefix_len && ps->prefix[j] >= '0' && ps->prefix[j] <= '9') {
                    j++;
                }
                if (j == ps->prefix_len) {
                    return MDF_OK;
                }
                if (ps->prefix[j] == '.' || ps->prefix[j] == ')') {
                    if (j + 1 == ps->prefix_len) {
                        return MDF_OK;
                    }
                    if (ps->prefix[j + 1] == ' ') {
                        st = flush_pending_list_item_end(impl, renderer, sink);
                        if (st != MDF_OK) {
                            return st;
                        }
                        st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                        if (st != MDF_OK) {
                            return st;
                        }
                        ps->list_marker_quote_context = 1;
                        i = 0;
                        while (i < ps->prefix_len && (ps->prefix[i] == ' ' || ps->prefix[i] == '\t')) i++;
                    } else {
                        st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                        if (st != MDF_OK) {
                            return st;
                        }
                        if (i > 0) {
                            memmove(ps->prefix, ps->prefix + i, ps->prefix_len - i);
                            ps->prefix_len -= i;
                        }
                        ps->pending_soft_space = 0;
                        ps->decided = 1;
                        return flush_prefix(ps, renderer, sink);
                    }
                } else {
                    st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                    if (st != MDF_OK) {
                        return st;
                    }
                    if (i > 0) {
                        memmove(ps->prefix, ps->prefix + i, ps->prefix_len - i);
                        ps->prefix_len -= i;
                    }
                    ps->pending_soft_space = 0;
                    ps->decided = 1;
                    return flush_prefix(ps, renderer, sink);
                }
            } else {
                st = flush_pending_quote_prefix(ps, renderer, sink, 0);
                if (st != MDF_OK) {
                    return st;
                }
                if (i > 0) {
                    memmove(ps->prefix, ps->prefix + i, ps->prefix_len - i);
                    ps->prefix_len -= i;
                }
                ps->pending_soft_space = 0;
                ps->decided = 1;
                return flush_prefix(ps, renderer, sink);
            }
        }
        if (!impl->pending_list_item_end) {
            st = flush_pending_list_item_end(impl, renderer, sink);
            if (st != MDF_OK) {
                return st;
            }
        }
        if (impl->prev_quote_line_text &&
            (ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
            i + 1 == ps->prefix_len) {
            return MDF_OK;
        }
        if ((ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
            i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ' && impl->prev_quote_line_text) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) {
                return st;
            }
            impl->prev_quote_line_text = 0;
        }
        st = flush_pending_quote_prefix(ps, renderer, sink, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->list_marker_quote_context = 1;
        i = 0;
        while (i < ps->prefix_len && (ps->prefix[i] == ' ' || ps->prefix[i] == '\t')) i++;
    }
    quoted_list_boundary = 0;
    if ((ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
        i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ') {
        quoted_list_boundary = 1;
    } else if (parse_ordered_marker(ps, i, &after_marker, &ordered_number, &ordered_delimiter)) {
        quoted_list_boundary = 1;
    }
    if (impl->prev_quote_depth > 0 && ps->quote_depth == 0 && i == 0 && ps->prefix[i] != '>') {
        if (impl->prev_quote_line_text &&
            (ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
            i + 1 == ps->prefix_len) {
            return MDF_OK;
        }
        if (impl->prev_quote_line_text && quoted_list_boundary) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) {
                return st;
            }
            impl->prev_quote_line_text = 0;
        }
        st = emit_quote_prefix_depth(ps, renderer, sink, impl->prev_quote_depth);
        if (st != MDF_OK) {
            return st;
        }
    }
    if (impl->in_indent_code && i < 4) {
        st = render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_END, NULL, 0, 0);
        if (st != MDF_OK) {
            return st;
        }
        impl->in_indent_code = 0;
    }
    if (impl->list_blank_pending && impl->list_continuation_indent > 0 &&
        i >= impl->list_continuation_indent && ps->prefix[i] == '>' &&
        i + 1 == ps->prefix_len) {
        return MDF_OK;
    }
    if (impl->list_blank_pending && impl->list_continuation_indent > 0 &&
        i >= impl->list_continuation_indent && ps->prefix[i] == '>' &&
        i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ') {
        size_t indent;

        indent = impl->list_visual_continuation_indent;
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        if (indent > 0) {
            st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->prefix, indent, 0);
            if (st != MDF_OK) return st;
        }
        impl->list_blank_pending = 0;
        impl->ordered_active = 0;
        ps->pending_quote_depth = ps->pending_quote_depth > 0 ? ps->pending_quote_depth + 1 : ps->quote_depth + 1;
        ps->prefix_len = 0;
        ps->decided = 0;
        return MDF_OK;
    }
    if (impl->list_blank_pending && impl->list_continuation_indent > 0 &&
        i >= impl->list_continuation_indent + 4) {
        size_t indent;
        size_t code_start;

        indent = impl->list_visual_continuation_indent;
        code_start = impl->list_continuation_indent + 4;
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        if (indent > 0) {
            st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->prefix, indent, 0);
            if (st != MDF_OK) return st;
        }
        impl->list_blank_pending = 0;
        impl->ordered_active = 0;
        if (!impl->in_indent_code) {
            st = render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_START, NULL, 0, 0);
            if (st != MDF_OK) return st;
            impl->in_indent_code = 1;
        }
        ps->decided = 1;
        ps->mode = 3;
        st = render_emit(renderer, sink, MDF_TOKEN_CODE_TEXT, ps->prefix + code_start, ps->prefix_len - code_start, 0);
        ps->prefix_len = 0;
        return st;
    }
    indented_list_marker = 0;
    if (impl->list_continuation_indent > 0 && i >= 4) {
        if (ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') {
            if (i + 1 == ps->prefix_len) {
                return MDF_OK;
            }
            if (ps->prefix[i + 1] == ' ') {
                indented_list_marker = 1;
            }
        } else if (ps->prefix[i] >= '0' && ps->prefix[i] <= '9') {
            size_t j;

            j = i;
            while (j < ps->prefix_len && ps->prefix[j] >= '0' && ps->prefix[j] <= '9') {
                j++;
            }
            if (j == ps->prefix_len) {
                return MDF_OK;
            }
            if (ps->prefix[j] == '.' || ps->prefix[j] == ')') {
                if (j + 1 == ps->prefix_len) {
                    return MDF_OK;
                }
                if (ps->prefix[j + 1] == ' ') {
                    indented_list_marker = 1;
                }
            }
        }
    }
    if (impl->list_continuation_indent > 0 && i >= impl->list_continuation_indent &&
        (ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
        i + 1 == ps->prefix_len) {
        return MDF_OK;
    }
    if (chart_fence_start_is_partial(ps)) {
        return MDF_OK;
    }
    if (chart_fence_start_matches(impl, ps)) {
        st = prepare_fenced_block_boundary(ps, impl, renderer, sink);
        if (st != MDF_OK) return st;
        impl->ordered_active = 0;
        impl->list_blank_pending = 0;
        ps->prefix_len = 0;
        ps->decided = 1;
        ps->mode = 7;
        impl->pending_fence = 1;
        impl->fence_info_len = 0;
        impl->fence_info[0] = '\0';
        return MDF_OK;
    }
    if (!indented_list_marker && impl->list_continuation_indent > 0 && i >= impl->list_continuation_indent &&
        ps->prefix[i] != '>' &&
        !((ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') &&
          i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ')) {
        continued_after_blank = impl->list_blank_pending;
        if (continued_after_blank && i > impl->list_visual_continuation_indent) {
            size_t trim;

            trim = i > impl->list_visual_continuation_indent ? i - impl->list_visual_continuation_indent : 0;
            memmove(ps->prefix, ps->prefix + trim, ps->prefix_len - trim);
            ps->prefix_len -= trim;
        }
        impl->list_blank_pending = 0;
        ps->decided = 1;
        if (impl->pending_list_item_end && !continued_after_blank && !impl->list_hard_break_pending) {
            ps->pending_soft_space = 1;
        }
        if (impl->list_hard_break_pending) {
            impl->list_hard_break_pending = 0;
        }
        st = flush_prefix(ps, renderer, sink);
        return st;
    }
    if ((impl->prev_quote_depth > 0 || impl->list_continuation_indent > 0) &&
        i >= 4 && ps->prefix[i] == '>' && i + 1 == ps->prefix_len) {
        return MDF_OK;
    }
    if ((impl->prev_quote_depth > 0 || impl->list_continuation_indent > 0) &&
        i >= 4 && ps->prefix[i] == '>' && i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ') {
        size_t indent;

        indent = 0;
        if (impl->prev_quote_depth == 0 && impl->list_visual_continuation_indent > 0 && impl->list_visual_continuation_indent < i) {
            indent = impl->list_visual_continuation_indent;
        }
        if (indent > 0) {
            st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->prefix, indent, 0);
            if (st != MDF_OK) return st;
        }
        ps->pending_quote_depth = ps->pending_quote_depth > 0 ? ps->pending_quote_depth + 1 : ps->quote_depth + 1;
        ps->prefix_len = 0;
        ps->decided = 0;
        return MDF_OK;
    }
    if (i >= 4 && !indented_list_marker) {
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        impl->ordered_active = 0;
        if (!impl->in_indent_code) {
            st = render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_START, NULL, 0, 0);
            if (st != MDF_OK) {
                return st;
            }
            impl->in_indent_code = 1;
        }
        ps->decided = 1;
        ps->mode = 3;
        st = render_emit(renderer, sink, MDF_TOKEN_CODE_TEXT, ps->prefix + 4, ps->prefix_len - 4, 0);
        ps->prefix_len = 0;
        return st;
    }
    hashes = 0;
    while (i + (size_t)hashes < ps->prefix_len && ps->prefix[i + (size_t)hashes] == '#') hashes++;
    if (hashes > 0 && hashes <= 6) {
        if (i + (size_t)hashes == ps->prefix_len) {
            return MDF_OK;
        }
        if (ps->prefix[i + (size_t)hashes] == ' ') {
            st = flush_pending_list_item_end(impl, renderer, sink);
            if (st != MDF_OK) return st;
            impl->ordered_active = 0;
            ps->prefix_len = 0;
            ps->decided = 1;
            ps->mode = 1;
            ps->heading_level = hashes;
            return render_emit(renderer, sink, MDF_TOKEN_HEADING_START, NULL, 0, hashes);
        }
    }
    if (ps->prefix[i] == '>' && (i + 1 == ps->prefix_len || ps->prefix[i + 1] == ' ')) {
        if (i + 1 == ps->prefix_len) {
            return MDF_OK;
        }
        marker_visual_indent = prefix_visual_cols(ps->prefix, i);
        if (impl->list_blank_pending && impl->list_visual_continuation_indent > 0 &&
            marker_visual_indent >= impl->list_continuation_indent) {
            st = emit_space_prefix(renderer, sink, impl->list_visual_continuation_indent);
            if (st != MDF_OK) return st;
            impl->list_blank_pending = 0;
        }
        if (!impl->pending_list_item_end) {
            st = flush_pending_list_item_end(impl, renderer, sink);
            if (st != MDF_OK) return st;
        }
        impl->ordered_active = 0;
        ps->pending_quote_depth = ps->pending_quote_depth > 0 ? ps->pending_quote_depth + 1 : ps->quote_depth + 1;
        ps->prefix_len = 0;
        ps->decided = 0;
        return MDF_OK;
    }
    if ((ps->prefix[i] == '-' || ps->prefix[i] == '*' || ps->prefix[i] == '+') && i + 1 < ps->prefix_len && ps->prefix[i + 1] == ' ') {
        size_t k;
        int nested_child;
        int nested_ordered_child;

        nested_child = impl->pending_list_item_end && impl->list_continuation_indent > 0 && i >= impl->list_continuation_indent;
        nested_ordered_child = nested_child && impl->ordered_active;
        marker_indent = list_visual_indent_for_marker(impl, i);
        if (nested_child && impl->list_visual_continuation_indent > marker_indent) {
            marker_indent = impl->list_visual_continuation_indent;
            for (k = 0; k < impl->list_stack_len; k++) {
                if (impl->list_source_indents[k] == i) {
                    impl->list_visual_indents[k] = marker_indent;
                    break;
                }
            }
        }
        if (marker_indent + 1 >= sizeof(ps->list_marker)) {
            return MDF_OK;
        }
        if (marker_indent > 0) {
            memset(ps->list_marker, ' ', marker_indent);
        }
        ps->list_marker[marker_indent] = '-';
        ps->list_marker_len = marker_indent + 1;
        impl->list_continuation_indent = i + 2;
        impl->list_visual_continuation_indent = marker_indent + 2;
        impl->ordered_active = 0;
        if (nested_child) {
            impl->pending_list_item_end = 0;
            st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
            if (st != MDF_OK) return st;
            if (nested_ordered_child) {
                st = render_emit(renderer, sink, MDF_TOKEN_NEWLINE, NULL, 0, 0);
                if (st != MDF_OK) return st;
            }
        } else {
            st = flush_pending_list_item_end(impl, renderer, sink);
            if (st != MDF_OK) return st;
        }
        if (impl->prev_quote_line_text && (ps->quote_depth > 0 || impl->prev_quote_depth > 0)) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) return st;
            impl->prev_quote_line_text = 0;
        }
        if (ps->pending_soft_space) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) return st;
            ps->pending_soft_space = 0;
        }
        impl->list_blank_pending = 0;
        ps->prefix_len = 0;
        ps->decided = 1;
        ps->mode = 4;
        ps->list_task_probe = 1;
        ps->list_item_start_pending = 1;
        return MDF_OK;
    }
    if (ps->prefix[i] == '+' && i + 1 == ps->prefix_len) {
        return MDF_OK;
    }
    if (ps->prefix[i] >= '0' && ps->prefix[i] <= '9') {
        size_t j;

        j = i;
        while (j < ps->prefix_len && ps->prefix[j] >= '0' && ps->prefix[j] <= '9') {
            j++;
        }
        if (j == ps->prefix_len) {
            return MDF_OK;
        }
        if ((ps->prefix[j] == '.' || ps->prefix[j] == ')') && j + 1 == ps->prefix_len) {
            return MDF_OK;
        }
    }
    if (parse_ordered_marker(ps, i, &after_marker, &ordered_number, &ordered_delimiter)) {
        marker_indent = list_visual_indent_for_marker(impl, i);
        if (!impl->ordered_active || impl->ordered_source_indent != i) {
            ordered_display = ordered_number;
        } else {
            ordered_display = impl->ordered_next;
        }
        if (!format_ordered_marker(ps, marker_indent, ordered_display, ordered_delimiter)) {
            return MDF_OK;
        }
        impl->ordered_active = 1;
        impl->ordered_source_indent = i;
        impl->ordered_next = ordered_display + 1;
        impl->list_continuation_indent = after_marker;
        impl->list_visual_continuation_indent = ps->list_marker_len + 1;
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        if (impl->prev_quote_line_text && (ps->quote_depth > 0 || impl->prev_quote_depth > 0)) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) return st;
            impl->prev_quote_line_text = 0;
        }
        if (ps->pending_soft_space) {
            st = render_emit(renderer, sink, MDF_TOKEN_PARAGRAPH_END, NULL, 0, 0);
            if (st != MDF_OK) return st;
            ps->pending_soft_space = 0;
        }
        impl->list_blank_pending = 0;
        ps->prefix_len = 0;
        ps->decided = 1;
        ps->mode = 4;
        ps->list_task_probe = 0;
        {
            int quote_level;

            quote_level = ps->quote_depth > 0 ? ps->quote_depth : ps->pending_quote_depth;
            if (quote_level <= 0) {
                quote_level = impl->prev_quote_depth;
            }
            return render_emit(renderer,
                        sink,
                        MDF_TOKEN_LIST_ITEM_START,
                        ps->list_marker,
                        ps->list_marker_len,
                        quote_level);
        }
    }
    if (chart_fence_start_matches(impl, ps)) {
        st = prepare_fenced_block_boundary(ps, impl, renderer, sink);
        if (st != MDF_OK) return st;
        impl->ordered_active = 0;
        ps->prefix_len = 0;
        ps->decided = 1;
        ps->mode = 7;
        impl->pending_fence = 1;
        impl->fence_info_len = 0;
        impl->fence_info[0] = '\0';
        return MDF_OK;
    }
    if (ps->thematic_possible) {
        if (ps->thematic_char == 0 && (c == '-' || c == '*' || c == '_')) {
            ps->thematic_char = c;
            ps->thematic_count = 1;
            return MDF_OK;
        }
        if (ps->thematic_char != 0 && c == ps->thematic_char) {
            ps->thematic_count++;
            return MDF_OK;
        }
        if (ps->thematic_char == 0 && (c == ' ' || c == '\t')) {
            return MDF_OK;
        }
        ps->thematic_possible = 0;
    }
    ps->decided = 1;
    if (impl->pending_list_item_end && !impl->list_blank_pending && !impl->list_hard_break_pending) {
        ps->pending_soft_space = 1;
    } else if (impl->pending_list_item_end && impl->list_hard_break_pending) {
        ps->pending_soft_space = 0;
        if (impl->list_visual_continuation_indent > 0) {
            size_t leading;
            size_t n;
            size_t visual;

            leading = 0;
            while (leading < ps->prefix_len && (ps->prefix[leading] == ' ' || ps->prefix[leading] == '\t')) {
                leading++;
            }
            visual = prefix_visual_cols(ps->prefix, leading);
            if (visual < impl->list_visual_continuation_indent) {
                n = impl->list_visual_continuation_indent - visual;
                if (n + ps->prefix_len >= sizeof(ps->prefix)) {
                    n = sizeof(ps->prefix) - 1 - ps->prefix_len;
                }
                memmove(ps->prefix + n, ps->prefix, ps->prefix_len);
                memset(ps->prefix, ' ', n);
                ps->prefix_len += n;
            }
        }
        impl->list_hard_break_pending = 0;
    } else if (impl->pending_list_item_end && impl->list_blank_pending) {
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        impl->list_blank_pending = 0;
        impl->ordered_active = 0;
    } else {
        st = flush_pending_list_item_end(impl, renderer, sink);
        if (st != MDF_OK) return st;
        impl->ordered_active = 0;
        impl->list_blank_pending = 0;
    }
    if (i > 0 && i < 4 &&
        impl->list_continuation_indent == 0 &&
        impl->prev_quote_depth == 0 &&
        ps->pending_quote_depth == 0 &&
        ps->quote_depth == 0) {
        memmove(ps->prefix, ps->prefix + i, ps->prefix_len - i);
        ps->prefix_len -= i;
    }
    st = flush_prefix(ps, renderer, sink);
    return st;
}

static mdf_status feed_decided(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, char c)
{
    mdf_status st;

    if (c == '\r') {
        return MDF_OK;
    }
    if (ps->mode == 7) {
        if (c == '\n') {
            int kind;

            kind = chart_info_kind(impl->fence_info, impl->fence_info_len);
            impl->pending_fence = 0;
            impl->fence_info_len = 0;
            impl->fence_info[0] = '\0';
            if (kind != 0) {
                impl->in_chart_block = 1;
                impl->chart_kind = kind;
                impl->chart_quote_depth = ps->quote_depth;
                impl->chart_len = 0;
                if (impl->chart_buf != NULL) {
                    impl->chart_buf[0] = '\0';
                }
                parse_reset_line(ps, impl);
                return MDF_OK;
            }
            impl->in_code_block = 1;
            parse_reset_line(ps, impl);
            return render_emit(renderer, sink, MDF_TOKEN_CODE_BLOCK_START, NULL, 0, 0);
        }
        if (impl->fence_info_len + 1 < sizeof(impl->fence_info)) {
            impl->fence_info[impl->fence_info_len++] = c;
            impl->fence_info[impl->fence_info_len] = '\0';
        }
        return MDF_OK;
    }
    if (ps->mode == 8) {
        if (c == '\n') {
            if (chart_append(impl, "\n", 1) != 0) {
                return MDF_ERROR_NOMEM;
            }
            parse_reset_line(ps, impl);
            return MDF_OK;
        }
        if (chart_append(impl, &c, 1) != 0) {
            return MDF_ERROR_NOMEM;
        }
        return MDF_OK;
    }
    if (c == '\n') {
        ps->hard_break = ps->trailing_spaces >= 2;
        if (ps->pending_task_space) {
            st = render_emit(renderer, sink, MDF_TOKEN_SPACE, " ", 1, 0);
            if (st != MDF_OK) {
                return st;
            }
            ps->pending_task_space = 0;
        }
        if (ps->list_task_probe) {
            st = emit_pending_list_item_start(ps, impl, renderer, sink);
            if (st != MDF_OK) {
                return st;
            }
        }
        if (ps->list_task_probe && ps->task_len > 0) {
            st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->task, ps->task_len, 0);
            if (st != MDF_OK) {
                return st;
            }
            ps->task_len = 0;
            ps->list_task_probe = 0;
        } else if (ps->list_task_probe) {
            ps->list_task_probe = 0;
        }
        return end_line(ps, impl, renderer, sink);
    }
    if (ps->mode == 6) {
        return MDF_OK;
    }
    if (ps->list_task_probe) {
        size_t j;
        int only_padding;

        only_padding = 1;
        for (j = 0; j < ps->task_len; j++) {
            if (ps->task[j] != ' ') {
                only_padding = 0;
                break;
            }
        }
        if (only_padding && c == ' ') {
            impl->list_continuation_indent++;
        }
        ps->task[ps->task_len++] = c;
        if (ps->task_len < 4) {
            return MDF_OK;
        }
        ps->list_task_probe = 0;
        st = emit_pending_list_item_start(ps, impl, renderer, sink);
        if (st != MDF_OK) return st;
        if (ps->task[0] == '[' && ps->task[2] == ']' && ps->task[3] == ' ') {
            impl->list_continuation_indent += 4;
            impl->list_visual_continuation_indent += 4;
            ps->pending_task_space = 1;
            if (ps->task[1] == 'x' || ps->task[1] == 'X') {
                return render_emit(renderer, sink, MDF_TOKEN_TASK_CHECKED, ps->task + 1, 1, 0);
            }
            if (ps->task[1] == ' ') {
                return render_emit(renderer, sink, MDF_TOKEN_TASK_UNCHECKED, NULL, 0, 0);
            }
        }
        st = render_emit(renderer, sink, MDF_TOKEN_TEXT, ps->task, ps->task_len, 0);
        ps->task_len = 0;
        return st;
    }
    if (impl->in_code_block || ps->mode == 3) {
        return render_emit(renderer, sink, MDF_TOKEN_CODE_TEXT, &c, 1, 0);
    }
    if (ps->pending_task_space) {
        st = render_emit(renderer, sink, MDF_TOKEN_SPACE, " ", 1, 0);
        if (st != MDF_OK) {
            return st;
        }
        ps->pending_task_space = 0;
    }
    if (!impl->in_code_block && ps->mode != 3 && ps->mode != 6) {
        if (c == ' ' || c == '\t') {
            if (c == ' ') {
                ps->trailing_spaces++;
            } else {
                ps->trailing_spaces = 0;
            }
            if (ps->immediate_spaces_len < sizeof(ps->immediate_spaces)) {
                ps->immediate_spaces[ps->immediate_spaces_len++] = c;
                return MDF_OK;
            }
            st = flush_immediate_spaces(ps, renderer, sink);
            if (st != MDF_OK) {
                return st;
            }
        } else if (ps->immediate_spaces_len > 0) {
            st = flush_immediate_spaces(ps, renderer, sink);
            if (st != MDF_OK) {
                return st;
            }
        }
    }
    if (c == ' ' || c == '\t') {
        if (c == ' ') {
            ps->trailing_spaces++;
        } else {
            ps->trailing_spaces = 0;
        }
        return render_emit(renderer, sink, MDF_TOKEN_SPACE, &c, 1, 0);
    }
    ps->trailing_spaces = 0;
    return render_emit(renderer, sink, MDF_TOKEN_TEXT, &c, 1, 0);
}

static mdf_status feed_parse_bytes(parse_state *ps, mdf_parser_impl *impl, mdf_renderer *renderer, mdf_sink *sink, const char *buf, size_t n)
{
    size_t i;
    mdf_status st;

    for (i = 0; i < n; i++) {
        if (!ps->decided) {
            st = decide_prefix(ps, impl, renderer, sink, buf[i]);
        } else {
            st = feed_decided(ps, impl, renderer, sink, buf[i]);
        }
        if (st != MDF_OK) {
            return st;
        }
    }
    return MDF_OK;
}

mdf_status mdf_parse_stream(mdf_parser *self, mdf_source *source, mdf_renderer *renderer, mdf_sink *sink)
{
    char buf[4096];
    parse_state ps;
    frontmatter_filter fm;
    table_filter tables;
    size_t n;
    int err;
    int decided;
    const char *filtered;
    size_t filtered_len;
    mdf_status st;
    mdf_status result;
    mdf_parser_impl *impl;

    if (self == NULL || self->impl == NULL || source == NULL || renderer == NULL || sink == NULL || source->read == NULL ||
        renderer->write_token == NULL || renderer->finish == NULL) {
        mdf_parser_set_error(self, "parse requires parser, source, renderer, and sink");
        return MDF_ERROR_INVALID;
    }
    impl = (mdf_parser_impl *)self->impl;
    memset(&ps, 0, sizeof(ps));
    memset(&fm, 0, sizeof(fm));
    table_filter_init(&tables, &impl->allocator);
    fm.allocator = &impl->allocator;
    ps.at_line_start = 1;
    ps.thematic_possible = 1;
    err = 0;
    result = MDF_OK;
    for (;;) {
        n = source->read(source->userdata, buf, sizeof(buf), &err);
        if (err != 0) {
            mdf_parser_set_error(self, "source read failed");
            result = MDF_ERROR_IO;
            goto cleanup_filters;
        }
        if (n == 0) {
            break;
        }
        if (fm.passthrough) {
            st = table_filter_feed(&tables, &ps, impl, renderer, sink, buf, n);
            if (st != MDF_OK) {
                result = st;
                goto cleanup_filters;
            }
            continue;
        }
        if (fm_append(&fm, buf, n) != 0) {
            mdf_parser_set_error(self, "out of memory");
            result = MDF_ERROR_NOMEM;
            goto cleanup_filters;
        }
        if (frontmatter_decide(&fm, 0, &filtered, &filtered_len, &decided) != 0) {
            mdf_parser_set_error(self, "frontmatter filter failed");
            result = MDF_ERROR_PARSE;
            goto cleanup_filters;
        }
        if (!decided && fm.len > MDF_MAX_FRONTMATTER_PROBE) {
            filtered = fm.probe;
            filtered_len = fm.len;
            fm.passthrough = 1;
            decided = 1;
        }
        if (decided && filtered_len > 0) {
            st = table_filter_feed(&tables, &ps, impl, renderer, sink, filtered, filtered_len);
            if (st != MDF_OK) {
                result = st;
                goto cleanup_filters;
            }
        }
    }
    if (!fm.passthrough && fm.len > 0) {
        if (frontmatter_decide(&fm, 1, &filtered, &filtered_len, &decided) != 0) {
            mdf_parser_set_error(self, "frontmatter filter failed");
            result = MDF_ERROR_PARSE;
            goto cleanup_filters;
        }
        if (decided && filtered_len > 0) {
            st = table_filter_feed(&tables, &ps, impl, renderer, sink, filtered, filtered_len);
            if (st != MDF_OK) {
                result = st;
                goto cleanup_filters;
            }
        }
    }
    mdf_free_mem(fm.allocator, fm.probe, fm.cap);
    fm.probe = NULL;
    fm.len = 0;
    fm.cap = 0;
    st = table_filter_finish(&tables, &ps, impl, renderer, sink);
    table_filter_destroy(&tables);
    if (st != MDF_OK) return st;
    if (ps.prefix_len > 0 || ps.decided) {
        st = end_line(&ps, impl, renderer, sink);
        if (st != MDF_OK) return st;
    }
    st = flush_pending_quoted_list_blank(&ps, impl, renderer, sink, 0);
    if (st != MDF_OK) return st;
    st = flush_pending_bare_quote_blank(&ps, impl, renderer, sink, -1);
    if (st != MDF_OK) return st;
    st = flush_pending_list_item_end(impl, renderer, sink);
    if (st != MDF_OK) return st;
    if (impl->in_chart_block) {
        int kind = impl->chart_kind;

        impl->in_chart_block = 0;
        impl->chart_kind = 0;
        st = emit_chart_block(&ps, impl, renderer, sink, kind);
        if (st != MDF_OK) return st;
    }
    st = render_emit(renderer, sink, MDF_TOKEN_DOCUMENT_END, NULL, 0, 0);
    if (st != MDF_OK) return st;
    return renderer->finish(renderer, sink);

cleanup_filters:
    table_filter_destroy(&tables);
    mdf_free_mem(fm.allocator, fm.probe, fm.cap);
    return result;
}
