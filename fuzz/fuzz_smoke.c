#include <libmdf/mdf.h>

#include <stddef.h>
#include <string.h>

typedef struct fuzz_source {
    const char *src;
    size_t len;
    size_t off;
} fuzz_source;

static size_t fuzz_read(void *userdata, char *dst, size_t cap, int *err)
{
    fuzz_source *src;
    size_t n;

    (void)err;
    src = (fuzz_source *)userdata;
    if (src->off >= src->len) {
        return 0;
    }
    n = src->len - src->off;
    if (n > cap) {
        n = cap;
    }
    if (n != 0) {
        memcpy(dst, src->src + src->off, n);
        src->off += n;
    }
    return n;
}

static int discard_write(void *userdata, const char *src, size_t len)
{
    (void)userdata;
    (void)src;
    (void)len;
    return 0;
}

static void fuzz_one_format(mdf_format format, const unsigned char *data, size_t size)
{
    mdf *inst;
    mdf_options opts;
    fuzz_source src_data;
    mdf_source src;
    mdf_sink sink;

    mdf_options_init(&opts);
    opts.width = 20 + (int)(size == 0 ? 0 : (data[0] % 101));
    opts.boring = size > 1 ? ((data[1] & 1U) ? 1 : 0) : 1;
    opts.osc8 = size > 2 ? ((data[2] & 1U) ? 1 : 0) : 0;
    opts.table_buffer_mode = (size > 3 && (data[3] & 1U)) ? MDF_TABLE_BUFFER_ROW : MDF_TABLE_BUFFER_FULL;
    opts.table_wire_mode = (size > 4) ? (mdf_table_wire_mode)(data[4] % 3U) : MDF_TABLE_WIRE_LINE;
    inst = 0;
    if (mdf_create(format, &opts, &inst) != MDF_OK) {
        return;
    }
    src_data.src = (const char *)data;
    src_data.len = size;
    src_data.off = 0;
    src.userdata = &src_data;
    src.read = fuzz_read;
    sink.userdata = 0;
    sink.write = discard_write;
    (void)inst->render(inst, &src, &sink);
    inst->destroy(inst);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    fuzz_one_format(MDF_FORMAT_ANSI, data, size);
    fuzz_one_format(MDF_FORMAT_HTML, data, size);
    return 0;
}
