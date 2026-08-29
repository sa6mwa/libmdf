#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mdf.c"
#include "../src/render.c"
#include "../src/render_ansi_blocks.c"
#include "../src/render_html.c"
#include "../src/render_ansi.c"
#include "../src/render_parse.c"
#include "../src/html_fonts.c"
#include "../src/unicode_classify.c"

typedef struct bridge_source {
    const char *data;
    size_t len;
    size_t off;
    size_t chunk;
} bridge_source;

typedef struct bridge_buffer {
    char *buf;
    size_t len;
    size_t cap;
} bridge_buffer;

typedef struct bridge_sink {
    bridge_buffer out;
    bridge_buffer trace;
    unsigned long seq;
    int trace_enabled;
    int failed;
} bridge_sink;

static int bridge_buf_append(bridge_buffer *buf, const char *src, size_t len)
{
    char *next;
    size_t new_cap;
    size_t need;

    if (len == 0) {
        return 0;
    }
    if (src == NULL) {
        return -1;
    }
    if (len > ((size_t)-1) - buf->len - 1) {
        return -1;
    }
    need = buf->len + len + 1;
    if (need > buf->cap) {
        new_cap = buf->cap == 0 ? 256 : buf->cap;
        while (new_cap < need) {
            if (new_cap > ((size_t)-1) / 2) {
                new_cap = need;
                break;
            }
            new_cap *= 2;
        }
        next = (char *)realloc(buf->buf, new_cap);
        if (next == NULL) {
            return -1;
        }
        buf->buf = next;
        buf->cap = new_cap;
    }
    memcpy(buf->buf + buf->len, src, len);
    buf->len += len;
    buf->buf[buf->len] = '\0';
    return 0;
}

static int bridge_buf_append_cstr(bridge_buffer *buf, const char *src)
{
    return bridge_buf_append(buf, src, strlen(src));
}

static int bridge_buf_append_ulong(bridge_buffer *buf, unsigned long value)
{
    char tmp[32];
    size_t len;

    len = (size_t)snprintf(tmp, sizeof(tmp), "%lu", value);
    if (len >= sizeof(tmp)) {
        return -1;
    }
    return bridge_buf_append(buf, tmp, len);
}

static int bridge_trace_base64(bridge_buffer *buf, const unsigned char *src, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[4];
    size_t i;
    unsigned int value;

    i = 0;
    while (i + 3 <= len) {
        value = ((unsigned int)src[i] << 16) | ((unsigned int)src[i + 1] << 8) | (unsigned int)src[i + 2];
        out[0] = table[(value >> 18) & 63];
        out[1] = table[(value >> 12) & 63];
        out[2] = table[(value >> 6) & 63];
        out[3] = table[value & 63];
        if (bridge_buf_append(buf, out, sizeof(out)) != 0) {
            return -1;
        }
        i += 3;
    }
    if (i < len) {
        value = (unsigned int)src[i] << 16;
        if (i + 1 < len) {
            value |= (unsigned int)src[i + 1] << 8;
        }
        out[0] = table[(value >> 18) & 63];
        out[1] = table[(value >> 12) & 63];
        out[2] = i + 1 < len ? table[(value >> 6) & 63] : '=';
        out[3] = '=';
        if (bridge_buf_append(buf, out, sizeof(out)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int bridge_trace_append_emit(bridge_sink *sink, const char *src, size_t len)
{
    if (!sink->trace_enabled) {
        return 0;
    }
    sink->seq++;
    if (bridge_buf_append_cstr(&sink->trace, "{\"seq\":") != 0) return -1;
    if (bridge_buf_append_ulong(&sink->trace, sink->seq) != 0) return -1;
    if (bridge_buf_append_cstr(&sink->trace, ",\"format\":\"ansi\",\"op\":\"emit\",\"bytes\":") != 0) return -1;
    if (bridge_buf_append_ulong(&sink->trace, (unsigned long)len) != 0) return -1;
    if (bridge_buf_append_cstr(&sink->trace, ",\"data_b64\":\"") != 0) return -1;
    if (bridge_trace_base64(&sink->trace, (const unsigned char *)src, len) != 0) return -1;
    return bridge_buf_append_cstr(&sink->trace, "\"}\n");
}

static int bridge_trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    bridge_sink *sink;

    (void)format;
    sink = (bridge_sink *)userdata;
    if (bridge_trace_append_emit(sink, src, len) != 0) {
        sink->failed = 1;
        return -1;
    }
    return 0;
}

static size_t bridge_read(void *userdata, char *dst, size_t cap, int *err)
{
    bridge_source *src;
    size_t n;
    size_t remain;

    src = (bridge_source *)userdata;
    *err = 0;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->chunk;
    if (n == 0 || n > cap) {
        n = cap;
    }
    remain = src->len - src->off;
    if (n > remain) {
        n = remain;
    }
    memcpy(dst, src->data + src->off, n);
    src->off += n;
    return n;
}

static int bridge_write(void *userdata, const char *src, size_t len)
{
    bridge_sink *sink;

    sink = (bridge_sink *)userdata;
    if (bridge_buf_append(&sink->out, src, len) != 0) {
        sink->failed = 1;
        return -1;
    }
    return 0;
}

int libmdf_bridge_render(int format,
                         const char *data,
                         size_t len,
                         int chunk,
                         int width,
                         const char *theme,
                         int boring,
                         int osc8,
                         int table_buffer,
                         int table_wire,
                         int trace,
                         char **out,
                         size_t *out_len,
                         char **trace_out,
                         size_t *trace_len)
{
    mdf *renderer;
    mdf_options opts;
    mdf_source source;
    mdf_sink sink;
    bridge_source src_data;
    bridge_sink sink_data;
    mdf_status st;

    *out = NULL;
    *out_len = 0;
    *trace_out = NULL;
    *trace_len = 0;
    if (data == NULL && len != 0) {
        return MDF_ERROR_INVALID;
    }
    memset(&src_data, 0, sizeof(src_data));
    memset(&sink_data, 0, sizeof(sink_data));
    src_data.data = data;
    src_data.len = len;
    src_data.chunk = chunk <= 0 ? 4096u : (size_t)chunk;
    sink_data.trace_enabled = trace;

    mdf_options_init(&opts);
    opts.width = width;
    opts.theme_name = theme == NULL || theme[0] == '\0' ? "default" : theme;
    opts.boring = boring;
    opts.osc8 = osc8;
    opts.table_buffer_mode = (mdf_table_buffer_mode)table_buffer;
    opts.table_wire_mode = (mdf_table_wire_mode)table_wire;
    if (trace) {
        opts.write_trace.userdata = &sink_data;
        opts.write_trace.emit = bridge_trace_emit;
    }
    st = mdf_create((mdf_format)format, &opts, &renderer);
    if (st != MDF_OK) {
        return (int)st;
    }
    source.userdata = &src_data;
    source.read = bridge_read;
    sink.userdata = &sink_data;
    sink.write = bridge_write;
    st = renderer->render(renderer, &source, &sink);
    renderer->destroy(renderer);
    if (st != MDF_OK || sink_data.failed) {
        free(sink_data.out.buf);
        free(sink_data.trace.buf);
        return st == MDF_OK ? MDF_ERROR_IO : (int)st;
    }
    *out = sink_data.out.buf;
    *out_len = sink_data.out.len;
    *trace_out = sink_data.trace.buf;
    *trace_len = sink_data.trace.len;
    return MDF_OK;
}

void libmdf_bridge_free(void *ptr)
{
    free(ptr);
}
