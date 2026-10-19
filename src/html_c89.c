#include "mdf_internal.h"
#include "c89_assets.h"

const unsigned char *mdf_html_c89_asset(int asset, size_t *len)
{
    switch (asset) {
    case MDF_C89_MARK:
        *len = sizeof(c89_mark_svg);
        return c89_mark_svg;
    case MDF_C89_LOGO:
        *len = sizeof(c89_systems_svg);
        return c89_systems_svg;
    case MDF_C89_CSS:
        *len = sizeof(style_css);
        return style_css;
    default:
        *len = sizeof(navigation_js);
        return navigation_js;
    }
}
