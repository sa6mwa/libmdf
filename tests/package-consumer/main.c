#include <libmdf.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    mdf_options opts;
    mdf_theme_palette palette;
    char foreground[MDF_THEME_FOREGROUND_SIZE];
    mdf *renderer;
    char *output;
    mdf_status status;
    int valid;
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    renderer = NULL;
    output = NULL;
    status = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (status == MDF_OK) status = renderer->get_theme_palette(renderer, &palette);
    if (status == MDF_OK) status = mdf_theme_get_palette(palette.name, &palette);
    if (status == MDF_OK) status = mdf_get_theme_palette(renderer, &palette);
    if (status == MDF_OK) status = mdf_theme_heading_foreground(NULL, 6, foreground, sizeof(foreground));
    if (status == MDF_OK && (strcmp(palette.heading[5], foreground) != 0 ||
        strcmp(palette.quote_text, "") != 0)) status = MDF_ERROR_INVALID;
    if (status == MDF_OK) status = renderer->render_cstr(renderer, "# Package\n", &output);
    valid = status == MDF_OK && output != NULL && strcmp(output, "# Package\n") == 0;
    if (output != NULL) renderer->string_free(renderer, output);
    if (renderer != NULL) renderer->destroy(renderer);
    if (!valid) return 1;
    puts(LIBMDF_VERSION);
    return 0;
}
