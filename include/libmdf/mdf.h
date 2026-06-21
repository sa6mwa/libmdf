#ifndef LIBMDF_MDF_H
#define LIBMDF_MDF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBMDF_VERSION "0.0.0"
#define LIBMDF_VERSION_MAJOR 0
#define LIBMDF_VERSION_MINOR 0
#define LIBMDF_VERSION_PATCH 0

typedef enum mdf_status {
    MDF_OK = 0,
    MDF_ERROR_INVALID = 1,
    MDF_ERROR_NOMEM = 2,
    MDF_ERROR_IO = 3,
    MDF_ERROR_PARSE = 4
} mdf_status;

typedef enum mdf_format {
    MDF_FORMAT_ANSI = 0,
    MDF_FORMAT_HTML = 1
} mdf_format;

typedef enum mdf_table_buffer_mode {
    MDF_TABLE_BUFFER_FULL = 0,
    MDF_TABLE_BUFFER_ROW = 1
} mdf_table_buffer_mode;

typedef enum mdf_table_wire_mode {
    MDF_TABLE_WIRE_LINE = 0,
    MDF_TABLE_WIRE_ASCII = 1,
    MDF_TABLE_WIRE_SPACE = 2
} mdf_table_wire_mode;

typedef struct mdf_allocator {
    void *userdata;
    void *(*alloc)(void *userdata, size_t size);
    void *(*realloc)(void *userdata, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *userdata, void *ptr, size_t size);
} mdf_allocator;

typedef struct mdf_memory_options {
    size_t max_retained_bytes;
    size_t max_reusable_block_bytes;
} mdf_memory_options;

typedef struct mdf_write_trace {
    void *userdata;
    int (*emit)(void *userdata, mdf_format format, const char *src, size_t len);
} mdf_write_trace;

typedef struct mdf_emission_buffer {
    void *data;
    size_t cap;
    size_t initial_cap;
    size_t max_cap;
    int fixed;
    int take_ownership;
} mdf_emission_buffer;

#define MDF_HTML_FONT_FORMAT_NONE 0
#define MDF_HTML_FONT_FORMAT_WOFF2 1
#define MDF_HTML_FONT_FORMAT_TTF 2

typedef struct mdf_html_font_face {
    int format;
    const unsigned char *data;
    size_t data_len;
    void *userdata;
    size_t (*read)(void *userdata, size_t offset, unsigned char *dst, size_t cap, int *err);
} mdf_html_font_face;

typedef struct mdf_html_font {
    const char *family;
    mdf_html_font_face regular;
    mdf_html_font_face italic;
} mdf_html_font;

typedef struct mdf_options {
    int width;
    int margin_left;
    int margin_right;
    int boring;
    int osc8;
    const char *theme_name;
    double html_content_width_ch;
    mdf_html_font html_font;
    mdf_table_buffer_mode table_buffer_mode;
    mdf_table_wire_mode table_wire_mode;
    mdf_emission_buffer emission_buffer;
    mdf_memory_options memory;
    mdf_write_trace write_trace;
    mdf_allocator allocator;
} mdf_options;

typedef struct mdf_source {
    void *userdata;
    size_t (*read)(void *userdata, char *dst, size_t cap, int *err);
} mdf_source;

typedef struct mdf_sink {
    void *userdata;
    int (*write)(void *userdata, const char *src, size_t len);
} mdf_sink;

typedef enum mdf_token_type {
    MDF_TOKEN_TEXT = 0,
    MDF_TOKEN_SPACE = 1,
    MDF_TOKEN_NEWLINE = 2,
    MDF_TOKEN_PARAGRAPH_END = 3,
    MDF_TOKEN_HEADING_START = 4,
    MDF_TOKEN_HEADING_END = 5,
    MDF_TOKEN_BLOCKQUOTE_START = 6,
    MDF_TOKEN_BLOCKQUOTE_END = 7,
    MDF_TOKEN_LIST_ITEM_START = 8,
    MDF_TOKEN_LIST_ITEM_END = 9,
    MDF_TOKEN_TASK_UNCHECKED = 10,
    MDF_TOKEN_TASK_CHECKED = 11,
    MDF_TOKEN_CODE_BLOCK_START = 12,
    MDF_TOKEN_CODE_BLOCK_END = 13,
    MDF_TOKEN_CODE_TEXT = 14,
    MDF_TOKEN_THEMATIC_BREAK = 15,
    MDF_TOKEN_DOCUMENT_END = 16
} mdf_token_type;

typedef struct mdf_token {
    mdf_token_type type;
    const char *text;
    size_t len;
    int level;
} mdf_token;

typedef struct mdf mdf;

struct mdf {
    mdf_status (*write_token)(mdf *self, const mdf_token *token, mdf_sink *sink);
    mdf_status (*finish)(mdf *self, mdf_sink *sink);
    mdf_status (*render)(mdf *self, mdf_source *source, mdf_sink *sink);
    mdf_status (*render_cstr)(mdf *self, const char *markdown, char **out);
    const char *(*error)(const mdf *self);
    void (*destroy)(mdf *self);
    void (*string_free)(mdf *self, char *s);
    void *impl;
};

void mdf_options_init(mdf_options *opts);
mdf_status mdf_create(mdf_format format, const mdf_options *opts, mdf **out);
const char *mdf_status_string(mdf_status status);
size_t mdf_theme_count(void);
const char *mdf_theme_name(size_t index);
int mdf_theme_exists(const char *name);
int mdf_detect_osc8_support(void);
int mdf_terminal_width(int fd, int fallback);

#ifdef __cplusplus
}
#endif

#endif
