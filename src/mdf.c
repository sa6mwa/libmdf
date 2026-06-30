#include "mdf_internal.h"

#include <stddef.h>
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

static void mdf_impl_reset_render_state(mdf_impl *impl)
{
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

static mdf_status instance_render(mdf_renderer *self, mdf_source *source, mdf_sink *sink)
{
    mdf_parser *parser;
    mdf_impl *impl;
    mdf_status st;
    const char *parser_err;

    impl = (mdf_impl *)self->impl;
    if (source == NULL || sink == NULL || source->read == NULL || sink->write == NULL) {
        mdf_set_error(self, "render requires instance, source, and sink");
        return MDF_ERROR_INVALID;
    }
    mdf_renderer_reset_session_state(self);
    parser = NULL;
    st = mdf_parser_create(&impl->opts, &parser);
    if (st != MDF_OK) {
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
    opts->theme_name = "default";
    opts->html_content_width_ch = 96.0;
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

    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL) {
        return MDF_ERROR_INVALID;
    }
    if (format != MDF_FORMAT_ANSI && format != MDF_FORMAT_HTML) {
        return MDF_ERROR_INVALID;
    }
    mdf_options_resolve(opts, &defaults);
    if (defaults.margin_left < 0 || defaults.margin_right < 0) {
        return MDF_ERROR_INVALID;
    }
    if (format == MDF_FORMAT_ANSI &&
        defaults.width > 0 &&
        defaults.margin_left + defaults.margin_right >= defaults.width) {
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
    st = mdf_impl_configure_emit_buffer(impl);
    if (st != MDF_OK) {
        mdf_memory_destroy(&impl->memory);
        mdf_free_mem(&impl->user_allocator, impl, sizeof(*impl));
        mdf_free_mem(&impl->user_allocator, r, sizeof(*r));
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
    if (impl->format != MDF_FORMAT_HTML) {
        return MDF_ERROR_INVALID;
    }
    if (title == NULL) {
        mdf_free_mem(&impl->allocator, impl->html_title, impl->html_title_cap);
        impl->html_title = NULL;
        impl->html_title_cap = 0;
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
