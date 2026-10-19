#include <libmdf/mdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
    char *data;
    size_t len;
    int fail;
    int fail_footer;
} capture;

typedef struct fail_allocator {
    size_t calls;
    size_t fail_at;
    size_t live;
} fail_allocator;

static void *fail_alloc(void *userdata, size_t size)
{
    fail_allocator *state;
    void *ptr;

    state = (fail_allocator *)userdata;
    if (state->calls++ == state->fail_at) return NULL;
    ptr = malloc(size);
    if (ptr != NULL) state->live++;
    return ptr;
}

static void fail_free(void *userdata, void *ptr, size_t size)
{
    fail_allocator *state;

    (void)size;
    state = (fail_allocator *)userdata;
    if (ptr != NULL) state->live--;
    free(ptr);
}

static size_t occurrences(const char *text, const char *needle)
{
    size_t count;

    count = 0;
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += strlen(needle);
    }
    return count;
}

static int write_capture(void *userdata, const char *data, size_t len)
{
    capture *out;
    char *next;

    out = (capture *)userdata;
    if (out->fail || (out->fail_footer && len >= 7 && memcmp(data, "<footer", 7) == 0)) return -1;
    next = (char *)realloc(out->data, out->len + len + 1);
    if (next == NULL) return -1;
    out->data = next;
    memcpy(out->data + out->len, data, len);
    out->len += len;
    out->data[out->len] = '\0';
    return 0;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "C89 check failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void)
{
    static const unsigned char font[] = {1, 2, 3};
    static const struct {
        const char *font_uri;
        const char *mark_uri;
    } uri_cases[] = {
        {"fonts/regular.woff2?src=https://cdn.example", "fonts/c89-mark.svg?src=https://cdn.example"},
        {"fonts/regular.woff2#src=https://cdn.example", "fonts/c89-mark.svg#src=https://cdn.example"},
        {"regular.woff2?src=https://cdn.example", "c89-mark.svg?src=https://cdn.example"},
        {"//example.test/regular.woff2", "//example.test/c89-mark.svg"},
        {"https://example.test?src=https://cdn.example", "https://example.test/c89-mark.svg?src=https://cdn.example"},
        {"fonts/file://regular.woff2", "fonts/file://c89-mark.svg"}
    };
    const char *markdown;
    mdf_options opts;
    mdf *renderer;
    mdf *reference;
    mdf_sink sink;
    capture out;
    char *expected;
    char *rendered;
    char mark[4096];
    char logo[4096];
    size_t i;
    int document;
    fail_allocator allocation;
    mdf_status status;

    markdown = "# Test\n\n[DOI](https://doi.org/10.1016/0048-7333(86)90027-2)\n\n## Next\n\nText.\n\n| Name | Value |\n| --- | --- |\n| **Table** | Cell |\n";
    mdf_options_init(&opts);
    CHECK(opts.html_c89 == 0);
    opts.html_c89 = 1;
    opts.html_content_width_ch = 72;
    opts.html_font.family = "Test Font";
    opts.html_font.regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
    opts.html_font.regular.data = font;
    opts.html_font.regular.data_len = sizeof(font);
    CHECK(mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) == MDF_ERROR_INVALID && renderer == NULL);
    CHECK(mdf_create(MDF_FORMAT_HTML_DECK, &opts, &renderer) == MDF_ERROR_INVALID && renderer == NULL);
    CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
    CHECK(mdf_set_html_title(renderer, "Test") == MDF_OK);
    CHECK(renderer->render_cstr(renderer, markdown, &expected) == MDF_OK);
    CHECK(strstr(expected, "data:image/svg+xml;base64,") != NULL);
    CHECK(strstr(expected, "<header class=\"lights-out-banner\">") != NULL);
    CHECK(strstr(expected, "<main class=\"mdf-document\" id=\"top\">") != NULL);
    CHECK(strstr(expected, "</main>\n<footer class=\"lights-out-footer\">") != NULL);
    CHECK(strstr(expected, "C89 Systems AB") != NULL);
    CHECK(strstr(expected, "color:rgb(215,215,255);font-weight:700;font-size:32pt;") != NULL);
    CHECK(strstr(expected, "color:rgb(135,175,215);font-weight:700;font-size:22pt;") != NULL);
    CHECK(strstr(expected, "mailto:info@c89.systems") != NULL);
    CHECK(strstr(expected, "--mdf-content-max-width:72ch") != NULL);
    CHECK(strstr(expected, "font-family:\"Test Font\"") != NULL);
    CHECK(strstr(expected, "font-family:\"JetBrains Mono\"") == NULL);
    CHECK(strstr(expected, "opacity:.1;pointer-events:none") != NULL);
    CHECK(strstr(expected, "prefers-reduced-motion") != NULL);
    CHECK(strstr(expected, "<table class=\"mdf-table") != NULL && strstr(expected, "</table>") != NULL);
    CHECK(strstr(expected, "addEventListener('click'") != NULL);
    CHECK(strstr(expected, "href=\"https://doi.org/10.1016/0048-7333(86)90027-2\"") != NULL);
    CHECK(strstr(expected, "</script>\n</body>\n</html>\n") != NULL);
    reference = renderer;

    /* Real incremental rendering must reproduce both wrappers exactly once,
     * including when reusing the same renderer for another document. */
    CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
    CHECK(mdf_set_html_title(renderer, "Test") == MDF_OK);
    for (document = 0; document < 2; document++) {
        memset(&out, 0, sizeof(out));
        sink.userdata = &out;
        sink.write = write_capture;
        for (i = 0; markdown[i] != '\0'; i++) CHECK(mdf_feed(renderer, markdown + i, 1, &sink) == MDF_OK);
        CHECK(mdf_finish_document(renderer, &sink) == MDF_OK);
        CHECK(strcmp(expected, out.data) == 0);
        free(out.data);
        CHECK(mdf_begin_document(renderer) == MDF_OK);
    }
    memset(&out, 0, sizeof(out));
    out.fail = 1;
    CHECK(mdf_feed(renderer, markdown, strlen(markdown), &sink) == MDF_ERROR_IO);
    CHECK(mdf_finish_document(renderer, &sink) != MDF_OK);
    renderer->destroy(renderer);
    reference->string_free(reference, expected);
    reference->destroy(reference);

    CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
    CHECK(mdf_set_html_title(renderer, "Test") == MDF_OK);
    memset(&out, 0, sizeof(out));
    out.fail_footer = 1;
    CHECK(mdf_feed(renderer, markdown, strlen(markdown), &sink) == MDF_OK);
    CHECK(mdf_finish_document(renderer, &sink) == MDF_ERROR_IO);
    CHECK(strstr(out.data, "</main>") != NULL && strstr(out.data, "</html>") == NULL);
    CHECK(mdf_finish_document(renderer, &sink) != MDF_OK);
    free(out.data);
    renderer->destroy(renderer);

    mdf_options_init(&opts);
    opts.html_c89 = 1;
    opts.html_font_uri = "https://example.test/fonts?v=1&x=2#face";
    CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
    CHECK(renderer->render_cstr(renderer, "", &rendered) == MDF_OK);
    CHECK(strstr(rendered, "url(\"https://example.test/fonts/c89-mark.svg?v=1&x=2#face\")") != NULL);
    CHECK(strstr(rendered, "src=\"https://example.test/fonts/c89-systems.svg?v=1&amp;x=2#face\"") != NULL);
    CHECK(strstr(rendered, "data:image/svg") == NULL);
    CHECK(strstr(rendered, "data:font/") == NULL);
    renderer->string_free(renderer, rendered);
    renderer->destroy(renderer);

    for (i = 0; i < sizeof(uri_cases) / sizeof(uri_cases[0]); i++) {
        mdf_options_init(&opts);
        opts.html_c89 = 1;
        opts.html_font_regular_uri = uri_cases[i].font_uri;
        CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
        CHECK(renderer->render_cstr(renderer, "", &rendered) == MDF_OK);
        sprintf(mark, "url(\"%s\")", uri_cases[i].mark_uri);
        CHECK(strstr(rendered, mark) != NULL);
        renderer->string_free(renderer, rendered);
        renderer->destroy(renderer);
    }

    mdf_options_init(&opts);
    opts.html_c89 = 1;
    opts.html_font_uri = "assets";
    opts.html_dump_font = 1;
    CHECK(mdf_html_c89_resolve_dump_paths(&opts, mark, sizeof(mark), logo, sizeof(logo)) == MDF_OK);
    CHECK(strcmp(mark, "assets/c89-mark.svg") == 0 && strcmp(logo, "assets/c89-systems.svg") == 0);
    CHECK(mdf_html_c89_resolve_dump_paths(&opts, mark, 1, logo, sizeof(logo)) == MDF_ERROR_INVALID);
    opts.html_font_dump_regular_path = "assets/c89-mark.svg";
    CHECK(mdf_html_c89_resolve_dump_paths(&opts, mark, sizeof(mark), logo, sizeof(logo)) == MDF_ERROR_INVALID);
    opts.html_c89 = 0;
    CHECK(mdf_html_c89_resolve_dump_paths(&opts, mark, sizeof(mark), logo, sizeof(logo)) == MDF_ERROR_INVALID);

    /* Navigation's selector must select only parser headings, regardless of
     * theme styling or heading-like code and escaped paragraph text. */
    for (i = 0; i < mdf_theme_count(); i++) {
        mdf_options_init(&opts);
        opts.html_c89 = 1;
        opts.theme_name = mdf_theme_name(i);
        opts.html_font.family = "Test Font";
        opts.html_font.regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
        opts.html_font.regular.data = font;
        opts.html_font.regular.data_len = sizeof(font);
        CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
        CHECK(renderer->render_cstr(renderer,
            "# Actual\n\n\\# Escaped paragraph\n\n```sh\n# comment\n## deeper comment\n```\n\n> ## Quoted\n\n## End\n", &rendered) == MDF_OK);
        if (strcmp(opts.theme_name, "default") == 0) {
            CHECK(strstr(rendered, "color:rgb(0,205,0);font-weight:700;font-size:32pt;") != NULL);
        }
        CHECK(occurrences(rendered, " data-mdf-heading=") == 3);
        CHECK(occurrences(rendered, " data-mdf-heading=\"1\"") == 1);
        CHECK(occurrences(rendered, " data-mdf-heading=\"2\"") == 2);
        CHECK(strstr(rendered, "querySelectorAll('.mdf-heading[data-mdf-heading]')") != NULL);
        renderer->string_free(renderer, rendered);
        renderer->destroy(renderer);
    }

    /* Fail each constructor allocation, including the copied SVG URIs. */
    mdf_options_init(&opts);
    opts.html_c89 = 1;
    opts.html_font_uri = "assets";
    opts.allocator.userdata = &allocation;
    opts.allocator.alloc = fail_alloc;
    opts.allocator.free = fail_free;
    for (i = 0; i < 64; i++) {
        memset(&allocation, 0, sizeof(allocation));
        allocation.fail_at = i;
        status = mdf_create(MDF_FORMAT_HTML, &opts, &renderer);
        CHECK(status == MDF_OK || status == MDF_ERROR_NOMEM);
        if (status == MDF_OK) renderer->destroy(renderer);
        else CHECK(renderer == NULL);
        CHECK(allocation.live == 0);
        if (status == MDF_OK) break;
    }
    CHECK(i < 64);
    return 0;
}
