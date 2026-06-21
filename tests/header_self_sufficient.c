#include <libmdf/mdf.h>

int main(void)
{
    mdf_options opts;

    mdf_options_init(&opts);
    return opts.width == 80 ? 0 : 1;
}
