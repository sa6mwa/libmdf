#include <libmdf.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    char *output;
    mdf_status status;
    int valid;
    mdf_options_init(&opts);
    opts.ansi_mode = MDF_ANSI_OFF;
    renderer = NULL;
    output = NULL;
    status = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (status == MDF_OK) status = renderer->render_cstr(renderer, "# Package\n", &output);
    valid = status == MDF_OK && output != NULL && strcmp(output, "# Package\n") == 0;
    if (output != NULL) renderer->string_free(renderer, output);
    if (renderer != NULL) renderer->destroy(renderer);
    if (!valid) return 1;
    puts(LIBMDF_VERSION);
    return 0;
}
