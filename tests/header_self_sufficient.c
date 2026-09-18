#include <libmdf/mdf.h>

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    mdf_format format;
    mdf_sink sink;

    mdf_options_init(&opts);
    renderer = NULL;
    format = MDF_FORMAT_HTML_DECK;
    opts.deck_transition = MDF_DECK_TRANSITION_CROSS;
    opts.slide_numbers = 1;
    opts.deck_center_front_text = 1;
    sink.userdata = NULL;
    sink.write = NULL;
    if (opts.width != 80) {
        return 1;
    }
    if (format != MDF_FORMAT_HTML_DECK ||
        opts.deck_transition != MDF_DECK_TRANSITION_CROSS ||
        opts.slide_numbers != 1 ||
        opts.deck_center_front_text != 1) {
        return 1;
    }
    (void)mdf_feed(renderer, "x", 1, &sink);
    (void)mdf_flush(renderer, &sink);
    (void)mdf_finish_document(renderer, &sink);
    (void)renderer->begin_document(renderer);
    return mdf_create(format, &opts, &renderer) == MDF_OK && renderer != NULL ? 0 : 1;
}
