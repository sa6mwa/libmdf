#include "render_internal.h"

int render_token_is_text(mdf_token_type t)
{
    return t == MDF_TOKEN_TEXT || t == MDF_TOKEN_SPACE || t == MDF_TOKEN_CODE_TEXT || t == MDF_TOKEN_CHART_BLOCK;
}

mdf_status mdf_renderer_fail_session(mdf_renderer *self, mdf_status st, const char *msg)
{
    if (st != MDF_ERROR_INVALID) {
        mdf_renderer_reset_session_state(self);
    }
    mdf_set_error(self, msg);
    return st;
}

mdf_status mdf_renderer_fail_from_impl(mdf_renderer *self, mdf_impl *impl, const char *fallback_msg)
{
    char msg[sizeof(impl->error)];

    if (impl->error[0] != '\0') {
        strncpy(msg, impl->error, sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
        if (strcmp(impl->error, "out of memory") == 0) {
            return mdf_renderer_fail_session(self, MDF_ERROR_NOMEM, msg);
        }
        return mdf_renderer_fail_session(self, MDF_ERROR_IO, msg);
    }
    return mdf_renderer_fail_session(self, MDF_ERROR_IO, fallback_msg);
}

mdf_status mdf_renderer_fail_sink_write(mdf_renderer *self)
{
    return mdf_renderer_fail_session(self, MDF_ERROR_IO, "sink write failed");
}

mdf_impl *mdf_renderer_require_sink(mdf_renderer *self, mdf_sink *sink, const char *msg)
{
    if (self == NULL || self->impl == NULL || sink == NULL || sink->write == NULL) {
        mdf_set_error(self, msg);
        return NULL;
    }
    return (mdf_impl *)self->impl;
}

int mdf_token_has_missing_text(const mdf_token *token)
{
    return render_token_is_text(token->type) && token->text == NULL && token->len != 0;
}

void mdf_impl_mark_oom(mdf_impl *impl)
{
    strncpy(impl->error, "out of memory", sizeof(impl->error) - 1);
    impl->error[sizeof(impl->error) - 1] = '\0';
}

mdf_status render_emit(mdf_renderer *renderer, mdf_sink *sink, mdf_token_type type, const char *text, size_t len, int level)
{
    mdf_token tok;

    tok.type = type;
    tok.text = text;
    tok.len = len;
    tok.level = level;
    return renderer->write_token(renderer, &tok, sink);
}

static const mdf_theme_style *theme_or_default(const mdf_impl *impl)
{
    const mdf_theme_style *theme;

    theme = impl == NULL ? NULL : impl->theme;
    if (theme == NULL) {
        theme = mdf_theme_resolve("default");
    }
    return theme;
}

const char *mdf_theme_heading(const mdf_impl *impl, int level)
{
    const mdf_theme_style *theme;

    if (level <= 0 || level > 6) {
        level = 1;
    }
    theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->heading[level - 1];
}

const char *mdf_theme_emphasis(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->emphasis;
}

const char *mdf_theme_strong(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->strong;
}

const char *mdf_theme_emphasis_strong(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->emphasis_strong;
}

const char *mdf_theme_code_inline(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->code_inline;
}

const char *mdf_theme_code_block(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->code_block;
}

const char *mdf_theme_quote(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->quote;
}

const char *mdf_theme_quote_text(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->quote_text;
}

const char *mdf_theme_list_marker(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->list_marker;
}

const char *mdf_theme_link_text(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->link_text;
}

const char *mdf_theme_link_url(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->link_url;
}

const char *mdf_theme_thematic_break(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->thematic_break;
}

const char *mdf_theme_table_header(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->table_header;
}

const char *mdf_theme_table_wire(const mdf_impl *impl)
{
    const mdf_theme_style *theme = theme_or_default(impl);
    return theme == NULL ? "" : theme->table_wire;
}


void mdf_render_release_state(mdf_impl *impl)
{
    html_state *state;

    if (impl == NULL) {
        return;
    }
    state = (html_state *)impl->html_state;
    html_state_destroy(impl, &state);
    impl->html_state = NULL;
}

void mdf_render_reset_state(mdf_impl *impl)
{
    html_state *state;

    if (impl == NULL || impl->html_state == NULL) {
        return;
    }
    state = (html_state *)impl->html_state;
    html_state_release(impl, state);
}

mdf_status mdf_renderer_finish_internal(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;
    mdf_status st;

    impl = mdf_renderer_require_sink(self, sink, "finish requires renderer and sink");
    if (impl == NULL) {
        return MDF_ERROR_INVALID;
    }
    if (impl->format == MDF_FORMAT_HTML_DECK) {
        mdf_set_error(self, "deck renderers do not support token streaming");
        return MDF_ERROR_INVALID;
    }
    if (impl->format == MDF_FORMAT_HTML) {
        st = html_renderer_finish(self, impl, sink);
    } else {
        st = ansi_renderer_finish(self, impl, sink);
    }
    if (st != MDF_OK) {
        return st;
    }
    mdf_renderer_reset_session_state(self);
    return MDF_OK;
}
