#include <libmdf/mdf.h>

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    mdf_format format;

    mdf_options_init(&opts);
    renderer = NULL;
    format = MDF_FORMAT_HTML_DECK;
    opts.deck_transition = MDF_DECK_TRANSITION_CROSS;
    opts.slide_numbers = 1;
    opts.deck_center_front_text = 1;
    if (opts.width != 80) {
        return 1;
    }
    if (format != MDF_FORMAT_HTML_DECK ||
        opts.deck_transition != MDF_DECK_TRANSITION_CROSS ||
        opts.slide_numbers != 1 ||
        opts.deck_center_front_text != 1) {
        return 1;
    }
    return mdf_create(format, &opts, &renderer) == MDF_OK && renderer != NULL ? 0 : 1;
}
