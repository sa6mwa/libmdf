#include "render_internal.h"

#include <limits.h>

static void html_state_reset_style(html_state *state);
static void html_state_parse_sgr(html_state *state, const char *buf, size_t len);
static const char *html_rgb_for_fg(mdf_impl *impl, int fg, int bold, char *buf, size_t buf_len);

#define MDF_HTML_DEFAULT_FONT_STACK "ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,\"Liberation Mono\",\"Courier New\",monospace"
#define MDF_HTML_BG_CELL_STYLE "display:inline-block;height:1lh;line-height:1lh;vertical-align:top;"

static int html_escape(mdf_sink *sink, const char *s, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        switch (s[i]) {
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
            if (((unsigned char)s[i]) < 0x80) {
                if (mdf_write_all(sink, s + i, 1) != 0) return -1;
            } else {
                unsigned long cp;
                size_t adv;

                adv = utf8_decode_codepoint(s + i, len - i, &cp);
                (void)cp;
                if (adv == 0) {
                    adv = 1;
                }
                if (mdf_write_all(sink, s + i, adv) != 0) return -1;
                i += adv - 1;
            }
            break;
        }
    }
    return 0;
}

static int html_escape_attr(mdf_sink *sink, const char *s, size_t len)
{
    return html_escape(sink, s, len);
}

static int html_is_full_block_at(const char *s, size_t len, size_t off)
{
    return off + 3 <= len &&
           (unsigned char)s[off] == 0xe2 &&
           (unsigned char)s[off + 1] == 0x96 &&
           (unsigned char)s[off + 2] == 0x88;
}

static const char *html_chart_box_glyph_style(const char *s, size_t len, size_t off)
{
    if (off + 3 > len || (unsigned char)s[off] != 0xe2 || (unsigned char)s[off + 1] != 0x94) {
        return NULL;
    }
    switch ((unsigned char)s[off + 2]) {
    case 0x80: /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL */
        return "background:linear-gradient(to bottom,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em));";
    case 0x82: /* U+2502 BOX DRAWINGS LIGHT VERTICAL */
        return "background:linear-gradient(to right,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em));";
    case 0x94: /* U+2514 BOX DRAWINGS LIGHT UP AND RIGHT */
        return "background:linear-gradient(to right,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em)) top left/100% 50% no-repeat,linear-gradient(to bottom,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em)) center right/50% 100% no-repeat;";
    case 0xa4: /* U+2524 BOX DRAWINGS LIGHT VERTICAL AND LEFT */
        return "background:linear-gradient(to right,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em)),linear-gradient(to bottom,transparent calc(50% - .045em),currentColor calc(50% - .045em),currentColor calc(50% + .045em),transparent calc(50% + .045em)) center left/50% 100% no-repeat;";
    default:
        return NULL;
    }
}

static int html_write_chart_box_glyph(mdf_sink *sink, const char *glyph, const char *style)
{
    if (mdf_write_cstr(sink, "<span style=\"display:inline-block;width:1ch;height:1lh;line-height:1lh;vertical-align:top;color:inherit;-webkit-text-fill-color:transparent;") != 0) return -1;
    if (mdf_write_cstr(sink, style) != 0) return -1;
    if (mdf_write_cstr(sink, "\">") != 0) return -1;
    if (mdf_write_all(sink, glyph, 3) != 0) return -1;
    return mdf_write_cstr(sink, "</span>");
}

static int html_write_solid_cells(mdf_impl *impl, mdf_sink *sink, int fg, int bold, size_t cells)
{
    char rgb[32];
    const char *color;
    size_t i;

    color = html_rgb_for_fg(impl, fg, bold, rgb, sizeof(rgb));
    if (mdf_write_cstr(sink, "<span style=\"display:inline-block;height:1lh;line-height:1lh;vertical-align:top;white-space:pre;color:transparent;background-color:rgb(") != 0) return -1;
    if (mdf_write_cstr(sink, color) != 0) return -1;
    if (mdf_write_cstr(sink, ");\">") != 0) return -1;
    for (i = 0; i < cells; i++) {
        if (mdf_write_cstr(sink, "█") != 0) return -1;
    }
    return mdf_write_cstr(sink, "</span>");
}

static int html_escape_styled_cells(mdf_impl *impl, mdf_sink *sink, const char *s, size_t len, int fg, int bold, int render_cells)
{
    size_t start;
    size_t i;

    if (!render_cells) {
        return html_escape(sink, s, len);
    }
    start = 0;
    i = 0;
    while (i < len) {
        if (html_is_full_block_at(s, len, i)) {
            size_t cells;

            if (i > start && html_escape(sink, s + start, i - start) != 0) return -1;
            cells = 0;
            while (html_is_full_block_at(s, len, i)) {
                cells++;
                i += 3;
            }
            if (html_write_solid_cells(impl, sink, fg, bold, cells) != 0) return -1;
            start = i;
        } else {
            const char *box_style;

            box_style = html_chart_box_glyph_style(s, len, i);
            if (box_style != NULL) {
                if (i > start && html_escape(sink, s + start, i - start) != 0) return -1;
                if (html_write_chart_box_glyph(sink, s + i, box_style) != 0) return -1;
                i += 3;
                start = i;
            } else {
                i++;
            }
        }
    }
    if (start < len && html_escape(sink, s + start, len - start) != 0) return -1;
    return 0;
}

static int html_ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int html_url_scheme_eq(const char *s, size_t len, const char *scheme)
{
    size_t i;

    for (i = 0; i < len && scheme[i] != '\0'; i++) {
        if (html_ascii_lower((unsigned char)s[i]) != scheme[i]) {
            return 0;
        }
    }
    return i == len && scheme[i] == '\0';
}

static int html_url_scheme_is_blocked(const char *s, size_t len)
{
    return html_url_scheme_eq(s, len, "javascript") ||
           html_url_scheme_eq(s, len, "data") ||
           html_url_scheme_eq(s, len, "vbscript") ||
           html_url_scheme_eq(s, len, "file");
}

static int html_link_url_is_safe(const char *url, size_t len)
{
    size_t i;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c;

        c = (unsigned char)url[i];
        if (c <= ' ' || c == 0x7f) {
            return 0;
        }
        if (url[i] == '/' || url[i] == '?' || url[i] == '#') {
            return 1;
        }
        if (url[i] == ':') {
            size_t j;

            if (i == 0 || !((url[0] >= 'A' && url[0] <= 'Z') || (url[0] >= 'a' && url[0] <= 'z'))) {
                return 0;
            }
            for (j = 1; j < i; j++) {
                if (!((url[j] >= 'A' && url[j] <= 'Z') ||
                      (url[j] >= 'a' && url[j] <= 'z') ||
                      (url[j] >= '0' && url[j] <= '9') ||
                      url[j] == '+' ||
                      url[j] == '-' ||
                      url[j] == '.')) {
                    return 0;
                }
            }
            if (html_url_scheme_is_blocked(url, i)) {
                return 0;
            }
            if (html_url_scheme_eq(url, i, "http") ||
                html_url_scheme_eq(url, i, "https") ||
                html_url_scheme_eq(url, i, "mailto")) {
                return 1;
            }
            return i + 2 < len && url[i + 1] == '/' && url[i + 2] == '/';
        }
    }
    return 1;
}

static void html_state_clear_link(html_state *state)
{
    state->link_len = 0;
    if (state->link != NULL) {
        state->link[0] = '\0';
    }
}

void html_parse_style_prefix(html_state *state, const char *style)
{
    const char *p;
    const char *start;

    html_state_reset_style(state);
    state->boring = 0;
    if (style == NULL) {
        return;
    }
    p = style;
    while (*p != '\0') {
        if (p[0] == '\033' && p[1] == '[') {
            start = p + 2;
            p = start;
            while (*p != '\0' && *p != 'm') {
                p++;
            }
            if (*p == 'm') {
                html_state_parse_sgr(state, start, (size_t)(p - start));
            }
        }
        if (*p != '\0') {
            p++;
        }
    }
}

static int html_style_attrs_equal(const html_state *state, int fg, int bold, int italic, int underline)
{
    return state->fg == fg &&
           state->bold == bold &&
           state->italic == italic &&
           state->underline == underline;
}

static int html_style_heading_level(mdf_impl *impl, int fg, int bold, int italic, int underline)
{
    html_state heading;
    int i;

    for (i = 1; i <= 6; i++) {
        html_parse_style_prefix(&heading, mdf_theme_heading(impl, i));
        if (html_style_attrs_equal(&heading, fg, bold, italic, underline)) {
            return i;
        }
    }
    return 0;
}

static int html_theme_has_code_block_semantics(mdf_impl *impl)
{
    return impl->theme != NULL &&
           (strcmp(impl->theme->name, "ayu-light") == 0 ||
            strcmp(impl->theme->name, "everforest-light") == 0 ||
            strcmp(impl->theme->name, "github-light") == 0 ||
            strcmp(impl->theme->name, "papercolor-light") == 0 ||
            strcmp(impl->theme->name, "rose-pine-dawn") == 0 ||
            strcmp(impl->theme->name, "solarized-dark") == 0 ||
            strcmp(impl->theme->name, "solarized-light") == 0);
}

static int html_code_heading_style_level(mdf_impl *impl, int fg, int bold, int italic, int underline)
{
    html_state code;

    html_parse_style_prefix(&code, mdf_theme_code_inline(impl));
    if (html_style_attrs_equal(&code, fg, bold, italic, underline)) {
        return html_style_heading_level(impl, fg, bold, italic, underline);
    }
    html_parse_style_prefix(&code, mdf_theme_code_block(impl));
    if (html_style_attrs_equal(&code, fg, bold, italic, underline)) {
        return html_style_heading_level(impl, fg, bold, italic, underline);
    }
    return 0;
}

static int html_semantic_heading_level(mdf_impl *impl, int fg, int bold, int italic, int underline, int list_marker)
{
    if (list_marker) {
        return html_style_heading_level(impl, fg, bold, italic, underline);
    }
    return 0;
}

static const char *html_heading_span_size(int level)
{
    /*
     * Standalone libmdf/cmdf HTML intentionally uses a stronger document ATX
     * scale than Go mdf's legacy HTML shell. Deck mode remaps these source
     * sizes with deck-specific viewport CSS; do not shrink this table to match
     * Go parity output.
     */
    static const char *const sizes[] = {
        "",
        "font-size:32pt;",
        "font-size:22pt;",
        "font-size:15pt;",
        "font-size:14pt;",
        "font-size:13pt;",
        "font-size:12.5pt;"
    };

    if (level < 1 || level > 6) {
        return "";
    }
    return sizes[level];
}

static int html_write_base64(mdf_sink *sink, const unsigned char *src, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[4096];
    size_t out_len;
    size_t i;

    out_len = 0;
    for (i = 0; i < len; i += 3) {
        unsigned int v;
        size_t remain;

        remain = len - i;
        v = ((unsigned int)src[i]) << 16;
        if (remain > 1) {
            v |= ((unsigned int)src[i + 1]) << 8;
        }
        if (remain > 2) {
            v |= (unsigned int)src[i + 2];
        }
        out[out_len++] = table[(v >> 18) & 0x3f];
        out[out_len++] = table[(v >> 12) & 0x3f];
        out[out_len++] = remain > 1 ? table[(v >> 6) & 0x3f] : '=';
        out[out_len++] = remain > 2 ? table[v & 0x3f] : '=';
        if (out_len == sizeof(out)) {
            if (mdf_write_all(sink, out, out_len) != 0) {
                return -1;
            }
            out_len = 0;
        }
    }
    if (out_len > 0 && mdf_write_all(sink, out, out_len) != 0) {
        return -1;
    }
    return 0;
}

static int html_write_base64_reader(mdf_sink *sink, const mdf_html_font_face *face)
{
    unsigned char chunk[4096];
    unsigned char carry[3];
    size_t carry_len;
    size_t off;
    size_t n;
    size_t i;
    size_t full_len;
    int err;

    off = 0;
    carry_len = 0;
    for (;;) {
        err = 0;
        n = face->read(face->userdata, off, chunk, sizeof(chunk), &err);
        if (n == 0) {
            if (err != 0) {
                return -1;
            }
            return carry_len == 0 ? 0 : html_write_base64(sink, carry, carry_len);
        }
        i = 0;
        if (carry_len > 0) {
            while (carry_len < 3 && i < n) {
                carry[carry_len++] = chunk[i++];
            }
            if (carry_len == 3) {
                if (html_write_base64(sink, carry, 3) != 0) {
                    return -1;
                }
                carry_len = 0;
            }
        }
        full_len = ((n - i) / 3) * 3;
        if (full_len > 0) {
            if (html_write_base64(sink, chunk + i, full_len) != 0) {
                return -1;
            }
            i += full_len;
        }
        while (i < n) {
            carry[carry_len++] = chunk[i++];
        }
        off += n;
    }
}

static const char *html_font_mime(int format)
{
    if (format == MDF_HTML_FONT_FORMAT_WOFF2) {
        return "font/woff2";
    }
    if (format == MDF_HTML_FONT_FORMAT_TTF) {
        return "font/ttf";
    }
    return NULL;
}

static const char *html_font_css_format(int format)
{
    if (format == MDF_HTML_FONT_FORMAT_WOFF2) {
        return "woff2";
    }
    if (format == MDF_HTML_FONT_FORMAT_TTF) {
        return "truetype";
    }
    return NULL;
}

static int html_write_css_quoted(mdf_sink *sink, const char *s)
{
    const char *p;
    char esc[4];
    static const char hex[] = "0123456789abcdef";

    if (mdf_write_cstr(sink, "\"") != 0) return -1;
    p = s;
    while (p != NULL && *p != '\0') {
        if (*p == '\\' || *p == '"') {
            esc[0] = '\\';
            esc[1] = *p;
            if (mdf_write_all(sink, esc, 2) != 0) return -1;
        } else if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f || *p == '<') {
            esc[0] = '\\';
            esc[1] = hex[((unsigned char)*p >> 4) & 15];
            esc[2] = hex[(unsigned char)*p & 15];
            esc[3] = ' ';
            if (mdf_write_all(sink, esc, 4) != 0) return -1;
        } else {
            if (mdf_write_all(sink, p, 1) != 0) return -1;
        }
        p++;
    }
    return mdf_write_cstr(sink, "\"");
}

static int html_write_font_face(mdf_sink *sink,
                                const char *family,
                                const mdf_html_font_face *face,
                                const char *uri,
                                const char *weight,
                                const char *style)
{
    const char *mime;
    const char *css_format;

    mime = html_font_mime(face->format);
    css_format = html_font_css_format(face->format);
    if (mime == NULL || css_format == NULL) {
        return 0;
    }
    if (uri == NULL && (face->data == NULL || face->data_len == 0) && face->read == NULL) {
        return 0;
    }
    if (mdf_write_cstr(sink, "@font-face{font-family:") != 0) return -1;
    if (html_write_css_quoted(sink, family) != 0) return -1;
    if (mdf_write_cstr(sink, ";src:url(") != 0) return -1;
    if (uri != NULL) {
        if (html_write_css_quoted(sink, uri) != 0) return -1;
    } else {
        if (mdf_write_cstr(sink, "data:") != 0) return -1;
        if (mdf_write_cstr(sink, mime) != 0) return -1;
        if (mdf_write_cstr(sink, ";base64,") != 0) return -1;
        if (face->data != NULL && face->data_len > 0) {
            if (html_write_base64(sink, face->data, face->data_len) != 0) return -1;
        } else if (face->read != NULL) {
            if (html_write_base64_reader(sink, face) != 0) return -1;
        }
    }
    if (mdf_write_cstr(sink, ") format('") != 0) return -1;
    if (mdf_write_cstr(sink, css_format) != 0) return -1;
    if (mdf_write_cstr(sink, "');font-weight:") != 0) return -1;
    if (mdf_write_cstr(sink, weight) != 0) return -1;
    if (mdf_write_cstr(sink, ";font-style:") != 0) return -1;
    if (mdf_write_cstr(sink, style) != 0) return -1;
    if (mdf_write_cstr(sink, ";font-display:block;}\n") != 0) return -1;
    return 0;
}

static int html_write_default_font_faces(mdf_impl *impl, mdf_sink *sink)
{
    const mdf_html_font *font;

    font = &impl->opts.html_font;
    if (font->family == NULL || font->family[0] == '\0' ||
        font->regular.format == MDF_HTML_FONT_FORMAT_NONE) {
        return 0;
    }
    if (html_write_font_face(sink, font->family, &font->regular,
                             impl->html_font_regular_uri, "400", "normal") != 0) return -1;
    if (html_write_font_face(sink, font->family, &font->regular,
                             impl->html_font_regular_uri, "700", "normal") != 0) return -1;
    if (font->italic.format != MDF_HTML_FONT_FORMAT_NONE) {
        if (html_write_font_face(sink, font->family, &font->italic,
                                 impl->html_font_italic_uri, "400", "italic") != 0) return -1;
        if (html_write_font_face(sink, font->family, &font->italic,
                                 impl->html_font_italic_uri, "700", "italic") != 0) return -1;
    }
    return 0;
}

int html_write_default_css(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;
    char buf[1024];
    char rgb_buf[32];
    html_state table_header_state;
    const char *bg;
    const char *text;
    const char *table_header_rgb;
    const char *table_wire;

    impl = (mdf_impl *)self->impl;
    bg = impl->opts.boring ? "transparent" : "rgb(0,0,0)";
    text = impl->opts.boring ? "rgb(0,0,0)" : "rgb(220,220,220)";
    table_wire = impl->opts.boring ? "rgb(0,0,0)" : "rgb(127,127,127)";
    html_parse_style_prefix(&table_header_state, mdf_theme_table_header(impl));
    table_header_rgb = html_rgb_for_fg(impl,
                                       table_header_state.fg,
                                       table_header_state.bold,
                                       rgb_buf,
                                       sizeof(rgb_buf));
    if (html_write_default_font_faces(impl, sink) != 0) return -1;
    if (mdf_write_cstr(sink, ":root{color-scheme:dark light;}\n") != 0) return -1;
    snprintf(buf, sizeof(buf), "html,body{margin:0;min-height:100%%;background:%s;}\n", bg);
    if (mdf_write_cstr(sink, buf) != 0) return -1;
    snprintf(buf, sizeof(buf), "body{color:%s;font-family:", text);
    if (mdf_write_cstr(sink, buf) != 0) return -1;
    if (impl->opts.html_font.family != NULL && impl->opts.html_font.family[0] != '\0') {
        if (html_write_css_quoted(sink, impl->opts.html_font.family) != 0) return -1;
        if (mdf_write_cstr(sink, ",monospace;font-size:12pt;line-height:1.4;}\n") != 0) return -1;
    } else {
        if (mdf_write_cstr(sink, MDF_HTML_DEFAULT_FONT_STACK ";font-size:12pt;line-height:1.4;}\n") != 0) return -1;
    }
    snprintf(buf, sizeof(buf), ".mdf-document{--mdf-content-max-width:%.0fch;--mdf-page-padding-block:36pt;--mdf-page-padding-inline:36pt;box-sizing:border-box;min-height:100vh;width:min(100%%,var(--mdf-content-max-width));margin-inline:auto;padding-block:var(--mdf-page-padding-block);padding-inline:clamp(1rem,4vw,var(--mdf-page-padding-inline));white-space:pre-wrap;overflow-wrap:anywhere;tab-size:4;}\n", impl->opts.html_content_width_ch);
    if (mdf_write_cstr(sink, buf) != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-document a{color:inherit;text-decoration:none;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-document a:hover{text-decoration:underline;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-heading{display:inline-block;box-sizing:border-box;max-width:100%;white-space:normal;overflow-wrap:anywhere;padding-left:var(--mdf-heading-indent);text-indent:calc(-1 * var(--mdf-heading-indent));vertical-align:top;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-line{display:inline-grid;grid-template-columns:max-content minmax(0,1fr);max-width:100%;white-space:normal;overflow-wrap:anywhere;vertical-align:top;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-prefix{grid-column:1;white-space:pre;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-content{grid-column:2;min-width:0;white-space:pre-wrap;overflow-wrap:anywhere;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-block{display:grid;grid-template-columns:max-content max-content;column-gap:0;align-items:start;margin:1em 0;clear:both;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-block .mdf-table{margin:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-corner-image{float:right;max-width:96pt;max-height:96pt;margin-left:8pt;margin-bottom:8pt;object-fit:contain;}\n") != 0) return -1;
    snprintf(buf, sizeof(buf), ".mdf-table{--mdf-table-wire:%s;white-space:normal;border-collapse:collapse;margin:1em 0;clear:both;}\n", table_wire);
    if (mdf_write_cstr(sink, buf) != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table th,.mdf-table td{vertical-align:top;overflow-wrap:anywhere;white-space:pre-wrap;}\n") != 0) return -1;
    snprintf(buf, sizeof(buf), ".mdf-table th{color:rgb(%s);font-weight:700;overflow-wrap:normal;word-break:normal;}\n", table_header_rgb);
    if (mdf_write_cstr(sink, buf) != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-bordered th,.mdf-table-bordered td{border:1px solid var(--mdf-table-wire);padding:.2em 1ch;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-space{border-collapse:collapse;border-spacing:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-space th,.mdf-table-space td{border:0;padding:0 1ch;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-space th:first-child,.mdf-table-space td:first-child{padding-left:0;}\n") != 0) return -1;
    if (mdf_write_cstr(sink, ".mdf-table-space th:last-child,.mdf-table-space td:last-child{padding-right:0;}\n") != 0) return -1;
    return 0;
}

int html_start(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;
    const char *title;

    impl = (mdf_impl *)self->impl;
    if (impl->html_open) {
        return 0;
    }
    impl->html_open = 1;
    if (impl->html_fragment) {
        return 0;
    }
    if (mdf_write_cstr(sink, "<!doctype html>\n<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n") != 0 ||
        mdf_write_cstr(sink, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>") != 0) {
        return -1;
    }
    title = impl->html_title != NULL ? impl->html_title : "mdf";
    if (html_escape(sink, title, strlen(title)) != 0 ||
        mdf_write_cstr(sink, "</title>\n<style>\n") != 0 ||
        html_write_default_css(self, sink) != 0 ||
        mdf_write_cstr(sink, "</style>\n</head>\n<body>\n<main class=\"mdf-document\">") != 0) {
        return -1;
    }
    return 0;
}

typedef struct html_bridge_scope {
    mdf_impl *impl;
    html_bridge_sink bridge;
    mdf_sink sink;
    int save_width;
    int save_osc8;
    int save_boring;
    int save_chart_suppress_centering;
} html_bridge_scope;

html_state *html_state_get(mdf_impl *impl)
{
    html_state *state;

    if (impl->html_state != NULL) {
        return (html_state *)impl->html_state;
    }
    state = (html_state *)mdf_alloc(&impl->allocator, sizeof(*state));
    if (state == NULL) {
        mdf_impl_mark_oom(impl);
        return NULL;
    }
    memset(state, 0, sizeof(*state));
    state->fg = -1;
    state->bg = -1;
    state->stream_bg = -1;
    impl->html_state = state;
    return state;
}

static int html_buf_append(mdf_allocator *allocator, char **buf, size_t *len, size_t *cap, const char *src, size_t src_len)
{
    char *next;
    size_t new_cap;
    size_t need;

    if (src_len > ((size_t)-1) - *len - 1) {
        return -1;
    }
    need = *len + src_len + 1;
    if (need > *cap) {
        new_cap = *cap == 0 ? 64 : *cap;
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
    memcpy(*buf + *len, src, src_len);
    *len += src_len;
    (*buf)[*len] = '\0';
    return 0;
}

static int html_buf_assign(mdf_allocator *allocator, char **buf, size_t *len, size_t *cap, const char *src, size_t src_len)
{
    *len = 0;
    if (*buf != NULL) {
        (*buf)[0] = '\0';
    }
    if (src == NULL || src_len == 0) {
        return 0;
    }
    return html_buf_append(allocator, buf, len, cap, src, src_len);
}

int html_buf_sink_write(void *userdata, const char *src, size_t len)
{
    html_buf_sink *buf;

    buf = (html_buf_sink *)userdata;
    return html_buf_append(buf->allocator, &buf->buf, &buf->len, &buf->cap, src, len);
}

static size_t html_heading_marker_len(const char *s, size_t len, int heading_level)
{
    size_t i;

    if (heading_level <= 0) {
        return 0;
    }
    if (len < (size_t)heading_level + 1) {
        return 0;
    }
    for (i = 0; i < (size_t)heading_level; i++) {
        if (s[i] != '#') {
            return 0;
        }
    }
    if (s[heading_level] != ' ') {
        return 0;
    }
    return (size_t)heading_level + 1;
}

int html_direct_close_prefix_line(mdf_sink *sink, int final_line)
{
    return mdf_write_cstr(sink, final_line ? "</span>\n" : "</span></span></span>\n");
}

static int discard_write(void *userdata, const char *src, size_t len)
{
    (void)userdata;
    (void)src;
    (void)len;
    return 0;
}

static void html_segment_free(mdf_allocator *allocator, html_segment *seg)
{
    if (seg == NULL) {
        return;
    }
    mdf_free_mem(allocator, seg->text, seg->cap);
    mdf_free_mem(allocator, seg->link, seg->link_cap);
    memset(seg, 0, sizeof(*seg));
    seg->fg = -1;
    seg->bg = -1;
}

static int html_segment_same(const html_segment *seg, const html_state *state)
{
    if (seg->bold != state->bold ||
        seg->italic != state->italic ||
        seg->underline != state->underline ||
        seg->fg != state->fg ||
        seg->bg != state->bg) {
        return 0;
    }
    if (seg->link_len != state->link_len) {
        return 0;
    }
    if (seg->link_len == 0) {
        return 1;
    }
    return memcmp(seg->link, state->link, state->link_len) == 0;
}

static int html_state_push_segment(mdf_impl *impl, html_state *state)
{
    html_segment *next;
    size_t new_cap;
    html_segment *seg;

    if (state->segment_count == state->segment_cap) {
        new_cap = state->segment_cap == 0 ? 8 : state->segment_cap * 2;
        next = (html_segment *)mdf_realloc_mem(&impl->allocator,
                                               state->segments,
                                               state->segment_cap * sizeof(state->segments[0]),
                                               new_cap * sizeof(state->segments[0]));
        if (next == NULL) {
            return -1;
        }
        memset(next + state->segment_cap, 0, (new_cap - state->segment_cap) * sizeof(next[0]));
        state->segments = next;
        state->segment_cap = new_cap;
    }
    seg = &state->segments[state->segment_count++];
    memset(seg, 0, sizeof(*seg));
    seg->bold = state->bold;
    seg->italic = state->italic;
    seg->underline = state->underline;
    seg->fg = state->fg;
    seg->bg = state->bg;
    if (state->link_len > 0) {
        if (html_buf_assign(&impl->allocator, &seg->link, &seg->link_len, &seg->link_cap, state->link, state->link_len) != 0) {
            return -1;
        }
    }
    return 0;
}

static int html_state_append_text(mdf_impl *impl, html_state *state, const char *src, size_t src_len)
{
    html_segment *seg;

    if (src_len == 0) {
        return 0;
    }
    if (html_buf_append(&impl->allocator, &state->line_plain, &state->line_plain_len, &state->line_plain_cap, src, src_len) != 0) {
        return -1;
    }
    if (state->segment_count == 0 || !html_segment_same(&state->segments[state->segment_count - 1], state)) {
        if (html_state_push_segment(impl, state) != 0) {
            return -1;
        }
    }
    seg = &state->segments[state->segment_count - 1];
    return html_buf_append(&impl->allocator, &seg->text, &seg->len, &seg->cap, src, src_len);
}

static void html_state_clear_line(mdf_impl *impl, html_state *state)
{
    size_t i;

    for (i = 0; i < state->segment_count; i++) {
        html_segment_free(&impl->allocator, &state->segments[i]);
    }
    state->segment_count = 0;
    state->line_plain_len = 0;
    if (state->line_plain != NULL) {
        state->line_plain[0] = '\0';
    }
}

static void html_state_reset_stream_line(html_state *state)
{
    state->stream_mode = 0;
    state->stream_heading_level = 0;
    state->stream_span_open = 0;
    state->stream_link_open = 0;
    state->stream_bold = 0;
    state->stream_italic = 0;
    state->stream_underline = 0;
    state->stream_fg = -1;
    state->stream_bg = -1;
    state->stream_link_len = 0;
    state->stream_utf8_len = 0;
    state->stream_utf8_need = 0;
    if (state->stream_link != NULL) {
        state->stream_link[0] = '\0';
    }
}

static void html_state_reset_style(html_state *state)
{
    state->bold = 0;
    state->italic = 0;
    state->underline = 0;
    state->fg = -1;
    state->bg = -1;
}

static int html_sgr_next(const char *buf, size_t len, size_t *off, long *value)
{
    long parsed;
    size_t start;

    if (*off > len) {
        return 0;
    }
    start = *off;
    parsed = 0;
    while (*off < len && buf[*off] != ';') {
        if (buf[*off] < '0' || buf[*off] > '9') {
            return 0;
        }
        if (parsed > (LONG_MAX - (long)(buf[*off] - '0')) / 10) {
            return 0;
        }
        parsed = parsed * 10 + (long)(buf[*off] - '0');
        (*off)++;
    }
    if (*off < len) {
        (*off)++;
    } else {
        *off = len + 1;
    }
    *value = *off == start ? 0 : parsed;
    return 1;
}

static void html_state_parse_sgr(html_state *state, const char *buf, size_t len)
{
    long value;
    long mode;
    long color;
    size_t off;

    if (len == 0) {
        html_state_reset_style(state);
        return;
    }
    if (len > 63) {
        len = 63;
    }
    off = 0;
    while (html_sgr_next(buf, len, &off, &value)) {
        if (value == 0) {
            html_state_reset_style(state);
        } else if (value == 1) {
            state->bold = 1;
        } else if (value == 3) {
            state->italic = 1;
        } else if (value == 4) {
            state->underline = 1;
        } else if (value == 22) {
            state->bold = 0;
        } else if (value == 23) {
            state->italic = 0;
        } else if (value == 24) {
            state->underline = 0;
        } else if ((value >= 30 && value <= 37) || (value >= 90 && value <= 97)) {
            state->fg = (int)value;
        } else if (value >= 40 && value <= 47) {
            state->bg = (int)(value - 10);
        } else if (value >= 100 && value <= 107) {
            state->bg = (int)(value - 10);
        } else if (value == 38) {
            if (html_sgr_next(buf, len, &off, &mode) &&
                mode == 5 &&
                html_sgr_next(buf, len, &off, &color) &&
                color >= 0 && color <= 255) {
                state->fg = 1000 + (int)color;
            }
        } else if (value == 48) {
            if (html_sgr_next(buf, len, &off, &mode) &&
                mode == 5 &&
                html_sgr_next(buf, len, &off, &color) &&
                color >= 0 && color <= 255) {
                state->bg = 1000 + (int)color;
            }
        } else if (value == 39) {
            state->fg = -1;
        } else if (value == 49) {
            state->bg = -1;
        }
    }
}

static void html_state_parse_osc(mdf_impl *impl, html_state *state)
{
    if (state->osc_len >= 3 &&
        state->osc_buf[0] == '8' &&
        state->osc_buf[1] == ';' &&
        state->osc_buf[2] == ';') {
        if (state->osc_len == 3 ||
            !html_link_url_is_safe(state->osc_buf + 3, state->osc_len - 3)) {
            html_state_clear_link(state);
            state->osc_len = 0;
            if (state->osc_buf != NULL) {
                state->osc_buf[0] = '\0';
            }
            return;
        }
        if (html_buf_assign(&impl->allocator,
                            &state->link,
                            &state->link_len,
                            &state->link_cap,
                            state->osc_buf + 3,
                            state->osc_len - 3) != 0) {
            return;
        }
    }
    state->osc_len = 0;
    if (state->osc_buf != NULL) {
        state->osc_buf[0] = '\0';
    }
}

static int html_stream_same_link_buf(const char *a, size_t a_len, const char *b, size_t b_len)
{
    if (a_len != b_len) {
        return 0;
    }
    if (a_len == 0) {
        return 1;
    }
    return memcmp(a, b, a_len) == 0;
}

static const char *html_xterm_rgb(int index, char *buf, size_t buf_len)
{
    static const int levels[] = {0, 95, 135, 175, 215, 255};
    int n;
    int r;
    int g;
    int b;

    if (index < 16) {
        static const char *basic[] = {
            "0,0,0", "205,0,0", "0,205,0", "205,205,0",
            "59,156,255", "205,0,205", "0,205,205", "220,220,220",
            "127,127,127", "255,0,0", "0,255,0", "255,255,0",
            "59,156,255", "255,0,255", "0,255,255", "255,255,255"
        };
        return basic[index];
    }
    if (index >= 16 && index <= 231) {
        n = index - 16;
        r = levels[n / 36];
        g = levels[(n / 6) % 6];
        b = levels[n % 6];
        snprintf(buf, buf_len, "%d,%d,%d", r, g, b);
        return buf;
    }
    if (index >= 232 && index <= 255) {
        n = 8 + ((index - 232) * 10);
        snprintf(buf, buf_len, "%d,%d,%d", n, n, n);
        return buf;
    }
    return "220,220,220";
}

static const char *html_rgb_for_fg(mdf_impl *impl, int fg, int bold, char *buf, size_t buf_len)
{
    if (impl->opts.boring) {
        return "0,0,0";
    }
    if (fg >= 1000 && fg <= 1255) {
        return html_xterm_rgb(fg - 1000, buf, buf_len);
    }
    switch (fg) {
    case 30:
        return "0,0,0";
    case 31:
        return "205,0,0";
    case 32:
        return "0,205,0";
    case 33:
        return "205,205,0";
    case 34:
        return "59,156,255";
    case 35:
        return "205,0,205";
    case 36:
        return "0,205,205";
    case 37:
        return bold ? "229,229,229" : "220,220,220";
    case 90:
        return "127,127,127";
    case 91:
        return "255,0,0";
    case 92:
        return "0,255,0";
    case 93:
        return "255,255,0";
    case 94:
        return "59,156,255";
    case 95:
        return "255,0,255";
    case 96:
        return "0,255,255";
    case 97:
        return "255,255,255";
    default:
        return "220,220,220";
    }
}

static const char *html_fg_rgb(mdf_impl *impl, const html_segment *seg, char *buf, size_t buf_len)
{
    return html_rgb_for_fg(impl, seg->fg, seg->bold, buf, buf_len);
}

static const char *html_bg_rgb(mdf_impl *impl, const html_segment *seg, char *buf, size_t buf_len)
{
    return html_rgb_for_fg(impl, seg->bg, 0, buf, buf_len);
}

const char *html_theme_quote_rgb(mdf_impl *impl, char *buf, size_t buf_len)
{
    html_state quote;

    html_parse_style_prefix(&quote, mdf_theme_quote(impl));
    return html_rgb_for_fg(impl, quote.fg, quote.bold, buf, buf_len);
}

const char *html_theme_heading_rgb(mdf_impl *impl, int level, char *buf, size_t buf_len)
{
    html_state heading;

    html_parse_style_prefix(&heading, mdf_theme_heading(impl, level));
    return html_rgb_for_fg(impl, heading.fg, heading.bold, buf, buf_len);
}

int html_write_style(mdf_impl *impl, mdf_sink *sink, const html_segment *seg, int list_marker)
{
    char buf[320];
    char rgb_buf[32];
    char bg_buf[32];
    int heading_level;
    int marker_heading_level;
    int body_sized_heading_style;
    size_t len;

    len = 0;
    heading_level = list_marker ? 0 : html_semantic_heading_level(impl, seg->fg, seg->bold, seg->italic, seg->underline, 0);
    marker_heading_level = list_marker ? html_style_heading_level(impl, seg->fg, seg->bold, seg->italic, seg->underline) : 0;
    body_sized_heading_style = marker_heading_level;
    if (body_sized_heading_style == 0 && !list_marker) {
        body_sized_heading_style = html_code_heading_style_level(impl, seg->fg, seg->bold, seg->italic, seg->underline);
    }
    if (snprintf(buf, sizeof(buf), "color:rgb(%s);", html_fg_rgb(impl, seg, rgb_buf, sizeof(rgb_buf))) < 0) return -1;
    len = strlen(buf);
    if (seg->bg != -1) {
        const char *bg;

        bg = html_bg_rgb(impl, seg, bg_buf, sizeof(bg_buf));
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "background-color:rgb(%s);", bg);
        if (len >= sizeof(buf)) return -1;
        if (len + strlen(MDF_HTML_BG_CELL_STYLE) >= sizeof(buf)) return -1;
        memcpy(buf + len, MDF_HTML_BG_CELL_STYLE, strlen(MDF_HTML_BG_CELL_STYLE));
        len += strlen(MDF_HTML_BG_CELL_STYLE);
    }
    if (heading_level > 0) {
        const char *size_css;
        size_css = html_heading_span_size(heading_level);
        if (len + strlen(size_css) >= sizeof(buf)) return -1;
        memcpy(buf + len, size_css, strlen(size_css));
        len += strlen(size_css);
    } else if (body_sized_heading_style > 0) {
        if (len + strlen("font-size:12pt;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-size:12pt;", strlen("font-size:12pt;"));
        len += strlen("font-size:12pt;");
    }
    if (seg->bold || heading_level > 0 || body_sized_heading_style > 0) {
        if (len + strlen("font-weight:700;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-weight:700;", strlen("font-weight:700;"));
        len += strlen("font-weight:700;");
    }
    if (seg->italic) {
        if (len + strlen("font-style:italic;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-style:italic;", strlen("font-style:italic;"));
        len += strlen("font-style:italic;");
    }
    if (seg->underline) {
        if (len + strlen("text-decoration:underline;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "text-decoration:underline;", strlen("text-decoration:underline;"));
        len += strlen("text-decoration:underline;");
    }
    return mdf_write_all(sink, buf, len);
}

static int html_write_style_effective(mdf_impl *impl, mdf_sink *sink, int fg, int bg, int bold, int italic, int underline, int list_marker)
{
    char buf[320];
    char rgb_buf[32];
    char bg_buf[32];
    const char *rgb;
    int heading_level;
    int marker_heading_level;
    int body_sized_heading_style;
    size_t len;

    rgb = html_rgb_for_fg(impl, fg, bold, rgb_buf, sizeof(rgb_buf));
    heading_level = list_marker ? 0 : html_semantic_heading_level(impl, fg, bold, italic, underline, 0);
    marker_heading_level = list_marker ? html_style_heading_level(impl, fg, bold, italic, underline) : 0;
    body_sized_heading_style = marker_heading_level;
    if (body_sized_heading_style == 0 && !list_marker) {
        body_sized_heading_style = html_code_heading_style_level(impl, fg, bold, italic, underline);
    }
    if (snprintf(buf, sizeof(buf), "color:rgb(%s);", rgb) < 0) return -1;
    len = strlen(buf);
    if (bg != -1) {
        const char *bg_rgb;

        bg_rgb = html_rgb_for_fg(impl, bg, 0, bg_buf, sizeof(bg_buf));
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "background-color:rgb(%s);", bg_rgb);
        if (len >= sizeof(buf)) return -1;
        if (len + strlen(MDF_HTML_BG_CELL_STYLE) >= sizeof(buf)) return -1;
        memcpy(buf + len, MDF_HTML_BG_CELL_STYLE, strlen(MDF_HTML_BG_CELL_STYLE));
        len += strlen(MDF_HTML_BG_CELL_STYLE);
    }
    if (heading_level > 0) {
        const char *size_css;
        size_css = html_heading_span_size(heading_level);
        if (len + strlen(size_css) >= sizeof(buf)) return -1;
        memcpy(buf + len, size_css, strlen(size_css));
        len += strlen(size_css);
    } else if (body_sized_heading_style > 0) {
        if (len + strlen("font-size:12pt;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-size:12pt;", strlen("font-size:12pt;"));
        len += strlen("font-size:12pt;");
    }
    if (bold || heading_level > 0 || body_sized_heading_style > 0) {
        if (len + strlen("font-weight:700;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-weight:700;", strlen("font-weight:700;"));
        len += strlen("font-weight:700;");
    }
    if (italic) {
        if (len + strlen("font-style:italic;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "font-style:italic;", strlen("font-style:italic;"));
        len += strlen("font-style:italic;");
    }
    if (underline) {
        if (len + strlen("text-decoration:underline;") >= sizeof(buf)) return -1;
        memcpy(buf + len, "text-decoration:underline;", strlen("text-decoration:underline;"));
        len += strlen("text-decoration:underline;");
    }
    return mdf_write_all(sink, buf, len);
}

static int html_stream_plain_ensure_style(mdf_impl *impl,
                                          html_state *state,
                                          mdf_sink *sink,
                                          int fg,
                                          int bg,
                                          int bold,
                                          int italic,
                                          int underline,
                                          const char *link,
                                          size_t link_len)
{
    int same;
    int old_level;
    int new_level;
    int old_bold_effect;
    int new_bold_effect;

    old_level = html_semantic_heading_level(impl,
                                            state->stream_fg,
                                            state->stream_bold,
                                            state->stream_italic,
                                            state->stream_underline,
                                            0);
    new_level = html_semantic_heading_level(impl, fg, bold, italic, underline, 0);
    old_bold_effect = state->stream_bold || old_level > 0;
    new_bold_effect = bold || new_level > 0;
    same = state->stream_span_open &&
           state->stream_italic == italic &&
           state->stream_underline == underline &&
           state->stream_bg == bg &&
           old_level == new_level &&
           old_bold_effect == new_bold_effect &&
           html_stream_same_link_buf(state->stream_link, state->stream_link_len, link, link_len);
    if (!impl->opts.boring) {
        same = same &&
               state->stream_fg == fg &&
               state->stream_bold == bold;
    }
    if (same) {
        return 0;
    }
    if (state->stream_span_open) {
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        state->stream_span_open = 0;
    }
    if (state->stream_link_open) {
        if (mdf_write_cstr(sink, "</a>") != 0) return -1;
        state->stream_link_open = 0;
    }
    if (link_len > 0) {
        if (html_write_open_link_grouped(impl, sink, link, link_len) != 0) return -1;
        state->stream_link_open = 1;
    }
    {
        if (mdf_write_cstr(sink, "<span style=\"") != 0 ||
            html_write_style_effective(impl, sink, fg, bg, bold, italic, underline, 0) != 0 ||
            mdf_write_cstr(sink, "\">") != 0) {
            return -1;
        }
    }
    state->stream_span_open = 1;
    state->stream_fg = fg;
    state->stream_bg = bg;
    state->stream_bold = bold;
    state->stream_italic = italic;
    state->stream_underline = underline;
    if (html_buf_assign(&impl->allocator, &state->stream_link, &state->stream_link_len, &state->stream_link_cap, link, link_len) != 0) {
        return -1;
    }
    return 0;
}

static int html_stream_emit_buffered_plain(mdf_impl *impl, html_state *state, mdf_sink *sink)
{
    size_t i;

    for (i = 0; i < state->segment_count; i++) {
        html_segment *seg;

        seg = &state->segments[i];
        if (html_stream_plain_ensure_style(impl,
                                           state,
                                           sink,
                                           seg->fg,
                                           seg->bg,
                                           seg->bold,
                                           seg->italic,
                                           seg->underline,
                                           seg->link,
                                           seg->link_len) != 0) {
            return -1;
        }
        if (html_escape_styled_cells(impl,
                                     sink,
                                     seg->text,
                                     seg->len,
                                     seg->fg,
                                     seg->bold,
                                     0) != 0) {
            return -1;
        }
    }
    return 0;
}

static size_t html_utf8_expected_len(unsigned char c)
{
    if ((c & 0x80) == 0) {
        return 1;
    }
    if ((c & 0xe0) == 0xc0) {
        return 2;
    }
    if ((c & 0xf0) == 0xe0) {
        return 3;
    }
    if ((c & 0xf8) == 0xf0) {
        return 4;
    }
    return 0;
}

static int html_stream_emit_current_char(mdf_impl *impl, html_state *state, mdf_sink *sink, char ch)
{
    unsigned char uc;

    uc = (unsigned char)ch;
    if (state->stream_utf8_need > 0 || uc >= 0x80) {
        if (state->stream_utf8_need == 0) {
            state->stream_utf8_need = html_utf8_expected_len(uc);
            state->stream_utf8_len = 0;
        }
        if (state->stream_utf8_len < sizeof(state->stream_utf8_buf)) {
            state->stream_utf8_buf[state->stream_utf8_len++] = ch;
        }
        if (state->stream_utf8_len < state->stream_utf8_need) {
            return 0;
        }
        if (state->stream_mode != 2) {
            if (html_stream_plain_ensure_style(impl,
                                               state,
                                               sink,
                                               state->fg,
                                               state->bg,
                                               state->bold,
                                               state->italic,
                                               state->underline,
                                               state->link,
                                               state->link_len) != 0) {
                return -1;
            }
        }
        if (html_escape_styled_cells(impl,
                                     sink,
                                     state->stream_utf8_buf,
                                     state->stream_utf8_len,
                                     state->fg,
                                     state->bold,
                                     0) != 0) {
            return -1;
        }
        state->stream_utf8_len = 0;
        state->stream_utf8_need = 0;
        return 0;
    }
    if (state->stream_mode == 2) {
        return html_escape(sink, &ch, 1);
    }
    if (html_stream_plain_ensure_style(impl,
                                       state,
                                       sink,
                                       state->fg,
                                       state->bg,
                                       state->bold,
                                       state->italic,
                                       state->underline,
                                       state->link,
                                       state->link_len) != 0) {
        return -1;
    }
    return html_escape(sink, &ch, 1);
}

static int html_stream_close_line(mdf_renderer *self, mdf_sink *sink, int final_line)
{
    mdf_impl *impl;
    html_state *state;

    impl = (mdf_impl *)self->impl;
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    if (state->stream_mode == 2) {
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        if (!final_line && mdf_write_cstr(sink, "\n") != 0) return -1;
    } else if (state->stream_mode == 1) {
        if (final_line) {
            if (state->stream_span_open && mdf_write_cstr(sink, "</span>") != 0) return -1;
            if (state->stream_link_open && mdf_write_cstr(sink, "</a>") != 0) return -1;
        } else {
            if (state->stream_span_open && mdf_write_cstr(sink, "</span>") != 0) return -1;
            if (state->stream_link_open && mdf_write_cstr(sink, "</a>") != 0) return -1;
            if (mdf_write_cstr(sink, "\n") != 0) return -1;
        }
    }
    html_state_reset_stream_line(state);
    return 0;
}

static int html_segment_same_style_link(const html_segment *a, const html_segment *b)
{
    if (a->bold != b->bold ||
        a->italic != b->italic ||
        a->underline != b->underline ||
        a->fg != b->fg ||
        a->link_len != b->link_len) {
        return 0;
    }
    if (a->link_len == 0) {
        return 1;
    }
    return memcmp(a->link, b->link, a->link_len) == 0;
}

int html_write_open_link_grouped(mdf_impl *impl, mdf_sink *sink, const char *link, size_t link_len)
{
    html_buf_sink buf;
    mdf_sink buf_sink;

    memset(&buf, 0, sizeof(buf));
    buf.allocator = &impl->allocator;
    buf_sink.userdata = &buf;
    buf_sink.write = html_buf_sink_write;
    if (mdf_write_cstr(&buf_sink, "<a href=\"") != 0 ||
        html_escape_attr(&buf_sink, link, link_len) != 0 ||
        (impl->html_links_blank &&
         mdf_write_cstr(&buf_sink, "\" target=\"_blank\" rel=\"noopener noreferrer") != 0) ||
        mdf_write_cstr(&buf_sink, "\">") != 0 ||
        mdf_write_all(sink, buf.buf, buf.len) != 0) {
        mdf_free_mem(&impl->allocator, buf.buf, buf.cap);
        return -1;
    }
    mdf_free_mem(&impl->allocator, buf.buf, buf.cap);
    return 0;
}

static int html_write_grouped_escaped(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len)
{
    html_buf_sink buf;
    mdf_sink buf_sink;

    memset(&buf, 0, sizeof(buf));
    buf.allocator = &impl->allocator;
    buf_sink.userdata = &buf;
    buf_sink.write = html_buf_sink_write;
    if (html_escape(&buf_sink, text, len) != 0 ||
        mdf_write_all(sink, buf.buf, buf.len) != 0) {
        mdf_free_mem(&impl->allocator, buf.buf, buf.cap);
        return -1;
    }
    mdf_free_mem(&impl->allocator, buf.buf, buf.cap);
    return 0;
}

static int html_segment_is_plain_table_text(const html_segment *seg)
{
    return seg->fg == -1 &&
           !seg->bold &&
           !seg->italic &&
           !seg->underline &&
           seg->link_len == 0;
}

static int html_segment_needs_grouped_table_text(const html_state *state, size_t i, int header, int *bracket_group)
{
    const html_segment *seg;

    *bracket_group = 0;
    if (header) {
        return 0;
    }
    seg = &state->segments[i];
    if (seg->fg == 35 &&
        !seg->bold &&
        !seg->italic &&
        !seg->underline &&
        seg->link_len == 0) {
        return 1;
    }
    if (!html_segment_is_plain_table_text(seg)) {
        return 0;
    }
    if (seg->len > 2 &&
        seg->text[0] == '[' &&
        seg->text[seg->len - 1] == ']') {
        *bracket_group = 1;
        return 1;
    }
    if (i > 0 &&
        i + 1 < state->segment_count &&
        state->segments[i - 1].len == 1 &&
        state->segments[i - 1].text[0] == '[' &&
        html_segment_is_plain_table_text(&state->segments[i - 1]) &&
        state->segments[i + 1].len == 1 &&
        state->segments[i + 1].text[0] == ']' &&
        html_segment_is_plain_table_text(&state->segments[i + 1])) {
        return 1;
    }
    return 0;
}

static int html_emit_plain_segment_table(mdf_impl *impl, mdf_sink *sink, const char *text, size_t len)
{
    size_t i;

    i = 0;
    while (i < len) {
        if (text[i] == '[') {
            size_t j;

            j = i + 1;
            while (j < len && text[j] != ']') {
                if (text[j] == '[' || text[j] == '\n' || text[j] == '\r') {
                    j = len;
                    break;
                }
                j++;
            }
            if (j < len && text[j] == ']' && j > i + 1) {
                if (mdf_write_all(sink, text + i, 1) != 0) return -1;
                if (html_write_grouped_escaped(impl, sink, text + i + 1, j - i - 1) != 0) return -1;
                if (mdf_write_all(sink, text + j, 1) != 0) return -1;
                i = j + 1;
                continue;
            }
        }
        if (((unsigned char)text[i]) < 0x80) {
            if (html_escape(sink, text + i, 1) != 0) return -1;
            i++;
        } else {
            unsigned long cp;
            size_t adv;

            adv = utf8_decode_codepoint(text + i, len - i, &cp);
            (void)cp;
            if (adv == 0) {
                adv = 1;
            }
            if (html_escape(sink, text + i, adv) != 0) return -1;
            i += adv;
        }
    }
    return 0;
}

int html_emit_segment_slice(mdf_impl *impl, mdf_sink *sink, const html_segment *seg, size_t off, size_t len, const html_segment *base_style, int list_marker, int suppress_link, int render_cells)
{
    int use_wrapper;

    use_wrapper = 1;
    if (base_style != NULL &&
        seg->link_len == 0 &&
        list_marker == 0 &&
        html_segment_same_style_link(seg, base_style)) {
        use_wrapper = 0;
    }
    if (!suppress_link && seg->link_len > 0) {
        if (html_write_open_link_grouped(impl, sink, seg->link, seg->link_len) != 0) return -1;
    }
    if (use_wrapper) {
        html_buf_sink span;
        mdf_sink span_sink;

        memset(&span, 0, sizeof(span));
        span.allocator = &impl->allocator;
        span_sink.userdata = &span;
        span_sink.write = html_buf_sink_write;
        if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
            html_write_style(impl, &span_sink, seg, list_marker) != 0 ||
            mdf_write_cstr(&span_sink, "\">") != 0 ||
            mdf_write_all(sink, span.buf, span.len) != 0) {
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            return -1;
        }
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
    }
    if (html_escape_styled_cells(impl,
                                 sink,
                                 seg->text + off,
                                 len,
                                 seg->fg,
                                 seg->bold,
                                 render_cells) != 0) return -1;
    if (use_wrapper) {
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
    }
    if (!suppress_link && seg->link_len > 0) {
        if (mdf_write_cstr(sink, "</a>") != 0) return -1;
    }
    return 0;
}

static int html_is_list_marker_text(const char *s, size_t len)
{
    size_t i;

    if (len == 1 && s[0] == '-') {
        return 1;
    }
    if (len >= 2 && (s[len - 1] == '.' || s[len - 1] == ')')) {
        for (i = 0; i + 1 < len; i++) {
            if (s[i] < '0' || s[i] > '9') {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

size_t html_line_prefix_len(const char *s, size_t len)
{
    size_t i;
    size_t start;
    size_t mark;
    int any;

    i = 0;
    any = 0;
    while (i < len) {
        while (i < len && s[i] == ' ') {
            i++;
        }
        if (i + 1 < len && s[i] == '>' && s[i + 1] == ' ') {
            i += 2;
            any = 1;
            continue;
        }
        start = i;
        if (i + 1 < len && s[i] == '-' && s[i + 1] == ' ') {
            i += 2;
            any = 1;
        } else {
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                i++;
            }
            if (i > start && i + 1 < len && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') {
                i += 2;
                any = 1;
            } else {
                if (!any && start + 1 == len && s[start] == '>') {
                    return 0;
                }
                if (!any && start > 0) {
                    return start;
                }
                i = start;
            }
        }
        if (i > start) {
            mark = i;
            if (mark + 4 <= len &&
                s[mark] == '[' &&
                (s[mark + 1] == ' ' || s[mark + 1] == 'x' || s[mark + 1] == 'X') &&
                s[mark + 2] == ']' &&
                s[mark + 3] == ' ') {
                i = mark + 4;
            }
            break;
        }
        break;
    }
    return any ? i : 0;
}

static size_t html_chart_container_prefix_len(const char *s, size_t len)
{
    size_t i;
    size_t prefix;
    int any;

    i = 0;
    prefix = 0;
    any = 0;
    while (i < len) {
        while (i < len && s[i] == ' ') {
            i++;
        }
        if (i + 1 < len && s[i] == '>' && s[i + 1] == ' ') {
            i += 2;
            prefix = i;
            any = 1;
            continue;
        }
        break;
    }
    return any ? prefix : 0;
}

static int html_vertical_chart_axis_at(const char *s, size_t len, size_t off)
{
    if (off + 3 > len ||
        (unsigned char)s[off] != 0xe2 ||
        (unsigned char)s[off + 1] != 0x94) {
        return 0;
    }
    return (unsigned char)s[off + 2] == 0x82 ||
           (unsigned char)s[off + 2] == 0x94 ||
           (unsigned char)s[off + 2] == 0xa4;
}

static size_t html_vertical_chart_axis_col(const char *s, size_t len, size_t start, int *found)
{
    size_t i;

    i = start;
    while (i < len) {
        if (html_vertical_chart_axis_at(s, len, i)) {
            *found = 1;
            return i - start;
        }
        i++;
    }
    *found = 0;
    return 0;
}

static size_t html_vertical_chart_content_start(html_state *state,
                                                const char *s,
                                                size_t len,
                                                size_t start,
                                                int trim_prefixed_compact_label,
                                                int quote_prefix)
{
    size_t i;
    size_t label_start;
    size_t label_len;
    size_t axis_col;
    size_t trim;
    int found;

    i = start;
    while (i < len && s[i] == ' ') {
        i++;
    }
    label_start = i;
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        i++;
    }
    while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
        i++;
    }
    label_len = i - label_start;
    if ((label_len == 1 || trim_prefixed_compact_label) &&
        label_len > 0 && label_len < 4 &&
        i + 4 <= len &&
        s[i] == ' ' &&
        (unsigned char)s[i + 1] == 0xe2 &&
        (unsigned char)s[i + 2] == 0x94 &&
        (unsigned char)s[i + 3] == 0xa4 &&
        start < label_start &&
        s[start] == ' ') {
        start++;
        if (quote_prefix && start < len && s[start] == ' ') {
            start++;
        }
    }
    axis_col = html_vertical_chart_axis_col(s, len, start, &found);
    if (!found) {
        return start;
    }
    if (!state->chart_vertical_axis_col_valid) {
        state->chart_vertical_axis_col = axis_col;
        state->chart_vertical_axis_col_valid = 1;
        return start;
    }
    if (axis_col <= state->chart_vertical_axis_col) {
        return start;
    }
    trim = axis_col - state->chart_vertical_axis_col;
    while (trim > 0 && start < len && s[start] == ' ') {
        start++;
        trim--;
    }
    return start;
}

static int html_heading_level(const char *s, size_t len)
{
    size_t i;

    i = 0;
    while (i < len && i < 6 && s[i] == '#') {
        i++;
    }
    if (i == 0 || i >= len || s[i] != ' ') {
        return 0;
    }
    return (int)i;
}

static int html_prefix_has_quote_marker(const char *s, size_t prefix_len)
{
    size_t i;

    for (i = 0; i < prefix_len; i++) {
        if (s[i] == '>') {
            return 1;
        }
    }
    return 0;
}

static int html_prefix_can_coalesce_boring(mdf_impl *impl, html_state *state, size_t prefix_len)
{
    size_t i;
    size_t pos;

    for (i = 0; i < prefix_len; i++) {
        char c;

        c = state->line_plain[i];
        if (c != ' ' && c != '\t' && c != '>' && c != '-' && c != '*' && c != '+' &&
            c != '.' && c != ')' && c != '[' && c != ']' && c != 'x' && c != 'X' &&
            (c < '0' || c > '9')) {
            return 0;
        }
    }
    pos = 0;
    for (i = 0; i < state->segment_count; i++) {
        html_segment *seg;
        size_t seg_end;
        size_t len;

        seg = &state->segments[i];
        seg_end = pos + seg->len;
        if (pos >= prefix_len) {
            break;
        }
        len = seg_end > prefix_len ? prefix_len - pos : seg->len;
        if (html_semantic_heading_level(impl, seg->fg, seg->bold, seg->italic, seg->underline,
                                        html_is_list_marker_text(seg->text, len)) > 0) {
            return 0;
        }
        pos = seg_end;
    }
    return 1;
}

static int html_segment_is_plain_boring_effect(mdf_impl *impl, const html_segment *seg)
{
    return seg->link_len == 0 &&
           !seg->bold &&
           !seg->italic &&
           !seg->underline &&
           html_semantic_heading_level(impl, seg->fg, seg->bold, seg->italic, seg->underline, 0) == 0;
}

static int html_emit_plain_boring_run(mdf_impl *impl,
                                      html_state *state,
                                      mdf_sink *sink,
                                      size_t start,
                                      size_t end,
                                      size_t *index,
                                      size_t *pos,
                                      size_t slice_start,
                                      size_t slice_end)
{
    html_segment *first;
    html_buf_sink span;
    mdf_sink span_sink;
    size_t i;
    size_t run_pos;
    size_t run_count;

    first = &state->segments[*index];
    i = *index;
    run_pos = *pos + first->len;
    run_count = 1;
    while (i + 1 < state->segment_count && run_pos < end) {
        html_segment *next;

        next = &state->segments[i + 1];
        if (next->len == 0 || !html_segment_is_plain_boring_effect(impl, next)) {
            break;
        }
        i++;
        run_pos += next->len;
        run_count++;
    }
    if (run_count == 1) {
        return 1;
    }

    memset(&span, 0, sizeof(span));
    span.allocator = &impl->allocator;
    span_sink.userdata = &span;
    span_sink.write = html_buf_sink_write;
    if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
        html_write_style(impl, &span_sink, first, 0) != 0 ||
        mdf_write_cstr(&span_sink, "\">") != 0 ||
        html_escape(&span_sink, first->text + slice_start, slice_end - slice_start) != 0) {
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
        return -1;
    }

    run_pos = *pos + first->len;
    for (i = *index + 1; i < state->segment_count && run_pos < end; i++) {
        html_segment *seg;
        size_t seg_start;
        size_t seg_end;
        size_t off;
        size_t len;

        seg = &state->segments[i];
        if (!html_segment_is_plain_boring_effect(impl, seg)) {
            break;
        }
        seg_start = run_pos;
        seg_end = seg_start + seg->len;
        off = start > seg_start ? start - seg_start : 0;
        len = end < seg_end ? end - seg_start : seg->len;
        if (len > off && html_escape(&span_sink, seg->text + off, len - off) != 0) {
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            return -1;
        }
        run_pos = seg_end;
    }
    if (mdf_write_cstr(&span_sink, "</span>") != 0 ||
        mdf_write_all(sink, span.buf, span.len) != 0) {
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
        return -1;
    }
    mdf_free_mem(&impl->allocator, span.buf, span.cap);
    *index = i - 1;
    *pos = run_pos;
    return 0;
}

static int html_write_heading_suffix(mdf_sink *sink, int level)
{
    static const char *const sizes[] = {
        "",
        "font-size:32pt;font-weight:700;--mdf-heading-indent:2ch;",
        "font-size:22pt;font-weight:700;--mdf-heading-indent:3ch;",
        "font-size:15pt;font-weight:700;--mdf-heading-indent:4ch;",
        "font-size:14pt;font-weight:700;--mdf-heading-indent:5ch;",
        "font-size:13pt;font-weight:700;--mdf-heading-indent:6ch;",
        "font-size:12.5pt;font-weight:700;--mdf-heading-indent:7ch;"
    };

    if (level < 1 || level > 6) {
        level = 1;
    }
    return mdf_write_cstr(sink, sizes[level]);
}

static int html_write_heading_indent_suffix(mdf_sink *sink, int level)
{
    static const char *const suffixes[] = {
        "",
        "--mdf-heading-indent:2ch;",
        "--mdf-heading-indent:3ch;",
        "--mdf-heading-indent:4ch;",
        "--mdf-heading-indent:5ch;",
        "--mdf-heading-indent:6ch;",
        "--mdf-heading-indent:7ch;"
    };

    if (level < 1 || level > 6) {
        level = 1;
    }
    return mdf_write_cstr(sink, suffixes[level]);
}

static int html_segment_is_code_block(mdf_impl *impl, const html_segment *seg)
{
    html_state code;

    html_parse_style_prefix(&code, mdf_theme_code_block(impl));
    return html_style_attrs_equal(&code, seg->fg, seg->bold, seg->italic, seg->underline);
}

static int html_line_stream_kind(const html_state *state, int *heading_level)
{
    size_t i;
    char first;

    *heading_level = 0;
    if (state->line_plain_len == 0) {
        return 0;
    }
    first = state->line_plain[0];
    if (first == ' ' || first == '>' || first == '-') {
        return 0;
    }
    if (first >= '0' && first <= '9') {
        return 0;
    }
    if (first == '#') {
        i = 0;
        while (i < state->line_plain_len && i < 6 && state->line_plain[i] == '#') {
            i++;
        }
        if (i < state->line_plain_len) {
            if (state->line_plain[i] == ' ') {
                *heading_level = (int)i;
                return 2;
            }
            return 1;
        }
        return 0;
    }
    return 1;
}

static int html_maybe_start_stream_line(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;
    html_state *state;
    int kind;
    int heading_level;
    html_segment *base_seg;
    size_t heading_marker_len;

    impl = (mdf_impl *)self->impl;
    state = html_state_get(impl);
    if (state == NULL || state->stream_mode != 0) {
        return 0;
    }
    if (state->chart_mode) {
        return 0;
    }
    kind = html_line_stream_kind(state, &heading_level);
    if (kind == 0) {
        return 0;
    }
    if (kind == 2) {
        base_seg = state->segment_count > 0 ? &state->segments[0] : NULL;
        if (base_seg == NULL) {
            return 0;
        }
        if (mdf_write_cstr(sink, "<span class=\"mdf-heading\" style=\"") != 0 ||
            html_write_style(impl, sink, base_seg, 0) != 0 ||
            html_write_heading_suffix(sink, heading_level) != 0 ||
            mdf_write_cstr(sink, "\">") != 0) {
            return -1;
        }
        heading_marker_len = html_heading_marker_len(state->line_plain, state->line_plain_len, heading_level);
        if (heading_marker_len > 0 && mdf_write_all(sink, state->line_plain, heading_marker_len) != 0) return -1;
        if (html_escape(sink,
                        state->line_plain + heading_marker_len,
                        state->line_plain_len - heading_marker_len) != 0) return -1;
        state->stream_mode = 2;
        state->stream_heading_level = heading_level;
        html_state_clear_line(impl, state);
        return 0;
    }
    if (html_stream_emit_buffered_plain(impl, state, sink) != 0) {
        return -1;
    }
    state->stream_mode = 1;
    html_state_clear_line(impl, state);
    return 0;
}

int html_emit_line_range(mdf_impl *impl, html_state *state, mdf_sink *sink, size_t start, size_t end, const html_segment *base_style, int prefix_mode, int suppress_link)
{
    size_t i;
    size_t off;
    size_t pos;
    int skip_leading_space;
    int skip_quote_marker;
    int skip_segment;
    size_t skip_segment_index;

    pos = 0;
    skip_leading_space = 0;
    skip_quote_marker = 0;
    skip_segment = 0;
    skip_segment_index = 0;
    for (i = 0; i < state->segment_count; i++) {
        html_segment *seg;
        size_t seg_start;
        size_t seg_end;
        size_t slice_start;
        size_t slice_end;

        seg = &state->segments[i];
        if (skip_segment && i == skip_segment_index) {
            skip_segment = 0;
            pos += seg->len;
            continue;
        }
        seg_start = pos;
        seg_end = pos + seg->len;
        if (seg_end <= start) {
            pos = seg_end;
            continue;
        }
        if (seg_start >= end) {
            break;
        }
        slice_start = start > seg_start ? start - seg_start : 0;
        slice_end = end < seg_end ? end - seg_start : seg->len;
        if (skip_quote_marker &&
            slice_start < slice_end &&
            slice_end - slice_start == 1 &&
            seg->text[slice_start] == '>') {
            skip_quote_marker = 0;
            skip_leading_space = 1;
            pos = seg_end;
            continue;
        }
        skip_quote_marker = 0;
        if (skip_leading_space && slice_start < slice_end && seg->text[slice_start] == ' ') {
            slice_start++;
            skip_leading_space = 0;
            if (slice_start == slice_end) {
                pos = seg_end;
                continue;
            }
        } else {
            skip_leading_space = 0;
        }
        off = slice_end - slice_start;
        if (!prefix_mode &&
            impl->opts.boring &&
            off > 0 &&
            html_segment_is_plain_boring_effect(impl, seg)) {
            int run_status;

            run_status = html_emit_plain_boring_run(impl,
                                                    state,
                                                    sink,
                                                    start,
                                                    end,
                                                    &i,
                                                    &pos,
                                                    slice_start,
                                                    slice_end);
            if (run_status < 0) {
                return -1;
            }
            if (run_status == 0) {
                continue;
            }
        }
        if (!prefix_mode &&
            impl->opts.boring &&
            off > 0 &&
            i + 1 < state->segment_count &&
            state->segments[i + 1].len > 0 &&
            state->segments[i + 1].text[0] == '>') {
            size_t j;
            int all_spaces;

            all_spaces = 1;
            for (j = 0; j < off; j++) {
                if (seg->text[slice_start + j] != ' ' && seg->text[slice_start + j] != '\t') {
                    all_spaces = 0;
                    break;
                }
            }
            if (all_spaces) {
                html_buf_sink span;
                mdf_sink span_sink;

                memset(&span, 0, sizeof(span));
                span.allocator = &impl->allocator;
                span_sink.userdata = &span;
                span_sink.write = html_buf_sink_write;
                if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                    html_write_style(impl, &span_sink, seg, 0) != 0 ||
                    mdf_write_cstr(&span_sink, "\">") != 0 ||
                    html_escape(&span_sink, seg->text + slice_start, off) != 0 ||
                    mdf_write_cstr(&span_sink, "&gt;</span>") != 0 ||
                    mdf_write_all(sink, span.buf, span.len) != 0) {
                    mdf_free_mem(&impl->allocator, span.buf, span.cap);
                    return -1;
                }
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                skip_quote_marker = 1;
                pos = seg_end;
                continue;
            }
        }
        if (prefix_mode &&
            impl->opts.boring &&
            off >= 2 &&
            seg->text[slice_start] == '>' &&
            seg->text[slice_start + 1] == ' ' &&
            i + 1 < state->segment_count &&
            state->segments[i + 1].len >= 2 &&
            state->segments[i + 1].text[0] == '>' &&
            state->segments[i + 1].text[1] == ' ') {
            html_buf_sink span;
            mdf_sink span_sink;

            memset(&span, 0, sizeof(span));
            span.allocator = &impl->allocator;
            span_sink.userdata = &span;
            span_sink.write = html_buf_sink_write;
            if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                html_write_style(impl, &span_sink, seg, 0) != 0 ||
                mdf_write_cstr(&span_sink, "\">&gt; &gt; </span>") != 0 ||
                mdf_write_all(sink, span.buf, span.len) != 0) {
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                return -1;
            }
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            skip_segment = 1;
            skip_segment_index = i + 1;
            pos = seg_end;
            continue;
        }
        if (prefix_mode &&
            impl->opts.boring &&
            off > 0 &&
            i + 1 < state->segment_count) {
            size_t j;
            size_t marker_index;
            int all_spaces;
            int found_marker;

            all_spaces = 1;
            for (j = 0; j < off; j++) {
                if (seg->text[slice_start + j] != ' ' && seg->text[slice_start + j] != '\t') {
                    all_spaces = 0;
                    break;
                }
            }
            found_marker = 0;
            marker_index = i + 1;
            if (all_spaces) {
                for (j = i + 1; j < state->segment_count; j++) {
                    if (state->segments[j].len == 0) {
                        continue;
                    }
                    if (state->segments[j].text[0] == '>') {
                        marker_index = j;
                        found_marker = 1;
                    }
                    break;
                }
            }
            if (found_marker) {
                html_buf_sink span;
                mdf_sink span_sink;
                int include_space;

                include_space = marker_index + 1 < state->segment_count &&
                                state->segments[marker_index + 1].len > 0 &&
                                state->segments[marker_index + 1].text[0] == ' ' &&
                                seg_end + 2 <= end;
                memset(&span, 0, sizeof(span));
                span.allocator = &impl->allocator;
                span_sink.userdata = &span;
                span_sink.write = html_buf_sink_write;
                if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                    html_write_style(impl, &span_sink, seg, 0) != 0 ||
                    mdf_write_cstr(&span_sink, "\">") != 0 ||
                    html_escape(&span_sink, seg->text + slice_start, off) != 0 ||
                    mdf_write_cstr(&span_sink, include_space ? "&gt; </span>" : "&gt;</span>") != 0 ||
                    mdf_write_all(sink, span.buf, span.len) != 0) {
                    mdf_free_mem(&impl->allocator, span.buf, span.cap);
                    return -1;
                }
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                if (marker_index == i + 1) {
                    skip_quote_marker = 1;
                    if (include_space) {
                        skip_leading_space = 1;
                    }
                }
                pos = seg_end;
                continue;
            }
        }
        if (prefix_mode &&
            impl->opts.boring &&
            off > 0 &&
            i + 2 < state->segment_count &&
            state->segments[i + 1].len > 0 &&
            state->segments[i + 1].text[0] == '>' &&
            state->segments[i + 2].len > 0 &&
            state->segments[i + 2].text[0] == ' ') {
            size_t j;
            int all_spaces;

            all_spaces = 1;
            for (j = 0; j < off; j++) {
                if (seg->text[slice_start + j] != ' ' && seg->text[slice_start + j] != '\t') {
                    all_spaces = 0;
                    break;
                }
            }
            if (all_spaces) {
                html_buf_sink span;
                mdf_sink span_sink;

                memset(&span, 0, sizeof(span));
                span.allocator = &impl->allocator;
                span_sink.userdata = &span;
                span_sink.write = html_buf_sink_write;
                if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                    html_write_style(impl, &span_sink, seg, 0) != 0 ||
                    mdf_write_cstr(&span_sink, "\">") != 0 ||
                    html_escape(&span_sink, seg->text + slice_start, off) != 0 ||
                    mdf_write_cstr(&span_sink, "&gt; </span>") != 0 ||
                    mdf_write_all(sink, span.buf, span.len) != 0) {
                    mdf_free_mem(&impl->allocator, span.buf, span.cap);
                    return -1;
                }
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                skip_quote_marker = 1;
                pos = seg_end;
                continue;
            }
        }
        if (prefix_mode &&
            impl->opts.boring &&
            off > 0 &&
            i + 1 < state->segment_count &&
            state->segments[i + 1].len > 0 &&
            state->segments[i + 1].text[0] == '>' &&
            (seg_end + state->segments[i + 1].len >= end ||
             i + 2 >= state->segment_count ||
             state->segments[i + 2].len == 0 ||
             state->segments[i + 2].text[0] != ' ')) {
            size_t j;
            int all_spaces;

            all_spaces = 1;
            for (j = 0; j < off; j++) {
                if (seg->text[slice_start + j] != ' ' && seg->text[slice_start + j] != '\t') {
                    all_spaces = 0;
                    break;
                }
            }
            if (all_spaces) {
                html_buf_sink span;
                mdf_sink span_sink;

                memset(&span, 0, sizeof(span));
                span.allocator = &impl->allocator;
                span_sink.userdata = &span;
                span_sink.write = html_buf_sink_write;
                if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                    html_write_style(impl, &span_sink, seg, 0) != 0 ||
                    mdf_write_cstr(&span_sink, "\">") != 0 ||
                    html_escape(&span_sink, seg->text + slice_start, off) != 0 ||
                    mdf_write_cstr(&span_sink, "&gt;</span>") != 0 ||
                    mdf_write_all(sink, span.buf, span.len) != 0) {
                    mdf_free_mem(&impl->allocator, span.buf, span.cap);
                    return -1;
                }
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                skip_quote_marker = 1;
                pos = seg_end;
                continue;
            }
        }
        if (prefix_mode &&
            impl->opts.boring &&
            off == 1 &&
            seg->text[slice_start] == '>' &&
            i + 1 < state->segment_count &&
            state->segments[i + 1].len > 0 &&
            state->segments[i + 1].text[0] == ' ') {
            html_buf_sink span;
            mdf_sink span_sink;

            memset(&span, 0, sizeof(span));
            span.allocator = &impl->allocator;
            span_sink.userdata = &span;
            span_sink.write = html_buf_sink_write;
            if (mdf_write_cstr(&span_sink, "<span style=\"") != 0 ||
                html_write_style(impl, &span_sink, seg, 0) != 0 ||
                mdf_write_cstr(&span_sink, "\">&gt; </span>") != 0 ||
                mdf_write_all(sink, span.buf, span.len) != 0) {
                mdf_free_mem(&impl->allocator, span.buf, span.cap);
                return -1;
            }
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            skip_leading_space = 1;
            pos = seg_end;
            continue;
        }
        if (html_emit_segment_slice(impl,
                                    sink,
                                    seg,
                                    slice_start,
                                    off,
                                    base_style,
                                    prefix_mode && html_is_list_marker_text(seg->text + slice_start, off),
                                    suppress_link,
                                    state->chart_mode) != 0) {
            return -1;
        }
        pos = seg_end;
    }
    return 0;
}

static int html_flush_line(mdf_renderer *self, mdf_sink *sink, int final_line, char next_char)
{
    mdf_impl *impl;
    html_state *state;
    size_t prefix_len;
    size_t heading_marker_len;
    int heading_level;
    html_segment *base_seg;
    html_segment *content_seg;
    size_t i;
    size_t pos;
    const char *whole_link;
    size_t whole_link_len;
    int whole_line_link;

    impl = (mdf_impl *)self->impl;
    (void)next_char;
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    if (state->line_plain_len == 0) {
        return mdf_write_cstr(sink, "\n");
    }
    if (state->chart_mode) {
        prefix_len = state->chart_kind == MDF_CHART_KIND_TILE ?
                     html_line_prefix_len(state->line_plain, state->line_plain_len) :
                     html_chart_container_prefix_len(state->line_plain, state->line_plain_len);
    } else {
        prefix_len = html_line_prefix_len(state->line_plain, state->line_plain_len);
    }
    heading_level = prefix_len == 0 ? html_heading_level(state->line_plain, state->line_plain_len) : 0;
    base_seg = state->segment_count > 0 ? &state->segments[0] : NULL;
    content_seg = NULL;
    pos = 0;
    for (i = 0; i < state->segment_count; i++) {
        if (pos + state->segments[i].len > prefix_len) {
            content_seg = &state->segments[i];
            break;
        }
        pos += state->segments[i].len;
    }
    if (prefix_len > 0 &&
        content_seg != NULL &&
        html_prefix_has_quote_marker(state->line_plain, prefix_len) &&
        html_heading_level(state->line_plain + prefix_len, state->line_plain_len - prefix_len) > 0) {
        int inner_level;
        const char *inner;
        size_t inner_len;
        html_buf_sink span;
        mdf_sink span_sink;

        inner = state->line_plain + prefix_len;
        inner_len = state->line_plain_len - prefix_len;
        inner_level = html_heading_level(inner, inner_len);
        memset(&span, 0, sizeof(span));
        span.allocator = &impl->allocator;
        span_sink.userdata = &span;
        span_sink.write = html_buf_sink_write;
        if (mdf_write_cstr(&span_sink, "<span class=\"mdf-heading\" style=\"") != 0 ||
            html_write_style(impl, &span_sink, content_seg, 0) != 0 ||
            html_write_heading_suffix(&span_sink, inner_level) != 0 ||
            mdf_write_cstr(&span_sink, "\">") != 0 ||
            mdf_write_all(sink, span.buf, span.len) != 0) {
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            return -1;
        }
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
        heading_marker_len = html_heading_marker_len(inner, inner_len, inner_level);
        if (heading_marker_len > 0 &&
            mdf_write_all(sink, inner, heading_marker_len) != 0) return -1;
        if (html_escape(sink, inner + heading_marker_len, inner_len - heading_marker_len) != 0) return -1;
        if (!final_line) {
            if (mdf_write_cstr(sink, "&gt;") != 0) return -1;
            if (mdf_write_cstr(sink, " ") != 0) return -1;
        }
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        if (!final_line && mdf_write_cstr(sink, "\n") != 0) return -1;
        return 0;
    }
    if (prefix_len > 0 &&
        content_seg != NULL &&
        html_segment_is_code_block(impl, content_seg) &&
        html_theme_has_code_block_semantics(impl) &&
        html_heading_level(state->line_plain + prefix_len, state->line_plain_len - prefix_len) > 0) {
        int inner_level;
        const char *inner;
        size_t inner_len;
        html_buf_sink span;
        mdf_sink span_sink;

        inner = state->line_plain + prefix_len;
        inner_len = state->line_plain_len - prefix_len;
        inner_level = html_heading_level(inner, inner_len);
        memset(&span, 0, sizeof(span));
        span.allocator = &impl->allocator;
        span_sink.userdata = &span;
        span_sink.write = html_buf_sink_write;
        if (mdf_write_cstr(&span_sink, "<span class=\"mdf-heading\" style=\"") != 0 ||
            html_write_style(impl, &span_sink, content_seg, 0) != 0 ||
            html_write_heading_indent_suffix(&span_sink, inner_level) != 0 ||
            mdf_write_cstr(&span_sink, "\">") != 0 ||
            mdf_write_all(sink, span.buf, span.len) != 0) {
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            return -1;
        }
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
        if (html_escape(sink, inner, inner_len) != 0) return -1;
        for (i = 0; i < prefix_len; i++) {
            if (mdf_write_cstr(sink, " ") != 0) return -1;
        }
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        if (!final_line && mdf_write_cstr(sink, "\n") != 0) return -1;
        return 0;
    }
    if (heading_level > 0 && base_seg != NULL) {
        html_buf_sink span;
        mdf_sink span_sink;

        memset(&span, 0, sizeof(span));
        span.allocator = &impl->allocator;
        span_sink.userdata = &span;
        span_sink.write = html_buf_sink_write;
        if (mdf_write_cstr(&span_sink, "<span class=\"mdf-heading\" style=\"") != 0 ||
            html_write_style(impl, &span_sink, base_seg, 0) != 0 ||
            html_write_heading_suffix(&span_sink, heading_level) != 0 ||
            mdf_write_cstr(&span_sink, "\">") != 0 ||
            mdf_write_all(sink, span.buf, span.len) != 0) {
            mdf_free_mem(&impl->allocator, span.buf, span.cap);
            return -1;
        }
        mdf_free_mem(&impl->allocator, span.buf, span.cap);
        heading_marker_len = html_heading_marker_len(state->line_plain, state->line_plain_len, heading_level);
        if (heading_marker_len > 0 &&
            mdf_write_all(sink, state->line_plain, heading_marker_len) != 0) return -1;
        if (html_escape(sink,
                        state->line_plain + heading_marker_len,
                        state->line_plain_len - heading_marker_len) != 0) return -1;
        if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        if (!final_line && mdf_write_cstr(sink, "\n") != 0) return -1;
        return 0;
    }
    if (state->chart_mode && prefix_len > 0) {
        size_t content_start;

        content_start = prefix_len;
        if (state->chart_kind == MDF_CHART_KIND_HORIZONTAL_BAR) {
            while (content_start < state->line_plain_len && state->line_plain[content_start] == ' ') {
                content_start++;
            }
        } else if (state->chart_kind == MDF_CHART_KIND_VERTICAL_BAR) {
            content_start = html_vertical_chart_content_start(state,
                                                              state->line_plain,
                                                              state->line_plain_len,
                                                              content_start,
                                                              1,
                                                              html_prefix_has_quote_marker(state->line_plain, prefix_len));
        }
        if (mdf_write_cstr(sink, state->chart_kind == MDF_CHART_KIND_TILE ?
                           "<span class=\"mdf-line mdf-chart-line\" style=\"display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"display:inline-block;white-space:pre;overflow-wrap:normal;text-align:left;\">" :
                           "<span class=\"mdf-line mdf-chart-line\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\">") != 0) return -1;
        if (html_emit_line_range(impl, state, sink, content_start, state->line_plain_len, NULL, 0, 0) != 0) return -1;
        if (final_line) {
            return 0;
        }
        if (state->chart_kind == MDF_CHART_KIND_TILE) {
            return mdf_write_cstr(sink, "</span></span>");
        }
        return mdf_write_cstr(sink, "</span></span>\n");
    }
    if (prefix_len > 0) {
        whole_line_link = 0;
        whole_link = NULL;
        whole_link_len = 0;
        pos = 0;
        for (i = 0; i < state->segment_count; i++) {
            html_segment *seg;
            size_t seg_end;

            seg = &state->segments[i];
            seg_end = pos + seg->len;
            if (seg_end <= prefix_len) {
                pos = seg_end;
                continue;
            }
            if (seg->link_len == 0) {
                whole_line_link = 0;
                whole_link = NULL;
                break;
            }
            if (whole_link == NULL) {
                whole_link = seg->link;
                whole_link_len = seg->link_len;
                whole_line_link = 1;
            } else if (seg->link_len != whole_link_len || memcmp(seg->link, whole_link, whole_link_len) != 0) {
                whole_line_link = 0;
                whole_link = NULL;
                break;
            }
            pos = seg_end;
        }
        if (whole_line_link && whole_link != NULL) {
            if (html_write_open_link_grouped(impl, sink, whole_link, whole_link_len) != 0) return -1;
        }
        if (mdf_write_cstr(sink, state->chart_mode ? "<span class=\"mdf-line mdf-chart-line\" style=\"white-space:pre;overflow-wrap:normal;\"><span class=\"mdf-prefix\">" : "<span class=\"mdf-line\"><span class=\"mdf-prefix\">") != 0) return -1;
        if (impl->opts.boring && html_prefix_can_coalesce_boring(impl, state, prefix_len)) {
            if (mdf_write_cstr(sink, "<span style=\"color:rgb(0,0,0);\">") != 0 ||
                html_escape(sink, state->line_plain, prefix_len) != 0 ||
                mdf_write_cstr(sink, "</span>") != 0) return -1;
        } else if (html_emit_line_range(impl, state, sink, 0, prefix_len, NULL, 1, 0) != 0) return -1;
        if (mdf_write_cstr(sink, state->chart_mode ? "</span><span class=\"mdf-content\" style=\"white-space:pre;overflow-wrap:normal;\">" : "</span><span class=\"mdf-content\">") != 0) return -1;
        if (html_emit_line_range(impl, state, sink, prefix_len, state->line_plain_len, NULL, 0, whole_line_link) != 0) return -1;
        if (whole_line_link) {
            if (mdf_write_cstr(sink, "</a>") != 0) return -1;
        }
        if (final_line) {
            return 0;
        }
        if (mdf_write_cstr(sink, "</span></span>") != 0) return -1;
        if (mdf_write_cstr(sink, "\n") != 0) return -1;
        return 0;
    }
    if (state->chart_mode) {
        size_t content_start;

        content_start = 0;
        if (state->chart_kind == MDF_CHART_KIND_HORIZONTAL_BAR) {
            while (content_start < state->line_plain_len && state->line_plain[content_start] == ' ') {
                content_start++;
            }
        } else if (state->chart_kind == MDF_CHART_KIND_VERTICAL_BAR) {
            content_start = html_vertical_chart_content_start(state, state->line_plain, state->line_plain_len, content_start, 0, 0);
        }
        if (mdf_write_cstr(sink, state->chart_kind == MDF_CHART_KIND_TILE ?
                           "<span class=\"mdf-line mdf-chart-line\" style=\"display:block;white-space:pre;overflow-wrap:normal;text-align:center;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"display:inline-block;white-space:pre;overflow-wrap:normal;text-align:left;\">" :
                           "<span class=\"mdf-line mdf-chart-line\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\"><span class=\"mdf-prefix\"></span><span class=\"mdf-content\" style=\"white-space:pre;overflow-wrap:normal;text-align:left;\">") != 0) return -1;
        if (html_emit_line_range(impl, state, sink, content_start, state->line_plain_len, NULL, 0, 0) != 0) return -1;
        if (final_line) {
            return 0;
        }
        if (state->chart_kind == MDF_CHART_KIND_TILE) {
            return mdf_write_cstr(sink, "</span></span>");
        }
        return mdf_write_cstr(sink, "</span></span>\n");
    }
    if (html_emit_line_range(impl, state, sink, 0, state->line_plain_len, NULL, 0, 0) != 0) return -1;
    if (final_line) {
        return 0;
    }
    return mdf_write_cstr(sink, "\n");
}

int html_flush_pending_lines(mdf_renderer *self, mdf_sink *sink, int final_line, char next_char)
{
    mdf_impl *impl;
    html_state *state;

    impl = (mdf_impl *)self->impl;
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    if (state->pending_newline && state->stream_mode != 0) {
        if (html_stream_close_line(self, sink, final_line) != 0) {
            return -1;
        }
        state->pending_newline = 0;
        if (final_line) {
            return 0;
        }
    }
    if (final_line && state->stream_mode != 0) {
        if (html_stream_close_line(self, sink, 1) != 0) {
            return -1;
        }
    }
    if (state->pending_newline || state->line_plain_len > 0) {
        if (html_flush_line(self, sink, final_line && state->line_plain_len > 0, next_char) != 0) {
            return -1;
        }
        html_state_clear_line(impl, state);
        state->pending_newline = 0;
    }
    return 0;
}

static int html_bridge_consume(mdf_renderer *self, mdf_sink *sink, const char *src, size_t len)
{
    mdf_impl *impl;
    html_state *state;
    size_t i;
    char ch;

    impl = (mdf_impl *)self->impl;
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    state->boring = impl->opts.boring;
    for (i = 0; i < len; i++) {
        ch = src[i];
        if (state->pending_newline) {
            if (html_flush_pending_lines(self, sink, 0, ch) != 0) return -1;
        }
        switch (state->mode) {
        case 0:
            if (ch == '\033') {
                state->mode = 1;
            } else if (ch == '\n') {
                if (state->stream_mode != 0) {
                    state->pending_newline = 1;
                } else if (state->line_plain_len == 0) {
                    if (html_flush_line(self, sink, 0, '\n') != 0) return -1;
                    html_state_clear_line(impl, state);
                } else {
                    state->pending_newline = 1;
                }
            } else if (ch != '\r') {
                if (state->stream_mode != 0) {
                    if (html_stream_emit_current_char(impl, state, sink, ch) != 0) return -1;
                } else {
                    if (html_state_append_text(impl, state, &ch, 1) != 0) return -1;
                    if (html_maybe_start_stream_line(self, sink) != 0) return -1;
                }
            }
            break;
        case 1:
            if (ch == '[') {
                state->mode = 2;
                state->csi_len = 0;
            } else if (ch == ']') {
                state->mode = 3;
                state->osc_len = 0;
                if (state->osc_buf != NULL) {
                    state->osc_buf[0] = '\0';
                }
            } else {
                state->mode = 0;
            }
            break;
        case 2:
            if (state->csi_len + 1 < sizeof(state->csi_buf)) {
                state->csi_buf[state->csi_len++] = ch;
                state->csi_buf[state->csi_len] = '\0';
            }
            if (ch >= '@' && ch <= '~') {
                if (ch == 'm' && state->csi_len > 0) {
                    html_state_parse_sgr(state, state->csi_buf, state->csi_len - 1);
                }
                state->mode = 0;
                state->csi_len = 0;
            }
            break;
        case 3:
            if (ch == '\007') {
                html_state_parse_osc(impl, state);
                state->mode = 0;
            } else if (ch == '\033') {
                state->mode = 4;
            } else if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, &ch, 1) != 0) {
                return -1;
            }
            break;
        case 4:
            if (ch == '\\') {
                html_state_parse_osc(impl, state);
                state->mode = 0;
            } else {
                if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, "\033", 1) != 0) return -1;
                if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, &ch, 1) != 0) return -1;
                state->mode = 3;
            }
            break;
        }
    }
    return 0;
}

int html_bridge_write(void *userdata, const char *src, size_t len)
{
    html_bridge_sink *bridge;
    mdf_impl *impl;
    int save_boring;
    int rc;

    bridge = (html_bridge_sink *)userdata;
    impl = (mdf_impl *)bridge->renderer->impl;
    save_boring = impl->opts.boring;
    impl->opts.boring = bridge->boring;
    rc = html_bridge_consume(bridge->renderer, bridge->sink, src, len);
    impl->opts.boring = save_boring;
    return rc;
}

static void html_bridge_scope_begin(mdf_renderer *self, mdf_sink *sink, html_bridge_scope *scope)
{
    mdf_impl *impl;

    impl = (mdf_impl *)self->impl;
    memset(scope, 0, sizeof(*scope));
    scope->impl = impl;
    scope->bridge.renderer = self;
    scope->bridge.sink = sink;
    scope->bridge.boring = impl->opts.boring;
    scope->sink.userdata = &scope->bridge;
    scope->sink.write = html_bridge_write;
    scope->save_width = impl->opts.width;
    scope->save_osc8 = impl->opts.osc8;
    scope->save_boring = impl->opts.boring;
    scope->save_chart_suppress_centering = impl->chart_suppress_centering;
    impl->opts.width = 0;
    impl->opts.osc8 = 1;
    impl->opts.boring = 0;
}

static void html_bridge_scope_end(html_bridge_scope *scope)
{
    scope->impl->opts.width = scope->save_width;
    scope->impl->opts.osc8 = scope->save_osc8;
    scope->impl->opts.boring = scope->save_boring;
    scope->impl->chart_suppress_centering = scope->save_chart_suppress_centering;
}

static int html_should_flush_ansi_word_after_text(const mdf_impl *impl, const mdf_token *token)
{
    return token->type == MDF_TOKEN_TEXT &&
           impl->inline_mode == 0 &&
           !impl->in_pre &&
           impl->inline_text_len == 0 &&
           impl->inline_url_len == 0 &&
           impl->inline_code_len == 0 &&
           impl->inline_emph_len == 0 &&
           impl->inline_entity_len == 0 &&
           impl->pending_fallback_url_len == 0 &&
           !impl->pending_fallback_exact &&
           impl->ansi_word_len > 0;
}

static int html_pending_line_is_quote_prefix_only(const html_state *state)
{
    size_t i;

    if (state == NULL || state->pending_newline || state->line_plain_len == 0) {
        return 0;
    }
    i = 0;
    while (i < state->line_plain_len && state->line_plain[i] == ' ') {
        i++;
    }
    if (i >= state->line_plain_len || state->line_plain[i] != '>') {
        return 0;
    }
    i++;
    while (i < state->line_plain_len && state->line_plain[i] == ' ') {
        i++;
    }
    return i == state->line_plain_len;
}

static int html_bridge_run_token(mdf_renderer *self, mdf_sink *sink, const mdf_token *token)
{
    mdf_impl *impl;
    html_state *state;
    html_bridge_scope bridge;
    int chart_width;
    int chart_token;
    int saved_chart_mode;
    int saved_chart_kind;
    int saved_chart_vertical_axis_col_valid;
    size_t saved_chart_vertical_axis_col;
    int rc;

    impl = (mdf_impl *)self->impl;
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    chart_token = token->type == MDF_TOKEN_CHART_BLOCK;
    saved_chart_mode = state->chart_mode;
    saved_chart_kind = state->chart_kind;
    saved_chart_vertical_axis_col_valid = state->chart_vertical_axis_col_valid;
    saved_chart_vertical_axis_col = state->chart_vertical_axis_col;
    if (chart_token) {
        if (html_pending_line_is_quote_prefix_only(state)) {
            html_state_clear_line(impl, state);
        } else if (html_flush_pending_lines(self, sink, 0, '\n') != 0) {
            return -1;
        }
    }
    if (chart_token && state->stream_mode != 0 && html_stream_close_line(self, sink, 0) != 0) {
        return -1;
    }
    state->chart_mode = chart_token ? 1 : saved_chart_mode;
    state->chart_kind = chart_token ? (token->level & MDF_CHART_KIND_MASK) : saved_chart_kind;
    if (chart_token) {
        state->chart_vertical_axis_col_valid = 0;
        state->chart_vertical_axis_col = 0;
    }
    if (chart_token && mdf_write_cstr(sink, "<div class=\"mdf-chart-block\" style=\"width:100%;text-align:center;\"><span class=\"mdf-chart-box\" style=\"display:inline-block;text-align:left;\">") != 0) {
        state->chart_mode = saved_chart_mode;
        state->chart_kind = saved_chart_kind;
        state->chart_vertical_axis_col_valid = saved_chart_vertical_axis_col_valid;
        state->chart_vertical_axis_col = saved_chart_vertical_axis_col;
        return -1;
    }
    html_bridge_scope_begin(self, sink, &bridge);
    if (chart_token) {
        chart_width = (int)(impl->opts.html_content_width_ch + 0.5);
        if (chart_width > 0) {
            impl->opts.width = chart_width;
        }
        impl->chart_suppress_centering = 1;
        if (bridge.save_boring) {
            impl->opts.boring = 1;
            bridge.bridge.boring = 1;
        }
    }
    rc = 0;
    if (ansi_write_token(self, token, &bridge.sink) != 0) {
        rc = -1;
    } else if (html_should_flush_ansi_word_after_text(impl, token) &&
               ansi_flush_word(impl, &bridge.sink) != 0) {
        rc = -1;
    }
    if (rc == 0 && chart_token && html_flush_pending_lines(self, sink, 0, '\n') != 0) {
        rc = -1;
    }
    html_bridge_scope_end(&bridge);
    if (chart_token && mdf_write_cstr(sink, "</span></div>") != 0) {
        rc = -1;
    }
    state->chart_mode = saved_chart_mode;
    state->chart_kind = saved_chart_kind;
    state->chart_vertical_axis_col_valid = saved_chart_vertical_axis_col_valid;
    state->chart_vertical_axis_col = saved_chart_vertical_axis_col;
    return rc;
}

int html_bridge_run_ansi_newline(mdf_renderer *self, mdf_sink *sink)
{
    mdf_impl *impl;
    html_bridge_scope bridge;

    impl = (mdf_impl *)self->impl;
    html_bridge_scope_begin(self, sink, &bridge);
    if (ansi_write_newline(impl, &bridge.sink) != 0) {
        html_bridge_scope_end(&bridge);
        return -1;
    }
    html_bridge_scope_end(&bridge);
    return 0;
}

void html_state_reset(html_state *state)
{
    memset(state, 0, sizeof(*state));
    state->fg = -1;
    state->bg = -1;
    state->stream_fg = -1;
    state->stream_bg = -1;
}

void html_state_release(mdf_impl *impl, html_state *state)
{
    size_t i;

    for (i = 0; i < state->segment_count; i++) {
        html_segment_free(&impl->allocator, &state->segments[i]);
    }
    mdf_free_mem(&impl->allocator, state->segments, state->segment_cap * sizeof(state->segments[0]));
    mdf_free_mem(&impl->allocator, state->line_plain, state->line_plain_cap);
    mdf_free_mem(&impl->allocator, state->osc_buf, state->osc_cap);
    mdf_free_mem(&impl->allocator, state->link, state->link_cap);
    mdf_free_mem(&impl->allocator, state->stream_link, state->stream_link_cap);
    html_state_reset(state);
}

void html_state_destroy(mdf_impl *impl, html_state **state_ptr)
{
    if (impl == NULL || state_ptr == NULL || *state_ptr == NULL) {
        return;
    }
    html_state_release(impl, *state_ptr);
    mdf_free_mem(&impl->allocator, *state_ptr, sizeof(**state_ptr));
    *state_ptr = NULL;
}

int html_parse_ansi_inline(mdf_impl *impl, html_state *state, const char *src, size_t len)
{
    size_t i;
    char ch;

    for (i = 0; i < len; i++) {
        ch = src[i];
        switch (state->mode) {
        case 0:
            if (ch == '\033') {
                state->mode = 1;
            } else if (ch != '\r') {
                if (html_state_append_text(impl, state, &ch, 1) != 0) return -1;
            }
            break;
        case 1:
            if (ch == '[') {
                state->mode = 2;
                state->csi_len = 0;
            } else if (ch == ']') {
                state->mode = 3;
                state->osc_len = 0;
                if (state->osc_buf != NULL) {
                    state->osc_buf[0] = '\0';
                }
            } else {
                state->mode = 0;
            }
            break;
        case 2:
            if (state->csi_len + 1 < sizeof(state->csi_buf)) {
                state->csi_buf[state->csi_len++] = ch;
                state->csi_buf[state->csi_len] = '\0';
            }
            if (ch >= '@' && ch <= '~') {
                if (ch == 'm' && state->csi_len > 0) {
                    html_state_parse_sgr(state, state->csi_buf, state->csi_len - 1);
                }
                state->mode = 0;
                state->csi_len = 0;
            }
            break;
        case 3:
            if (ch == '\007') {
                html_state_parse_osc(impl, state);
                state->mode = 0;
            } else if (ch == '\033') {
                state->mode = 4;
            } else if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, &ch, 1) != 0) {
                return -1;
            }
            break;
        case 4:
            if (ch == '\\') {
                html_state_parse_osc(impl, state);
                state->mode = 0;
            } else {
                if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, "\033", 1) != 0) return -1;
                if (html_buf_append(&impl->allocator, &state->osc_buf, &state->osc_len, &state->osc_cap, &ch, 1) != 0) return -1;
                state->mode = 3;
            }
            break;
        }
    }
    return 0;
}

int html_emit_inline_state(mdf_impl *impl, html_state *state, mdf_sink *sink, int header)
{
    size_t i;

    for (i = 0; i < state->segment_count; i++) {
        html_segment *seg;
        int fg;
        int bold;
        int italic;
        int underline;
        int use_span;
        int group_text;
        int bracket_group;
        seg = &state->segments[i];
        fg = header ? 36 : seg->fg;
        bold = header ? 1 : seg->bold;
        italic = seg->italic;
        underline = seg->underline || seg->link_len > 0;
        use_span = 0;
        group_text = html_segment_needs_grouped_table_text(state, i, header, &bracket_group);
        if (header) {
            if (seg->bold || seg->italic || seg->underline || seg->fg != -1 || seg->bg != -1 || seg->link_len > 0) {
                use_span = 1;
            }
        } else if (seg->bold || seg->italic || seg->underline || seg->fg != -1 || seg->bg != -1) {
            use_span = 1;
        }
        if (seg->link_len > 0) {
            if (html_write_open_link_grouped(impl, sink, seg->link, seg->link_len) != 0) return -1;
        }
        if (use_span) {
            if (mdf_write_cstr(sink, "<span style=\"") != 0 ||
                html_write_style_effective(impl, sink, fg, seg->bg, bold, italic, underline, 0) != 0 ||
                mdf_write_cstr(sink, "\">") != 0) {
                return -1;
            }
        }
        if (group_text) {
            if (bracket_group && mdf_write_all(sink, seg->text, 1) != 0) {
                return -1;
            }
            if (html_write_grouped_escaped(impl,
                                           sink,
                                           seg->text + (bracket_group ? 1 : 0),
                                           seg->len - (bracket_group ? 2 : 0)) != 0) return -1;
            if (bracket_group && mdf_write_all(sink, seg->text + seg->len - 1, 1) != 0) return -1;
        } else {
            if (!header &&
                seg->fg == -1 &&
                seg->bg == -1 &&
                !seg->bold &&
                !seg->italic &&
                !seg->underline &&
                seg->link_len == 0) {
                if (html_emit_plain_segment_table(impl, sink, seg->text, seg->len) != 0) return -1;
            } else {
                if (html_escape_styled_cells(impl,
                                             sink,
                                             seg->text,
                                             seg->len,
                                             fg,
                                             bold,
                                             state->chart_mode) != 0) return -1;
            }
        }
        if (use_span) {
            if (mdf_write_cstr(sink, "</span>") != 0) return -1;
        }
        if (seg->link_len > 0) {
            if (mdf_write_cstr(sink, "</a>") != 0) return -1;
        }
    }
    return 0;
}

int html_write_token(mdf_renderer *self, const mdf_token *token, mdf_sink *sink)
{
    mdf_impl *impl;
    html_state *state;
    mdf_sink discard_sink;

    impl = (mdf_impl *)self->impl;
    if (html_start(self, sink) != 0) return -1;
    if (impl->html_footer_needs_newline &&
        token->type != MDF_TOKEN_DOCUMENT_END &&
        token->type != MDF_TOKEN_NEWLINE &&
        token->type != MDF_TOKEN_PARAGRAPH_END) {
        if (mdf_write_cstr(sink, "\n") != 0) {
            return -1;
        }
        impl->html_footer_needs_newline = 0;
    }
    if (token->type != MDF_TOKEN_DOCUMENT_END) {
        impl->html_body_emitted = 1;
    }
    state = html_state_get(impl);
    if (state == NULL) {
        return -1;
    }
    discard_sink.userdata = NULL;
    discard_sink.write = discard_write;
    if (state->direct_prefix_open) {
        if (token->type == MDF_TOKEN_LIST_ITEM_END) {
            if (ansi_write_token(self, token, &discard_sink) != 0) {
                return -1;
            }
            return 0;
        }
        if (html_direct_close_prefix_line(sink, token->type == MDF_TOKEN_DOCUMENT_END) != 0) {
            return -1;
        }
        state->direct_prefix_open = 0;
        if (token->type == MDF_TOKEN_DOCUMENT_END) {
            return 0;
        }
    }
    if (token->type == MDF_TOKEN_NEWLINE && token->level < 0) {
        if (ansi_write_token(self, token, &discard_sink) != 0) {
            return -1;
        }
        return 0;
    }
    return html_bridge_run_token(self, sink, token);
}
