#include "cmdf_fonts.h"

#include "html_embedded/jetbrains_regular.h"
#include "html_embedded/jetbrains_italic.h"

void cmdf_enable_embedded_fonts(mdf_options *opts)
{
    if (opts == NULL) {
        return;
    }
    opts->html_font.family = "JetBrains Mono";
    opts->html_font.regular.format = MDF_HTML_FONT_FORMAT_WOFF2;
    opts->html_font.regular.data = libmdf_html_jetbrains_regular;
    opts->html_font.regular.data_len = (size_t)libmdf_html_jetbrains_regular_len;
    opts->html_font.italic.format = MDF_HTML_FONT_FORMAT_WOFF2;
    opts->html_font.italic.data = libmdf_html_jetbrains_italic;
    opts->html_font.italic.data_len = (size_t)libmdf_html_jetbrains_italic_len;
}
