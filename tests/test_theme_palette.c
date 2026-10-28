#include <libmdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "theme palette check failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static void fields(const mdf_theme_palette *palette, const char **out)
{
    int i;

    for (i = 0; i < 6; i++) out[i] = palette->heading[i];
    out[6] = palette->emphasis;
    out[7] = palette->strong;
    out[8] = palette->emphasis_strong;
    out[9] = palette->code_inline;
    out[10] = palette->code_block;
    out[11] = palette->quote;
    out[12] = palette->quote_text;
    out[13] = palette->list_marker;
    out[14] = palette->link_text;
    out[15] = palette->link_url;
    out[16] = palette->thematic_break;
    out[17] = palette->table_header;
    out[18] = palette->table_wire;
}

/* Accept exactly one foreground SGR, including indexed/RGB colors, or empty. */
static int foreground_only(const char *text)
{
    unsigned int code;
    unsigned int mode;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    int end;

    if (*text == '\0') return 1;
    end = 0;
    if (sscanf(text, "\033[%um%n", &code, &end) == 1 && end > 0 && text[end] == '\0')
        return (code >= 30 && code <= 37) || (code >= 90 && code <= 97);
    end = 0;
    if (sscanf(text, "\033[38;%u;%um%n", &mode, &a, &end) == 2 && end > 0 && text[end] == '\0')
        return mode == 5 && a <= 255;
    end = 0;
    return sscanf(text, "\033[38;2;%u;%u;%um%n", &a, &b, &c, &end) == 3 &&
           end > 0 && text[end] == '\0' && a <= 255 && b <= 255 && c <= 255;
}

static void *count_alloc(void *userdata, size_t size)
{
    size_t *calls;

    calls = (size_t *)userdata;
    (*calls)++;
    return malloc(size);
}

static void count_free(void *userdata, void *ptr, size_t size)
{
    (void)userdata;
    (void)size;
    free(ptr);
}

int main(void)
{
    static const char *expected[] = {
        "\033[32m", "\033[34m", "\033[35m", "\033[33m", "\033[36m", "\033[37m",
        "\033[34m", "\033[37m", "\033[35m", "\033[35m", "\033[32m", "\033[90m", "",
        "\033[36m", "\033[34m", "\033[90m", "\033[90m", "\033[36m", "\033[90m"
    };
    mdf_theme_palette palette;
    mdf_theme_palette reference;
    mdf_theme_palette saved;
    mdf_options opts;
    mdf *renderer;
    mdf invalid;
    const char *colors[19];
    char upper[128];
    char foreground[MDF_THEME_FOREGROUND_SIZE];
    char guard[MDF_THEME_FOREGROUND_SIZE];
    char *rendered;
    const char *name;
    size_t i;
    size_t j;
    size_t calls;
    size_t before;
    int slot;
    int level;

    CHECK(mdf_theme_get_palette(NULL, &reference) == MDF_OK);
    CHECK(strcmp(reference.name, "default") == 0);
    fields(&reference, colors);
    for (slot = 0; slot < 19; slot++) CHECK(strcmp(colors[slot], expected[slot]) == 0);
    CHECK(mdf_theme_get_palette("", &palette) == MDF_OK);
    CHECK(memcmp(&palette, &reference, sizeof(palette)) == 0);
    CHECK(mdf_theme_get_palette("DeFaUlT", &palette) == MDF_OK);
    CHECK(memcmp(&palette, &reference, sizeof(palette)) == 0);
    for (level = 1; level <= 6; level++) {
        CHECK(mdf_theme_heading_foreground(NULL, level, foreground, sizeof(foreground)) == MDF_OK);
        CHECK(strcmp(foreground, reference.heading[level - 1]) == 0);
        CHECK(mdf_theme_heading_foreground("", level, foreground, sizeof(foreground)) == MDF_OK);
    }
    saved = palette;
    CHECK(mdf_theme_get_palette("missing", &palette) == MDF_ERROR_INVALID);
    CHECK(memcmp(&palette, &saved, sizeof(palette)) == 0);
    CHECK(mdf_theme_get_palette(NULL, NULL) == MDF_ERROR_INVALID);
    CHECK(mdf_get_theme_palette(NULL, &palette) == MDF_ERROR_INVALID);
    memset(&invalid, 0, sizeof(invalid));
    CHECK(mdf_get_theme_palette(&invalid, &palette) == MDF_ERROR_INVALID);
    CHECK(memcmp(&palette, &saved, sizeof(palette)) == 0);
    memset(guard, 'X', sizeof(guard));
    CHECK(mdf_theme_heading_foreground("missing", 1, guard, sizeof(guard)) == MDF_ERROR_INVALID);
    CHECK(mdf_theme_heading_foreground(NULL, 0, guard, sizeof(guard)) == MDF_ERROR_INVALID);
    CHECK(mdf_theme_heading_foreground(NULL, 7, guard, sizeof(guard)) == MDF_ERROR_INVALID);
    CHECK(mdf_theme_heading_foreground(NULL, 1, NULL, sizeof(guard)) == MDF_ERROR_INVALID);
    CHECK(mdf_theme_heading_foreground(NULL, 1, guard, 0) == MDF_ERROR_INVALID);
    CHECK(mdf_theme_heading_foreground(NULL, 1, guard, 5) == MDF_ERROR_INVALID);
    for (i = 0; i < sizeof(guard); i++) CHECK(guard[i] == 'X');
    CHECK(mdf_theme_heading_foreground(NULL, 1, guard, 6) == MDF_OK);
    CHECK(strcmp(guard, "\033[32m") == 0 && guard[6] == 'X');

    for (i = 0; i < mdf_theme_count(); i++) {
        name = mdf_theme_name(i);
        CHECK(strlen(name) < sizeof(upper));
        for (j = 0; name[j] != '\0'; j++)
            upper[j] = name[j] >= 'a' && name[j] <= 'z' ? (char)(name[j] - 'a' + 'A') : name[j];
        upper[j] = '\0';
        CHECK(mdf_theme_get_palette(upper, &palette) == MDF_OK);
        CHECK(strcmp(palette.name, name) == 0);
        fields(&palette, colors);
        for (slot = 0; slot < 19; slot++) {
            CHECK(strlen(colors[slot]) < MDF_THEME_FOREGROUND_SIZE);
            CHECK(foreground_only(colors[slot]));
        }
        for (level = 1; level <= 6; level++) {
            CHECK(mdf_theme_heading_foreground(upper, level, foreground, sizeof(foreground)) == MDF_OK);
            CHECK(strcmp(foreground, palette.heading[level - 1]) == 0);
            CHECK(mdf_theme_heading_foreground(name, level, foreground, strlen(foreground)) == MDF_ERROR_INVALID);
        }
        calls = 0;
        mdf_options_init(&opts);
        opts.theme_name = upper;
        opts.ansi_mode = MDF_ANSI_OFF;
        opts.boring = 1;
        opts.allocator.userdata = &calls;
        opts.allocator.alloc = count_alloc;
        opts.allocator.free = count_free;
        CHECK(mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) == MDF_OK);
        before = calls;
        CHECK(renderer->get_theme_palette != NULL);
        CHECK(renderer->get_theme_palette(renderer, &reference) == MDF_OK);
        CHECK(mdf_get_theme_palette(renderer, NULL) == MDF_ERROR_INVALID);
        CHECK(calls == before);
        CHECK(memcmp(&reference, &palette, sizeof(palette)) == 0);
        CHECK(mdf_get_theme_palette(renderer, &reference) == MDF_OK);
        CHECK(calls == before);
        CHECK(renderer->render_cstr(renderer, "## Still uncolored\n", &rendered) == MDF_OK);
        CHECK(strchr(rendered, '\033') == NULL);
        renderer->string_free(renderer, rendered);
        renderer->destroy(renderer);
        CHECK(strcmp(reference.name, name) == 0);
        palette.heading[0][0] = '\0';
        CHECK(mdf_theme_get_palette(name, &palette) == MDF_OK);
        CHECK(memcmp(&reference, &palette, sizeof(palette)) == 0);
    }
    mdf_options_init(&opts);
    opts.html_c89 = 1;
    CHECK(mdf_create(MDF_FORMAT_HTML, &opts, &renderer) == MDF_OK);
    CHECK(renderer->get_theme_palette(renderer, &palette) == MDF_OK);
    CHECK(strcmp(palette.name, "one-dark") == 0);
    CHECK(strcmp(palette.heading[0], "\033[38;5;189m") == 0);
    CHECK(strcmp(palette.heading[1], "\033[38;5;110m") == 0);
    CHECK(strcmp(palette.heading[2], "\033[38;5;147m") == 0);
    renderer->destroy(renderer);
    puts("all theme palette checks passed");
    return 0;
}
