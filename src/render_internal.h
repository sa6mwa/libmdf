#ifndef LIBMDF_RENDER_INTERNAL_H
#define LIBMDF_RENDER_INTERNAL_H

#include "mdf_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDF_MAX_FRONTMATTER_PROBE 65536

typedef struct html_segment {
    char *text;
    size_t len;
    size_t cap;
    int bold;
    int italic;
    int underline;
    int fg;
    char *link;
    size_t link_len;
    size_t link_cap;
} html_segment;

typedef struct html_state {
    html_segment *segments;
    size_t segment_count;
    size_t segment_cap;
    char *line_plain;
    size_t line_plain_len;
    size_t line_plain_cap;
    char *osc_buf;
    size_t osc_len;
    size_t osc_cap;
    char csi_buf[64];
    size_t csi_len;
    int mode;
    int bold;
    int italic;
    int underline;
    int fg;
    int boring;
    char *link;
    size_t link_len;
    size_t link_cap;
    int stream_mode;
    int stream_heading_level;
    int stream_span_open;
    int stream_link_open;
    int stream_bold;
    int stream_italic;
    int stream_underline;
    int stream_fg;
    char *stream_link;
    size_t stream_link_len;
    size_t stream_link_cap;
    char stream_utf8_buf[4];
    size_t stream_utf8_len;
    size_t stream_utf8_need;
    int pending_newline;
    int direct_prefix_open;
} html_state;

typedef struct ansi_pre_wrap_state {
    int in_pre;
    int list_item_open;
    int wrap_indent;
    int wrap_indent_in_quote;
} ansi_pre_wrap_state;

typedef struct parse_state parse_state;

typedef struct html_buf_sink {
    mdf_allocator *allocator;
    char *buf;
    size_t len;
    size_t cap;
} html_buf_sink;

typedef struct html_bridge_sink {
    mdf_renderer *renderer;
    mdf_sink *sink;
    int boring;
} html_bridge_sink;

int render_token_is_text(mdf_token_type t);
mdf_status render_emit(mdf_renderer *renderer, mdf_sink *sink, mdf_token_type type, const char *text, size_t len, int level);
mdf_status mdf_renderer_fail_session(mdf_renderer *self, mdf_status st, const char *msg);
mdf_status mdf_renderer_fail_from_impl(mdf_renderer *self, mdf_impl *impl, const char *fallback_msg);
mdf_status mdf_renderer_fail_sink_write(mdf_renderer *self);
mdf_impl *mdf_renderer_require_sink(mdf_renderer *self, mdf_sink *sink, const char *msg);
int mdf_token_has_missing_text(const mdf_token *token);
void mdf_impl_mark_oom(mdf_impl *impl);

int ansi_write_token(mdf_renderer *self, const mdf_token *token, mdf_sink *sink);
int ansi_flush_word(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_visible_chunk(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len);
int ansi_emit_pending_style_reset(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_styled_visible_chunk(mdf_impl *impl, mdf_sink *sink, const char *style, const char *src, size_t len);
int ansi_write_styled_words_inner(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, const char *style, int open_style, size_t trailing_reserve);
int ansi_write_visible_char(mdf_impl *impl, mdf_sink *sink, char c);
int ansi_write_visible(mdf_impl *impl, mdf_sink *sink, const char *src, size_t len);
int ansi_write_visible_cstr(mdf_impl *impl, mdf_sink *sink, const char *src);
int ansi_write_spaces(mdf_impl *impl, mdf_sink *sink, size_t count);
int ansi_write_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_ensure_left_margin(mdf_impl *impl, mdf_sink *sink);
int ansi_wrap_limit(const mdf_impl *impl);
int ansi_content_limit(mdf_impl *impl);
int ansi_inline_append(mdf_impl *impl, char **buf, size_t *len, size_t *cap, const char *src, size_t n);
int ansi_inline_flush_literal_char(mdf_impl *impl, mdf_sink *sink, char c);
int ansi_write_standalone_visible_char(mdf_impl *impl, mdf_sink *sink, char c);
int ansi_inline_emit_streamed_emphasis_word(mdf_impl *impl, mdf_sink *sink, int closing);
void ansi_inline_clear_emphasis_state(mdf_impl *impl);
int ansi_inline_emit_streamed_emphasis_char(mdf_impl *impl, mdf_sink *sink, char c);
int ansi_inline_flush_streamed_emphasis_closers(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_quote_prefix(mdf_impl *impl, mdf_sink *sink, size_t prefix_spaces, size_t extra_spaces);
int ansi_emit_quote_prefix_split(mdf_impl *impl, mdf_sink *sink, size_t prefix_spaces, size_t extra_spaces);
int ansi_emit_quote_marker(mdf_impl *impl, mdf_sink *sink, int trailing_space);
void ansi_update_visible_output_state(mdf_impl *impl, const char *src, size_t len);
int ansi_emit_styled_prefix(mdf_impl *impl, mdf_sink *sink, const char *style);
int ascii_is_digit_char(char c);
int ansi_flush_pending_list_marker(mdf_impl *impl, mdf_sink *sink);
int ansi_flush_pending_breaks(mdf_impl *impl, mdf_sink *sink);
int ansi_flush_pending_space(mdf_impl *impl, mdf_sink *sink);
int ansi_flush_pre_code_buffer(mdf_impl *impl, mdf_sink *sink);
int ansi_inline_flush_literal(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_plain_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_quote_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_quote_blank_newline(mdf_impl *impl, mdf_sink *sink);
int ansi_finish_heading_end(mdf_impl *impl, mdf_sink *sink);
int ansi_emit_link_fallback_only(mdf_impl *impl, mdf_sink *sink, const char *url, size_t url_len, int outer_paren_context);
int ansi_emit_task_marker(mdf_impl *impl, mdf_sink *sink, char marker);
int ansi_set_pending_list_marker_state(mdf_impl *impl, const char *text, size_t len);
void ansi_set_pending_wrapped_link_punct(mdf_impl *impl, char c);
int ansi_flush_pending_wrapped_link_fallback(mdf_impl *impl, mdf_sink *sink);
int ansi_flush_pending_wrapped_link_punct(mdf_impl *impl, mdf_sink *sink);
void ansi_raise_pending_breaks(mdf_impl *impl, int count);
void ansi_clear_pending_quote_end_state(mdf_impl *impl);
int ansi_emit_resumed_quote_paragraph(mdf_impl *impl, mdf_sink *sink, int emit_blank_quote_line, int *emitted_break);
void ansi_clear_pending_list_marker_state(mdf_impl *impl);
void ansi_pre_wrap_state_capture(ansi_pre_wrap_state *state, mdf_impl *impl);
void ansi_pre_wrap_state_suspend(mdf_impl *impl);
void ansi_pre_wrap_state_restore(mdf_impl *impl, const ansi_pre_wrap_state *state);
int ansi_flush_pending_list_marker_if_any(mdf_impl *impl, mdf_sink *sink);
int ansi_flush_pre_code_if_active(mdf_impl *impl, mdf_sink *sink);
int ansi_prepare_for_token(mdf_impl *impl, mdf_sink *sink, const mdf_token *token);
int ansi_start_list_item(mdf_impl *impl, mdf_sink *sink, const mdf_token *token);
int ansi_write_task_token(mdf_impl *impl, mdf_sink *sink, const mdf_token *token, int checked);
int ansi_finish_list_item(mdf_impl *impl, mdf_sink *sink);
int ansi_handle_pending_quote_block_start(mdf_impl *impl, mdf_sink *sink, const mdf_token *token);
int ansi_begin_blockquote(mdf_impl *impl, mdf_sink *sink, int prior_quote_prefix_indent, int was_quote_open);
int ansi_end_blockquote(mdf_impl *impl, mdf_sink *sink);
int ansi_end_paragraph(mdf_impl *impl, mdf_sink *sink);
int ansi_finish_document_end(mdf_impl *impl, mdf_sink *sink);
void ansi_reset_line_output_state(mdf_impl *impl);

size_t html_line_prefix_len(const char *s, size_t len);
int html_start(mdf_renderer *self, mdf_sink *sink);
int html_write_style(mdf_impl *impl, mdf_sink *sink, const html_segment *seg, int list_marker);
int html_emit_segment_slice(mdf_impl *impl, mdf_sink *sink, const html_segment *seg, size_t off, size_t len, const html_segment *base_style, int list_marker, int suppress_link);
int html_emit_line_range(mdf_impl *impl, html_state *state, mdf_sink *sink, size_t start, size_t end, const html_segment *base_style, int prefix_mode, int suppress_link);
int html_buf_sink_write(void *userdata, const char *src, size_t len);
int html_write_open_link_grouped(mdf_impl *impl, mdf_sink *sink, const char *link, size_t link_len);
int html_write_token(mdf_renderer *self, const mdf_token *token, mdf_sink *sink);
html_state *html_state_get(mdf_impl *impl);
void html_state_release(mdf_impl *impl, html_state *state);
void html_state_destroy(mdf_impl *impl, html_state **state_ptr);
int html_direct_close_prefix_line(mdf_sink *sink, int final_line);
int html_flush_pending_lines(mdf_renderer *self, mdf_sink *sink, int final_line, char next_char);
int html_bridge_run_ansi_newline(mdf_renderer *self, mdf_sink *sink);
int html_bridge_write(void *userdata, const char *src, size_t len);
void html_state_reset(html_state *state);
void html_parse_style_prefix(html_state *state, const char *style);
int html_parse_ansi_inline(mdf_impl *impl, html_state *state, const char *src, size_t len);
int html_emit_inline_state(mdf_impl *impl, html_state *state, mdf_sink *sink, int header);
const char *html_theme_quote_rgb(mdf_impl *impl, char *buf, size_t buf_len);

size_t utf8_decode_codepoint(const char *s, size_t len, unsigned long *cp);
size_t utf8_display_width(unsigned long cp);

mdf_status ansi_renderer_finish(mdf_renderer *self, mdf_impl *impl, mdf_sink *sink);
mdf_status html_renderer_finish(mdf_renderer *self, mdf_impl *impl, mdf_sink *sink);

const char *mdf_theme_heading(const mdf_impl *impl, int level);
const char *mdf_theme_emphasis(const mdf_impl *impl);
const char *mdf_theme_strong(const mdf_impl *impl);
const char *mdf_theme_emphasis_strong(const mdf_impl *impl);
const char *mdf_theme_code_inline(const mdf_impl *impl);
const char *mdf_theme_code_block(const mdf_impl *impl);
const char *mdf_theme_quote(const mdf_impl *impl);
const char *mdf_theme_quote_text(const mdf_impl *impl);
const char *mdf_theme_list_marker(const mdf_impl *impl);
const char *mdf_theme_link_text(const mdf_impl *impl);
const char *mdf_theme_link_url(const mdf_impl *impl);
const char *mdf_theme_thematic_break(const mdf_impl *impl);
const char *mdf_theme_table_header(const mdf_impl *impl);
const char *mdf_theme_table_wire(const mdf_impl *impl);

#endif
