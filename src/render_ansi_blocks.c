#include "render_internal.h"

static void ansi_clear_pending_wrapped_link_fallback(mdf_impl *impl)
{
    impl->pending_wrapped_link_fallback = 0;
    impl->pending_wrapped_link_outer_paren = 0;
    if (!impl->pending_fallback_exact) {
        impl->pending_fallback_url_len = 0;
    }
}

static void ansi_clear_pending_wrapped_link_punct(mdf_impl *impl)
{
    impl->pending_wrapped_link_punct_valid = 0;
    impl->pending_wrapped_link_punct = 0;
}

void ansi_set_pending_wrapped_link_punct(mdf_impl *impl, char c)
{
    impl->pending_wrapped_link_punct_valid = 1;
    impl->pending_wrapped_link_punct = c;
}

int ansi_flush_pending_wrapped_link_fallback(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->pending_wrapped_link_fallback) {
        return 0;
    }
    if (ansi_emit_link_fallback_only(impl, sink, impl->pending_fallback_url, impl->pending_fallback_url_len,
            impl->pending_wrapped_link_outer_paren) != 0) {
        return -1;
    }
    ansi_clear_pending_wrapped_link_fallback(impl);
    return 0;
}

int ansi_flush_pending_wrapped_link_punct(mdf_impl *impl, mdf_sink *sink)
{
    if (!impl->pending_wrapped_link_punct_valid) {
        return 0;
    }
    if (ansi_write_standalone_visible_char(impl, sink, impl->pending_wrapped_link_punct) != 0) {
        return -1;
    }
    ansi_clear_pending_wrapped_link_punct(impl);
    return 0;
}

void ansi_raise_pending_breaks(mdf_impl *impl, int count)
{
    if (impl->pending_breaks < count) {
        impl->pending_breaks = count;
    }
}

void ansi_clear_pending_quote_end_state(mdf_impl *impl)
{
    impl->pending_quote_end = 0;
    impl->quote_wrap_active = 0;
}

static void ansi_begin_quote_paragraph_resume(mdf_impl *impl)
{
    impl->pending_quote_paragraph_separator = 0;
    impl->quote_open = 1;
    impl->quote_wrap_active = 1;
}

int ansi_emit_resumed_quote_paragraph(mdf_impl *impl, mdf_sink *sink, int emit_blank_quote_line, int *emitted_break)
{
    ansi_begin_quote_paragraph_resume(impl);
    if (emit_blank_quote_line) {
        if (ansi_emit_quote_blank_newline(impl, sink) != 0) {
            return -1;
        }
        if (emitted_break != NULL) {
            *emitted_break = 1;
        }
    } else if (impl->pending_quote_end && impl->ansi_col > 0) {
        if (ansi_emit_plain_newline(impl, sink) != 0) {
            return -1;
        }
        if (emitted_break != NULL) {
            *emitted_break = 1;
        }
    }
    if (ansi_emit_quote_newline(impl, sink) != 0) {
        return -1;
    }
    impl->pending_quote_end = 0;
    if (emitted_break != NULL) {
        *emitted_break = 1;
    }
    return 0;
}

void ansi_clear_pending_list_marker_state(mdf_impl *impl)
{
    impl->pending_list_marker = 0;
    impl->pending_list_marker_quote_context = 0;
    impl->pending_list_marker_len = 0;
    memset(impl->pending_list_marker_text, 0, sizeof(impl->pending_list_marker_text));
}

void ansi_pre_wrap_state_capture(ansi_pre_wrap_state *state, mdf_impl *impl)
{
    state->in_pre = impl->in_pre;
    state->list_item_open = impl->list_item_open;
    state->wrap_indent = impl->ansi_wrap_indent;
    state->wrap_indent_in_quote = impl->ansi_wrap_indent_in_quote;
}

void ansi_pre_wrap_state_suspend(mdf_impl *impl)
{
    impl->in_pre = 0;
    impl->list_item_open = 0;
    impl->ansi_wrap_indent = 0;
    impl->ansi_wrap_indent_in_quote = 0;
}

void ansi_pre_wrap_state_restore(mdf_impl *impl, const ansi_pre_wrap_state *state)
{
    impl->in_pre = state->in_pre;
    impl->list_item_open = state->list_item_open;
    impl->ansi_wrap_indent = state->wrap_indent;
    impl->ansi_wrap_indent_in_quote = state->wrap_indent_in_quote;
}

int ansi_flush_pending_list_marker_if_any(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->pending_list_marker) {
        if (ansi_flush_pending_list_marker(impl, sink) != 0) {
            return -1;
        }
    }
    return 0;
}

int ansi_flush_pre_code_if_active(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->in_pre && ansi_flush_pre_code_buffer(impl, sink) != 0) {
        return -1;
    }
    return 0;
}

static int ansi_prepare_task_marker(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_pending_list_marker_if_any(impl, sink) != 0) {
        return -1;
    }
    if (ansi_flush_pre_code_if_active(impl, sink) != 0) {
        return -1;
    }
    if (impl->list_item_open) {
        impl->ansi_wrap_indent += 4;
    }
    return 0;
}

int ansi_start_list_item(mdf_impl *impl, mdf_sink *sink, const mdf_token *token)
{
    if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
    if (impl->pending_quote_paragraph_separator == 2) {
        if (ansi_emit_resumed_quote_paragraph(impl, sink, 1, NULL) != 0) return -1;
    }
    impl->in_paragraph = 0;
    impl->list_item_open = 1;
    if (ansi_set_pending_list_marker_state(impl, token->text, token->len) != 0) {
        return -1;
    }
    if (token->level > 0) {
        impl->pending_list_marker_quote_context = 1;
    }
    return 0;
}

int ansi_write_task_token(mdf_impl *impl, mdf_sink *sink, const mdf_token *token, int checked)
{
    char marker;

    if (ansi_prepare_task_marker(impl, sink) != 0) return -1;
    if (!checked) {
        return ansi_emit_task_marker(impl, sink, ' ');
    }
    marker = (token->text != NULL && token->len == 1 && token->text[0] == 'X') ? 'X' : 'x';
    return ansi_emit_task_marker(impl, sink, marker);
}

int ansi_finish_list_item(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_pending_list_marker_if_any(impl, sink) != 0) return -1;
    if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
    if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
    if (ansi_flush_word(impl, sink) != 0) return -1;
    impl->list_item_open = 0;
    impl->list_blank_newline_emitted = 0;
    if (impl->pending_quote_end) {
        if (impl->quote_prefix_indent > 0 && impl->ansi_wrap_indent > 0 && !impl->ansi_wrap_indent_in_quote) {
            impl->pending_list_quote_blank_indent = impl->quote_prefix_indent < 4 ? 4 : impl->quote_prefix_indent;
        }
        ansi_clear_pending_quote_end_state(impl);
    }
    impl->ansi_wrap_indent = 0;
    impl->ansi_wrap_indent_in_quote = 0;
    if (impl->ansi_col == 0) {
        return 0;
    }
    return ansi_emit_plain_newline(impl, sink);
}

int ansi_handle_pending_quote_block_start(mdf_impl *impl, mdf_sink *sink, const mdf_token *token)
{
    int visible_col;

    impl->pending_quote_end = 0;
    if (token->level <= 0 && impl->ansi_col > 0) {
        impl->quote_open = 1;
        visible_col = impl->ansi_col;
        if (impl->opts.margin_left > 0 && visible_col >= impl->opts.margin_left) {
            visible_col -= impl->opts.margin_left;
        }
        if (impl->pending_quote_reopen_prefix) {
            impl->pending_quote_reopen_prefix = 0;
            return 1;
        }
        if (impl->inline_mode != 5 &&
            visible_col == impl->quote_prefix_indent + 2 &&
            impl->ansi_prev_char == ' ' &&
            !impl->ansi_pending_space) {
            return 1;
        }
        if (impl->inline_mode == 5) {
            if (ansi_inline_flush_streamed_emphasis_closers(impl, sink) != 0) return -1;
            return ansi_inline_emit_streamed_emphasis_char(impl, sink, ' ') == 0 ? 1 : -1;
        }
        if (impl->quote_text_open && !impl->opts.boring) {
            impl->quote_text_open = 0;
            if (mdf_emit_cstr(impl, sink, "\033[0m") != 0) return -1;
        } else {
            impl->quote_text_open = 0;
        }
        impl->ansi_pending_space = 1;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 1;
        return 1;
    }
    if (impl->ansi_col > 0 && ansi_write_newline(impl, sink) != 0) {
        return -1;
    }
    return 0;
}

int ansi_begin_blockquote(mdf_impl *impl, mdf_sink *sink, int prior_quote_prefix_indent, int was_quote_open)
{
    int i;
    int prior_visible_indent;

    if (ansi_flush_pending_breaks(impl, sink) != 0) return -1;
    if (ansi_flush_pending_space(impl, sink) != 0) return -1;
    impl->quote_open = 1;
    impl->quote_depth++;
    impl->quote_wrap_active = 1;
    if (impl->ansi_col == 0 && prior_quote_prefix_indent > 0) {
        prior_visible_indent = prior_quote_prefix_indent;
        if (impl->opts.margin_left > 0 && prior_visible_indent >= impl->opts.margin_left) {
            if (ansi_ensure_left_margin(impl, sink) != 0) return -1;
            prior_visible_indent -= impl->opts.margin_left;
        }
        for (i = 0; i < prior_visible_indent; i++) {
            if (mdf_emit_cstr(impl, sink, " ") != 0) return -1;
            impl->ansi_col++;
        }
    }
    if (!was_quote_open) {
        impl->quote_prefix_indent = impl->ansi_col;
    }
    if (ansi_emit_quote_marker(impl, sink, 0) != 0) return -1;
    impl->ansi_col++;
    impl->ansi_prev_char = '>';
    impl->ansi_pending_space = 1;
    impl->ansi_pending_space_no_split = 0;
    impl->ansi_pending_space_plain = 0;
    return 0;
}

int ansi_end_blockquote(mdf_impl *impl, mdf_sink *sink)
{
    int visible_col;

    if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
    if (impl->inline_mode != 5 && ansi_inline_flush_literal(impl, sink) != 0) return -1;
    if (ansi_flush_word(impl, sink) != 0) return -1;
    if (impl->quote_depth > 0) {
        impl->quote_depth--;
    }
    impl->quote_open = impl->quote_depth > 0;
    if (impl->inline_mode == 5) {
        impl->pending_quote_end = 1;
        return 0;
    }
    if (impl->ansi_col == 0) {
        ansi_clear_pending_quote_end_state(impl);
        if (impl->quote_depth == 0) {
            impl->quote_prefix_indent = 0;
        }
        return 0;
    }
    visible_col = impl->ansi_col;
    if (impl->opts.margin_left > 0 && visible_col >= impl->opts.margin_left) {
        visible_col -= impl->opts.margin_left;
    }
    if (visible_col <= impl->quote_prefix_indent + 1 && impl->ansi_pending_space) {
        impl->ansi_pending_space = 0;
        impl->ansi_pending_space_no_split = 0;
        impl->ansi_pending_space_plain = 0;
        ansi_clear_pending_quote_end_state(impl);
        return ansi_write_newline(impl, sink);
    }
    impl->pending_quote_end = 1;
    return 0;
}

int ansi_end_paragraph(mdf_impl *impl, mdf_sink *sink)
{
    if (ansi_flush_pending_list_marker_if_any(impl, sink) != 0) return -1;
    if (ansi_flush_pre_code_if_active(impl, sink) != 0) return -1;
    if (ansi_inline_flush_literal(impl, sink) != 0) return -1;
    if (impl->pending_quote_end) {
        if (impl->quote_wrap_active) {
            impl->pending_quote_paragraph_separator = 1;
            return 0;
        }
        if (impl->ansi_col > 0 && ansi_write_newline(impl, sink) != 0) return -1;
        ansi_clear_pending_quote_end_state(impl);
        ansi_raise_pending_breaks(impl, 1);
        return 0;
    }
    if (!impl->in_paragraph) {
        impl->quote_wrap_active = 0;
        return 0;
    }
    impl->in_paragraph = 0;
    impl->quote_wrap_active = 0;
    ansi_raise_pending_breaks(impl, 2);
    return 0;
}

int ansi_finish_document_end(mdf_impl *impl, mdf_sink *sink)
{
    if (impl->pending_quote_end) {
        ansi_clear_pending_quote_end_state(impl);
        impl->quote_prefix_indent = 0;
        return ansi_write_newline(impl, sink);
    }
    impl->quote_prefix_indent = 0;
    return 0;
}

int ansi_emit_task_marker(mdf_impl *impl, mdf_sink *sink, char marker)
{
    char buf[4];

    buf[0] = '[';
    buf[1] = marker;
    buf[2] = ']';
    buf[3] = '\0';
    return ansi_write_visible_cstr(impl, sink, buf);
}

int ansi_set_pending_list_marker_state(mdf_impl *impl, const char *text, size_t len)
{
    ansi_clear_pending_list_marker_state(impl);
    impl->pending_list_marker = 1;
    impl->pending_list_marker_quote_context =
        impl->quote_open || impl->quote_wrap_active || impl->quote_depth > 0 ||
        impl->quote_prefix_indent > 0 || impl->pending_quote_reopen_prefix;
    if (text != NULL && len > 0) {
        if (len > sizeof(impl->pending_list_marker_text) - 1) {
            return -1;
        }
        memcpy(impl->pending_list_marker_text, text, len);
        impl->pending_list_marker_len = len;
        return 0;
    }
    impl->pending_list_marker_text[0] = '-';
    impl->pending_list_marker_len = 1;
    return 0;
}

static int ansi_token_starts_inline_text(mdf_token_type type)
{
    return type == MDF_TOKEN_TEXT ||
           type == MDF_TOKEN_SPACE ||
           type == MDF_TOKEN_CODE_TEXT;
}

int ansi_prepare_for_token(mdf_impl *impl, mdf_sink *sink, const mdf_token *token)
{
    if (!ansi_token_starts_inline_text(token->type) && impl->pending_wrapped_link_fallback) {
        if (ansi_flush_pending_wrapped_link_fallback(impl, sink) != 0) return -1;
    }
    if (!ansi_token_starts_inline_text(token->type) && impl->pending_wrapped_link_punct_valid) {
        if (ansi_flush_pending_wrapped_link_punct(impl, sink) != 0) return -1;
    }
    if (impl->pending_quote_reopen_prefix &&
        token->type != MDF_TOKEN_BLOCKQUOTE_END &&
        token->type != MDF_TOKEN_BLOCKQUOTE_START) {
        impl->pending_quote_reopen_prefix = 0;
    }
    return 0;
}
