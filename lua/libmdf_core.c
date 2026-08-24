#include <lua.h>
#include <lauxlib.h>

#include <libmdf/mdf.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct lua_mdf_source_ctx {
    lua_State *L;
    int ref;
} lua_mdf_source_ctx;

typedef struct lua_mdf_sink_ctx {
    lua_State *L;
    int ref;
} lua_mdf_sink_ctx;

typedef struct lua_mdf_trace_ctx {
    lua_State *L;
    int ref;
} lua_mdf_trace_ctx;

typedef struct lua_mdf_handle {
    mdf *mdf;
    int opts_ref;
    lua_mdf_trace_ctx trace_ctx;
} lua_mdf_handle;

#define LUA_MDF_HANDLE "libmdf.mdf"

static int lua_mdf_get_boolean_field(lua_State *L, int table, const char *name);

static const char *lua_mdf_format_name(mdf_format format)
{
    if (format == MDF_FORMAT_HTML) {
        return "html";
    }
    if (format == MDF_FORMAT_HTML_DECK) {
        return "deck";
    }
    return "ansi";
}

static int lua_mdf_trace_emit(void *userdata, mdf_format format, const char *src, size_t len)
{
    lua_mdf_trace_ctx *ctx;
    int ok;

    ctx = (lua_mdf_trace_ctx *)userdata;
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref);
    lua_pushstring(ctx->L, lua_mdf_format_name(format));
    lua_pushlstring(ctx->L, src, len);
    if (lua_pcall(ctx->L, 2, 1, 0) != LUA_OK) {
        return -1;
    }
    ok = lua_isnil(ctx->L, -1) || lua_toboolean(ctx->L, -1);
    lua_pop(ctx->L, 1);
    return ok ? 0 : -1;
}

static mdf_format lua_mdf_format_from_options(lua_State *L, int index)
{
    const char *format;

    if (!lua_istable(L, index)) {
        return MDF_FORMAT_ANSI;
    }
    lua_getfield(L, index, "format");
    if (!lua_isnil(L, -1)) {
        format = luaL_checkstring(L, -1);
        if (strcmp(format, "ansi") == 0) {
            lua_pop(L, 1);
            return MDF_FORMAT_ANSI;
        }
        if (strcmp(format, "html") == 0) {
            lua_pop(L, 1);
            return MDF_FORMAT_HTML;
        }
        if (strcmp(format, "deck") == 0 || strcmp(format, "html_deck") == 0) {
            lua_pop(L, 1);
            return MDF_FORMAT_HTML_DECK;
        }
        return (mdf_format)luaL_error(L, "unknown format: %s", format);
    }
    lua_pop(L, 1);
    if (lua_mdf_get_boolean_field(L, index, "deck")) {
        return MDF_FORMAT_HTML_DECK;
    }
    if (lua_mdf_get_boolean_field(L, index, "html")) {
        return MDF_FORMAT_HTML;
    }
    return MDF_FORMAT_ANSI;
}

static mdf_deck_transition lua_mdf_deck_transition(lua_State *L, int index)
{
    const char *s;

    if (lua_isnoneornil(L, index)) {
        return MDF_DECK_TRANSITION_FADE;
    }
    s = luaL_checkstring(L, index);
    if (strcmp(s, "fade") == 0) {
        return MDF_DECK_TRANSITION_FADE;
    }
    if (strcmp(s, "cross") == 0) {
        return MDF_DECK_TRANSITION_CROSS;
    }
    if (strcmp(s, "hard") == 0) {
        return MDF_DECK_TRANSITION_HARD;
    }
    return (mdf_deck_transition)luaL_error(L, "unknown deck transition: %s", s);
}

static int lua_mdf_font_format(lua_State *L, int index)
{
    const char *s;

    if (lua_isnoneornil(L, index)) {
        return MDF_HTML_FONT_FORMAT_NONE;
    }
    s = luaL_checkstring(L, index);
    if (strcmp(s, "woff2") == 0) {
        return MDF_HTML_FONT_FORMAT_WOFF2;
    }
    if (strcmp(s, "ttf") == 0 || strcmp(s, "truetype") == 0) {
        return MDF_HTML_FONT_FORMAT_TTF;
    }
    return luaL_error(L, "unknown html font format: %s", s);
}

static mdf_table_buffer_mode lua_mdf_table_buffer_mode(lua_State *L, int index)
{
    const char *s;

    if (lua_isnoneornil(L, index)) {
        return MDF_TABLE_BUFFER_FULL;
    }
    s = luaL_checkstring(L, index);
    if (strcmp(s, "full") == 0) {
        return MDF_TABLE_BUFFER_FULL;
    }
    if (strcmp(s, "row") == 0) {
        return MDF_TABLE_BUFFER_ROW;
    }
    return (mdf_table_buffer_mode)luaL_error(L, "unknown table buffer mode: %s", s);
}

static mdf_table_wire_mode lua_mdf_table_wire_mode(lua_State *L, int index)
{
    const char *s;

    if (lua_isnoneornil(L, index)) {
        return MDF_TABLE_WIRE_LINE;
    }
    s = luaL_checkstring(L, index);
    if (strcmp(s, "line") == 0) {
        return MDF_TABLE_WIRE_LINE;
    }
    if (strcmp(s, "ascii") == 0) {
        return MDF_TABLE_WIRE_ASCII;
    }
    if (strcmp(s, "space") == 0) {
        return MDF_TABLE_WIRE_SPACE;
    }
    return (mdf_table_wire_mode)luaL_error(L, "unknown table wire mode: %s", s);
}

static int lua_mdf_get_boolean_field(lua_State *L, int table, const char *name)
{
    int value;

    lua_getfield(L, table, name);
    value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static const char *lua_mdf_get_optional_html_title(lua_State *L, int index)
{
    const char *title;

    if (!lua_istable(L, index)) {
        return NULL;
    }
    lua_getfield(L, index, "html_title");
    title = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return title;
}

static void lua_mdf_apply_options(lua_State *L, int index, mdf_options *opts, lua_mdf_trace_ctx *trace_ctx)
{
    size_t len;
    const char *bytes;

    if (!lua_istable(L, index)) {
        return;
    }
    if (lua_mdf_get_boolean_field(L, index, "boring")) {
        opts->boring = 1;
    }
    lua_getfield(L, index, "osc8");
    if (!lua_isnil(L, -1)) {
        opts->osc8 = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "width");
    if (!lua_isnil(L, -1)) {
        opts->width = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "margin_left");
    if (!lua_isnil(L, -1)) {
        opts->margin_left = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "margin_right");
    if (!lua_isnil(L, -1)) {
        opts->margin_right = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "theme");
    if (!lua_isnil(L, -1)) {
        opts->theme_name = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "html_content_width_ch");
    if (!lua_isnil(L, -1)) {
        opts->html_content_width_ch = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "deck_transition");
    opts->deck_transition = lua_mdf_deck_transition(L, -1);
    lua_pop(L, 1);
    if (lua_mdf_get_boolean_field(L, index, "slide_numbers")) {
        opts->slide_numbers = 1;
    }
    if (lua_mdf_get_boolean_field(L, index, "deck_center_front_text")) {
        opts->deck_center_front_text = 1;
    }
    lua_getfield(L, index, "table_buffer_mode");
    opts->table_buffer_mode = lua_mdf_table_buffer_mode(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "table_wire_mode");
    opts->table_wire_mode = lua_mdf_table_wire_mode(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "write_trace");
    if (!lua_isnil(L, -1)) {
        if (trace_ctx == NULL) {
            luaL_error(L, "write_trace requires a trace context");
        }
        luaL_checktype(L, -1, LUA_TFUNCTION);
        lua_pushvalue(L, -1);
        trace_ctx->L = L;
        trace_ctx->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        opts->write_trace.userdata = trace_ctx;
        opts->write_trace.emit = lua_mdf_trace_emit;
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "html_font");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "family");
        if (!lua_isnil(L, -1)) {
            opts->html_font.family = luaL_checkstring(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "regular_format");
        opts->html_font.regular.format = lua_mdf_font_format(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "regular_data");
        if (!lua_isnil(L, -1)) {
            bytes = luaL_checklstring(L, -1, &len);
            opts->html_font.regular.data = (const unsigned char *)bytes;
            opts->html_font.regular.data_len = len;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "italic_format");
        opts->html_font.italic.format = lua_mdf_font_format(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "italic_data");
        if (!lua_isnil(L, -1)) {
            bytes = luaL_checklstring(L, -1, &len);
            opts->html_font.italic.data = (const unsigned char *)bytes;
            opts->html_font.italic.data_len = len;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static mdf_status lua_mdf_apply_html_font_options(lua_State *L, int index, mdf_format format, mdf_options *opts)
{
    const char *font_uri;
    const char *regular_uri;
    const char *italic_uri;
    int disable_embedded_font;
    int dump_font;
    int dump_font_force;

    if (!lua_istable(L, index) || opts == NULL) {
        return MDF_OK;
    }
    disable_embedded_font = lua_mdf_get_boolean_field(L, index, "disable_embedded_font");
    dump_font = lua_mdf_get_boolean_field(L, index, "dump_font");
    dump_font_force = lua_mdf_get_boolean_field(L, index, "dump_font_force");
    font_uri = NULL;
    regular_uri = NULL;
    italic_uri = NULL;
    lua_getfield(L, index, "font_uri");
    if (!lua_isnil(L, -1)) {
        font_uri = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "font_regular_uri");
    if (!lua_isnil(L, -1)) {
        regular_uri = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "font_italic_uri");
    if (!lua_isnil(L, -1)) {
        italic_uri = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "font_path");
    if (!lua_isnil(L, -1)) {
        opts->html_font_dump_path = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "font_regular_path");
    if (!lua_isnil(L, -1)) {
        opts->html_font_dump_regular_path = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "font_italic_path");
    if (!lua_isnil(L, -1)) {
        opts->html_font_dump_italic_path = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    if ((disable_embedded_font || dump_font || dump_font_force || font_uri != NULL || regular_uri != NULL || italic_uri != NULL ||
         opts->html_font_dump_path != NULL || opts->html_font_dump_regular_path != NULL || opts->html_font_dump_italic_path != NULL) &&
        format != MDF_FORMAT_HTML && format != MDF_FORMAT_HTML_DECK) {
        return MDF_ERROR_INVALID;
    }
    opts->html_font_source = disable_embedded_font ?
        MDF_HTML_FONT_SOURCE_EXTERNAL : MDF_HTML_FONT_SOURCE_EMBEDDED;
    opts->html_font_uri = font_uri;
    opts->html_font_regular_uri = regular_uri;
    opts->html_font_italic_uri = italic_uri;
    opts->html_dump_font = dump_font;
    opts->html_dump_font_force = dump_font_force;
    return MDF_OK;
}

/* Expose the library's path resolution and identity rules so cmdf.lua can
 * reject destructive dump aliases before opening either user file. */
static int lua_mdf_html_font_dump_paths(lua_State *L)
{
    mdf_options opts;
    mdf_status st;
    char regular_path[4096];
    char italic_path[4096];

    luaL_checktype(L, 1, LUA_TTABLE);
    mdf_options_init(&opts);
    st = lua_mdf_apply_html_font_options(L, 1, MDF_FORMAT_HTML, &opts);
    if (st != MDF_OK) {
        return luaL_error(L, "html font dump paths: %s", mdf_status_string(st));
    }
    st = mdf_html_font_resolve_dump_paths(&opts,
                                          regular_path, sizeof(regular_path),
                                          italic_path, sizeof(italic_path));
    if (st != MDF_OK) {
        return luaL_error(L, "html font dump paths: %s", mdf_status_string(st));
    }
    lua_pushstring(L, regular_path);
    lua_pushstring(L, italic_path);
    return 2;
}

static int lua_mdf_paths_alias(lua_State *L)
{
    const char *first_path;
    const char *second_path;

    first_path = luaL_checkstring(L, 1);
    second_path = luaL_checkstring(L, 2);
    lua_pushboolean(L, mdf_paths_alias(first_path, second_path));
    return 1;
}

/* Match a candidate path to the real stdout destination, including descriptor
 * aliases such as /dev/stdout and /proc/self/fd/1. */
static int lua_mdf_path_aliases_stdout(lua_State *L)
{
    const char *path;
    struct stat path_st;
    struct stat stdout_st;
    int aliases;

    path = luaL_checkstring(L, 1);
    aliases = stat(path, &path_st) == 0 && fstat(STDOUT_FILENO, &stdout_st) == 0 &&
        path_st.st_dev == stdout_st.st_dev && path_st.st_ino == stdout_st.st_ino;
    lua_pushboolean(L, aliases);
    return 1;
}

/* Keep cmdf.lua's implicit local font references on the same lexical path
 * model as native cmdf, without using a shell or resolving symlinks. This
 * module is built against libmdf's installed public headers, so the small
 * helper stays self-contained instead of expanding the public C API. */
static int lua_mdf_normalize_absolute_path(char *dst, size_t cap, const char *path)
{
    char source[4096];
    char cwd[4096];
    const char *p;
    size_t len;
    size_t out_len;

    if (dst == NULL || cap < 2 || path == NULL || path[0] == '\0') {
        return -1;
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(source)) {
            return -1;
        }
        strcpy(source, path);
    } else {
        if (getcwd(cwd, sizeof(cwd)) == NULL ||
            strlen(cwd) + 1 + strlen(path) + 1 > sizeof(source)) {
            return -1;
        }
        strcpy(source, cwd);
        strcat(source, "/");
        strcat(source, path);
    }
    dst[0] = '/';
    out_len = 1;
    p = source;
    while (*p != '\0') {
        const char *component;

        while (*p == '/') p++;
        component = p;
        while (*p != '\0' && *p != '/') p++;
        len = (size_t)(p - component);
        if (len == 0 || (len == 1 && component[0] == '.')) {
            continue;
        }
        if (len == 2 && component[0] == '.' && component[1] == '.') {
            while (out_len > 1 && dst[out_len - 1] != '/') out_len--;
            if (out_len > 1) out_len--;
            continue;
        }
        if (out_len + (out_len > 1 ? 1 : 0) + len + 1 > cap) {
            return -1;
        }
        if (out_len > 1) dst[out_len++] = '/';
        memcpy(dst + out_len, component, len);
        out_len += len;
    }
    dst[out_len] = '\0';
    return 0;
}

static int lua_mdf_path_relative_to(lua_State *L)
{
    const char *from_dir;
    const char *target_path;
    char from[4096];
    char target[4096];
    char relative_path[4096];
    const char *from_part;
    const char *target_part;
    size_t out_len;

    from_dir = luaL_checkstring(L, 1);
    target_path = luaL_checkstring(L, 2);
    if (lua_mdf_normalize_absolute_path(from, sizeof(from), from_dir) != 0 ||
        lua_mdf_normalize_absolute_path(target, sizeof(target), target_path) != 0) {
        return luaL_error(L, "relative path: invalid path");
    }
    from_part = from + 1;
    target_part = target + 1;
    while (*from_part != '\0' && *target_part != '\0') {
        const char *from_end;
        const char *target_end;
        size_t from_len;
        size_t target_len;

        from_end = strchr(from_part, '/');
        target_end = strchr(target_part, '/');
        if (from_end == NULL) from_end = from_part + strlen(from_part);
        if (target_end == NULL) target_end = target_part + strlen(target_part);
        from_len = (size_t)(from_end - from_part);
        target_len = (size_t)(target_end - target_part);
        if (from_len != target_len || strncmp(from_part, target_part, from_len) != 0) {
            break;
        }
        from_part = *from_end == '\0' ? from_end : from_end + 1;
        target_part = *target_end == '\0' ? target_end : target_end + 1;
    }
    out_len = 0;
    while (*from_part != '\0') {
        const char *from_end;

        from_end = strchr(from_part, '/');
        if (from_end == NULL) from_end = from_part + strlen(from_part);
        if (out_len + 3 + 1 > sizeof(relative_path)) {
            return luaL_error(L, "relative path: invalid path");
        }
        memcpy(relative_path + out_len, "../", 3);
        out_len += 3;
        from_part = *from_end == '\0' ? from_end : from_end + 1;
    }
    if (*target_part != '\0') {
        size_t target_len;

        target_len = strlen(target_part);
        if (out_len + target_len + 1 > sizeof(relative_path)) {
            return luaL_error(L, "relative path: invalid path");
        }
        memcpy(relative_path + out_len, target_part, target_len);
        out_len += target_len;
    }
    if (out_len == 0) relative_path[out_len++] = '.';
    relative_path[out_len] = '\0';
    {
        char uri_path[4096];
        size_t i;
        size_t uri_len;

        uri_len = 0;
        for (i = 0; relative_path[i] != '\0'; i++) {
            unsigned char c;

            c = (unsigned char)relative_path[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                c == '~' || c == '/') {
                if (uri_len + 2 > sizeof(uri_path)) return luaL_error(L, "relative path: invalid path");
                uri_path[uri_len++] = (char)c;
            } else {
                static const char hex[] = "0123456789ABCDEF";

                if (uri_len + 4 > sizeof(uri_path)) return luaL_error(L, "relative path: invalid path");
                uri_path[uri_len++] = '%';
                uri_path[uri_len++] = hex[c >> 4];
                uri_path[uri_len++] = hex[c & 0x0f];
            }
        }
        uri_path[uri_len] = '\0';
        lua_pushstring(L, uri_path);
    }
    return 1;
}

static size_t lua_mdf_source_read(void *userdata, char *dst, size_t cap, int *err)
{
    lua_mdf_source_ctx *ctx;
    size_t len;
    const char *chunk;

    ctx = (lua_mdf_source_ctx *)userdata;
    *err = 0;
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref);
    lua_pushinteger(ctx->L, (lua_Integer)cap);
    if (lua_pcall(ctx->L, 1, 1, 0) != LUA_OK) {
        *err = 1;
        lua_pop(ctx->L, 1);
        return 0;
    }
    if (lua_isnil(ctx->L, -1)) {
        lua_pop(ctx->L, 1);
        return 0;
    }
    if (lua_type(ctx->L, -1) != LUA_TSTRING) {
        *err = 1;
        lua_pop(ctx->L, 1);
        return 0;
    }
    chunk = lua_tolstring(ctx->L, -1, &len);
    if (len > cap) {
        *err = 1;
        lua_pop(ctx->L, 1);
        return 0;
    }
    memcpy(dst, chunk, len);
    lua_pop(ctx->L, 1);
    return len;
}

static lua_mdf_handle *lua_mdf_check_handle(lua_State *L, int index)
{
    lua_mdf_handle *handle;

    handle = (lua_mdf_handle *)luaL_checkudata(L, index, LUA_MDF_HANDLE);
    if (handle->mdf == NULL) {
        luaL_error(L, "libmdf handle is closed");
    }
    return handle;
}

static int lua_mdf_token_type(lua_State *L, int index)
{
    const char *s;

    if (lua_isinteger(L, index)) {
        return (int)lua_tointeger(L, index);
    }
    s = luaL_checkstring(L, index);
    if (strcmp(s, "text") == 0) return MDF_TOKEN_TEXT;
    if (strcmp(s, "space") == 0) return MDF_TOKEN_SPACE;
    if (strcmp(s, "newline") == 0) return MDF_TOKEN_NEWLINE;
    if (strcmp(s, "paragraph_end") == 0) return MDF_TOKEN_PARAGRAPH_END;
    if (strcmp(s, "heading_start") == 0) return MDF_TOKEN_HEADING_START;
    if (strcmp(s, "heading_end") == 0) return MDF_TOKEN_HEADING_END;
    if (strcmp(s, "blockquote_start") == 0) return MDF_TOKEN_BLOCKQUOTE_START;
    if (strcmp(s, "blockquote_end") == 0) return MDF_TOKEN_BLOCKQUOTE_END;
    if (strcmp(s, "list_item_start") == 0) return MDF_TOKEN_LIST_ITEM_START;
    if (strcmp(s, "list_item_end") == 0) return MDF_TOKEN_LIST_ITEM_END;
    if (strcmp(s, "task_unchecked") == 0) return MDF_TOKEN_TASK_UNCHECKED;
    if (strcmp(s, "task_checked") == 0) return MDF_TOKEN_TASK_CHECKED;
    if (strcmp(s, "code_block_start") == 0) return MDF_TOKEN_CODE_BLOCK_START;
    if (strcmp(s, "code_block_end") == 0) return MDF_TOKEN_CODE_BLOCK_END;
    if (strcmp(s, "code_text") == 0) return MDF_TOKEN_CODE_TEXT;
    if (strcmp(s, "thematic_break") == 0) return MDF_TOKEN_THEMATIC_BREAK;
    if (strcmp(s, "document_end") == 0) return MDF_TOKEN_DOCUMENT_END;
    if (strcmp(s, "chart_block") == 0) return MDF_TOKEN_CHART_BLOCK;
    return luaL_error(L, "unknown token type: %s", s);
}

static void lua_mdf_read_token(lua_State *L, int index, mdf_token *tok)
{
    size_t len;

    luaL_checktype(L, index, LUA_TTABLE);
    memset(tok, 0, sizeof(*tok));
    lua_getfield(L, index, "type");
    tok->type = (mdf_token_type)lua_mdf_token_type(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "text");
    if (!lua_isnil(L, -1)) {
        tok->text = luaL_checklstring(L, -1, &len);
        tok->len = len;
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "level");
    if (!lua_isnil(L, -1)) {
        tok->level = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
}

static int lua_mdf_sink_write(void *userdata, const char *src, size_t len)
{
    lua_mdf_sink_ctx *ctx;
    int ok;

    ctx = (lua_mdf_sink_ctx *)userdata;
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->ref);
    lua_pushlstring(ctx->L, src, len);
    if (lua_pcall(ctx->L, 1, 1, 0) != LUA_OK) {
        return -1;
    }
    ok = lua_isnil(ctx->L, -1) || lua_toboolean(ctx->L, -1);
    lua_pop(ctx->L, 1);
    return ok ? 0 : -1;
}

static int lua_mdf_render(lua_State *L)
{
    const char *markdown;
    mdf_options opts;
    lua_mdf_trace_ctx trace_ctx;
    mdf_format format;
    mdf *inst;
    const char *html_title;
    char *out;
    mdf_status st;

    markdown = luaL_checkstring(L, 1);
    mdf_options_init(&opts);
    trace_ctx.L = L;
    trace_ctx.ref = LUA_NOREF;
    format = MDF_FORMAT_ANSI;
    html_title = NULL;
    if (lua_istable(L, 2)) {
        format = lua_mdf_format_from_options(L, 2);
        html_title = lua_mdf_get_optional_html_title(L, 2);
        lua_mdf_apply_options(L, 2, &opts, &trace_ctx);
    }
    inst = NULL;
    st = lua_istable(L, 2) ? lua_mdf_apply_html_font_options(L, 2, format, &opts) : MDF_OK;
    if (st == MDF_OK) {
        st = mdf_create(format, &opts, &inst);
    }
    if (st != MDF_OK) {
        if (inst != NULL) {
            inst->destroy(inst);
        }
        if (trace_ctx.ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
        }
        return luaL_error(L, "mdf_create: %s", mdf_status_string(st));
    }
    if ((format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) && html_title != NULL) {
        st = mdf_set_html_title(inst, html_title);
        if (st != MDF_OK) {
            inst->destroy(inst);
            if (trace_ctx.ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
            }
            return luaL_error(L, "mdf_set_html_title: %s", mdf_status_string(st));
        }
    }
    out = NULL;
    st = inst->render_cstr(inst, markdown, &out);
    if (st != MDF_OK) {
        char errbuf[256];
        const char *err;

        err = inst->error(inst);
        snprintf(errbuf, sizeof(errbuf), "%s", err == NULL ? "" : err);
        inst->destroy(inst);
        if (trace_ctx.ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
        }
        return luaL_error(L, "mdf_render: %s: %s", mdf_status_string(st), errbuf);
    }
    lua_pushlstring(L, out == NULL ? "" : out, out == NULL ? 0 : strlen(out));
    inst->string_free(inst, out);
    inst->destroy(inst);
    if (trace_ctx.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
    }
    return 1;
}

static int lua_mdf_render_stream(lua_State *L)
{
    mdf_options opts;
    lua_mdf_trace_ctx trace_ctx;
    mdf_format format;
    mdf *inst;
    mdf_source source;
    mdf_sink sink;
    lua_mdf_source_ctx source_ctx;
    lua_mdf_sink_ctx sink_ctx;
    const char *html_title;
    mdf_status st;

    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    mdf_options_init(&opts);
    trace_ctx.L = L;
    trace_ctx.ref = LUA_NOREF;
    format = MDF_FORMAT_ANSI;
    html_title = NULL;
    if (lua_istable(L, 3)) {
        format = lua_mdf_format_from_options(L, 3);
        html_title = lua_mdf_get_optional_html_title(L, 3);
        lua_mdf_apply_options(L, 3, &opts, &trace_ctx);
    }
    lua_pushvalue(L, 1);
    source_ctx.L = L;
    source_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 2);
    sink_ctx.L = L;
    sink_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source.userdata = &source_ctx;
    source.read = lua_mdf_source_read;
    sink.userdata = &sink_ctx;
    sink.write = lua_mdf_sink_write;
    inst = NULL;
    st = lua_istable(L, 3) ? lua_mdf_apply_html_font_options(L, 3, format, &opts) : MDF_OK;
    if (st == MDF_OK) {
        st = mdf_create(format, &opts, &inst);
    }
    if (st == MDF_OK && (format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) && html_title != NULL) {
        st = mdf_set_html_title(inst, html_title);
    }
    if (st == MDF_OK) {
        st = inst->render(inst, &source, &sink);
    }
    if (inst != NULL) {
        inst->destroy(inst);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.ref);
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.ref);
    if (trace_ctx.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
    }
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_render_stream: %s", mdf_status_string(st));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mdf_new(lua_State *L)
{
    mdf_options opts;
    mdf_format format;
    lua_mdf_handle *handle;
    const char *html_title;
    mdf_status st;

    mdf_options_init(&opts);
    format = lua_mdf_format_from_options(L, 1);
    html_title = lua_mdf_get_optional_html_title(L, 1);
    handle = (lua_mdf_handle *)lua_newuserdatauv(L, sizeof(*handle), 0);
    handle->mdf = NULL;
    handle->opts_ref = LUA_NOREF;
    handle->trace_ctx.L = L;
    handle->trace_ctx.ref = LUA_NOREF;
    if (lua_istable(L, 1)) {
        lua_mdf_apply_options(L, 1, &opts, &handle->trace_ctx);
    }
    if (lua_istable(L, 1)) {
        lua_pushvalue(L, 1);
        handle->opts_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    st = lua_istable(L, 1) ? lua_mdf_apply_html_font_options(L, 1, format, &opts) : MDF_OK;
    if (st == MDF_OK) {
        st = mdf_create(format, &opts, &handle->mdf);
    }
    if (st == MDF_OK && (format == MDF_FORMAT_HTML || format == MDF_FORMAT_HTML_DECK) && html_title != NULL) {
        st = mdf_set_html_title(handle->mdf, html_title);
    }
    if (st != MDF_OK) {
        if (handle->mdf != NULL) {
            handle->mdf->destroy(handle->mdf);
            handle->mdf = NULL;
        }
        if (handle->opts_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, handle->opts_ref);
        }
        if (handle->trace_ctx.ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, handle->trace_ctx.ref);
        }
        return luaL_error(L, "mdf_create: %s", mdf_status_string(st));
    }
    luaL_getmetatable(L, LUA_MDF_HANDLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_mdf_handle_render(lua_State *L)
{
    lua_mdf_handle *handle;
    const char *markdown;
    char *out;
    mdf_status st;

    handle = lua_mdf_check_handle(L, 1);
    markdown = luaL_checkstring(L, 2);
    out = NULL;
    st = handle->mdf->render_cstr(handle->mdf, markdown, &out);
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_render: %s: %s",
                          mdf_status_string(st),
                          handle->mdf->error(handle->mdf));
    }
    lua_pushlstring(L, out == NULL ? "" : out, out == NULL ? 0 : strlen(out));
    handle->mdf->string_free(handle->mdf, out);
    return 1;
}

static int lua_mdf_handle_render_stream(lua_State *L)
{
    lua_mdf_handle *handle;
    mdf_source source;
    mdf_sink sink;
    lua_mdf_source_ctx source_ctx;
    lua_mdf_sink_ctx sink_ctx;
    mdf_status st;

    handle = lua_mdf_check_handle(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    source_ctx.L = L;
    source_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 3);
    sink_ctx.L = L;
    sink_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source.userdata = &source_ctx;
    source.read = lua_mdf_source_read;
    sink.userdata = &sink_ctx;
    sink.write = lua_mdf_sink_write;
    st = handle->mdf->render(handle->mdf, &source, &sink);
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.ref);
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.ref);
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_render_stream: %s: %s",
                          mdf_status_string(st),
                          handle->mdf->error(handle->mdf));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mdf_handle_set_html_title(lua_State *L)
{
    lua_mdf_handle *handle;
    const char *title;
    mdf_status st;

    handle = lua_mdf_check_handle(L, 1);
    title = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    st = mdf_set_html_title(handle->mdf, title);
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_set_html_title: %s: %s",
                          mdf_status_string(st),
                          handle->mdf->error(handle->mdf));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mdf_handle_write_token(lua_State *L)
{
    lua_mdf_handle *handle;
    lua_mdf_sink_ctx sink_ctx;
    mdf_sink sink;
    mdf_token tok;
    mdf_status st;

    handle = lua_mdf_check_handle(L, 1);
    lua_mdf_read_token(L, 2, &tok);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    sink_ctx.L = L;
    sink_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sink.userdata = &sink_ctx;
    sink.write = lua_mdf_sink_write;
    st = handle->mdf->write_token(handle->mdf, &tok, &sink);
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.ref);
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_write_token: %s: %s",
                          mdf_status_string(st),
                          handle->mdf->error(handle->mdf));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mdf_handle_finish(lua_State *L)
{
    lua_mdf_handle *handle;
    lua_mdf_sink_ctx sink_ctx;
    mdf_sink sink;
    mdf_status st;

    handle = lua_mdf_check_handle(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    sink_ctx.L = L;
    sink_ctx.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sink.userdata = &sink_ctx;
    sink.write = lua_mdf_sink_write;
    st = handle->mdf->finish(handle->mdf, &sink);
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.ref);
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_finish: %s: %s",
                          mdf_status_string(st),
                          handle->mdf->error(handle->mdf));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_mdf_handle_error(lua_State *L)
{
    lua_mdf_handle *handle;

    handle = lua_mdf_check_handle(L, 1);
    lua_pushstring(L, handle->mdf->error(handle->mdf));
    return 1;
}

static int lua_mdf_handle_close(lua_State *L)
{
    lua_mdf_handle *handle;

    handle = (lua_mdf_handle *)luaL_checkudata(L, 1, LUA_MDF_HANDLE);
    if (handle->mdf != NULL) {
        handle->mdf->destroy(handle->mdf);
        handle->mdf = NULL;
    }
    if (handle->opts_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, handle->opts_ref);
        handle->opts_ref = LUA_NOREF;
    }
    if (handle->trace_ctx.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, handle->trace_ctx.ref);
        handle->trace_ctx.ref = LUA_NOREF;
    }
    return 0;
}

static int lua_mdf_theme_names(lua_State *L)
{
    size_t i;
    size_t count;

    count = mdf_theme_count();
    lua_createtable(L, (int)count, 0);
    for (i = 0; i < count; i++) {
        lua_pushstring(L, mdf_theme_name(i));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int lua_mdf_detect_osc8_support(lua_State *L)
{
    lua_pushboolean(L, mdf_detect_osc8_support());
    return 1;
}

static int lua_mdf_theme_exists(lua_State *L)
{
    lua_pushboolean(L, mdf_theme_exists(luaL_optstring(L, 1, NULL)));
    return 1;
}

static int lua_mdf_status_string(lua_State *L)
{
    lua_pushstring(L, mdf_status_string((mdf_status)luaL_checkinteger(L, 1)));
    return 1;
}

static int lua_mdf_terminal_width(lua_State *L)
{
    int fd;
    int fallback;

    fd = (int)luaL_optinteger(L, 1, -1);
    fallback = (int)luaL_optinteger(L, 2, 80);
    lua_pushinteger(L, mdf_terminal_width(fd, fallback));
    return 1;
}

static int lua_mdf_pager(lua_State *L)
{
    const char *path;
    mdf_options opts;
    lua_mdf_trace_ctx trace_ctx;
    mdf_pager_format format;
    mdf_status st;

    path = luaL_checkstring(L, 1);
    mdf_options_init(&opts);
    trace_ctx.L = L;
    trace_ctx.ref = LUA_NOREF;
    format = MDF_PAGER_FORMAT_AUTO;
    if (!lua_isnoneornil(L, 2)) {
        const char *value;

        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "format");
        if (!lua_isnil(L, -1)) {
            value = luaL_checkstring(L, -1);
            if (strcmp(value, "markdown") == 0) {
                format = MDF_PAGER_FORMAT_MARKDOWN;
            } else if (strcmp(value, "text") == 0) {
                format = MDF_PAGER_FORMAT_TEXT;
            } else {
                lua_pop(L, 1);
                return luaL_error(L, "pager format must be markdown or text");
            }
        }
        lua_pop(L, 1);
        lua_mdf_apply_options(L, 2, &opts, &trace_ctx);
    }
    st = mdf_pager_file(path, &opts, format);
    if (trace_ctx.ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, trace_ctx.ref);
    }
    if (st != MDF_OK) {
        return luaL_error(L, "mdf_pager: %s", mdf_status_string(st));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg lua_mdf_funcs[] = {
    {"new", lua_mdf_new},
    {"create", lua_mdf_new},
    {"render", lua_mdf_render},
    {"render_stream", lua_mdf_render_stream},
    {"html_font_dump_paths", lua_mdf_html_font_dump_paths},
    {"paths_alias", lua_mdf_paths_alias},
    {"path_aliases_stdout", lua_mdf_path_aliases_stdout},
    {"path_relative_to", lua_mdf_path_relative_to},
    {"theme_names", lua_mdf_theme_names},
    {"theme_exists", lua_mdf_theme_exists},
    {"status_string", lua_mdf_status_string},
    {"detect_osc8_support", lua_mdf_detect_osc8_support},
    {"terminal_width", lua_mdf_terminal_width},
    {"pager", lua_mdf_pager},
    {NULL, NULL}
};

static const luaL_Reg lua_mdf_methods[] = {
    {"render", lua_mdf_handle_render},
    {"render_stream", lua_mdf_handle_render_stream},
    {"set_html_title", lua_mdf_handle_set_html_title},
    {"write_token", lua_mdf_handle_write_token},
    {"finish", lua_mdf_handle_finish},
    {"error", lua_mdf_handle_error},
    {"close", lua_mdf_handle_close},
    {"destroy", lua_mdf_handle_close},
    {"__gc", lua_mdf_handle_close},
    {NULL, NULL}
};

static void lua_mdf_set_token_constants(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, MDF_TOKEN_TEXT); lua_setfield(L, -2, "TEXT");
    lua_pushinteger(L, MDF_TOKEN_SPACE); lua_setfield(L, -2, "SPACE");
    lua_pushinteger(L, MDF_TOKEN_NEWLINE); lua_setfield(L, -2, "NEWLINE");
    lua_pushinteger(L, MDF_TOKEN_PARAGRAPH_END); lua_setfield(L, -2, "PARAGRAPH_END");
    lua_pushinteger(L, MDF_TOKEN_HEADING_START); lua_setfield(L, -2, "HEADING_START");
    lua_pushinteger(L, MDF_TOKEN_HEADING_END); lua_setfield(L, -2, "HEADING_END");
    lua_pushinteger(L, MDF_TOKEN_BLOCKQUOTE_START); lua_setfield(L, -2, "BLOCKQUOTE_START");
    lua_pushinteger(L, MDF_TOKEN_BLOCKQUOTE_END); lua_setfield(L, -2, "BLOCKQUOTE_END");
    lua_pushinteger(L, MDF_TOKEN_LIST_ITEM_START); lua_setfield(L, -2, "LIST_ITEM_START");
    lua_pushinteger(L, MDF_TOKEN_LIST_ITEM_END); lua_setfield(L, -2, "LIST_ITEM_END");
    lua_pushinteger(L, MDF_TOKEN_TASK_UNCHECKED); lua_setfield(L, -2, "TASK_UNCHECKED");
    lua_pushinteger(L, MDF_TOKEN_TASK_CHECKED); lua_setfield(L, -2, "TASK_CHECKED");
    lua_pushinteger(L, MDF_TOKEN_CODE_BLOCK_START); lua_setfield(L, -2, "CODE_BLOCK_START");
    lua_pushinteger(L, MDF_TOKEN_CODE_BLOCK_END); lua_setfield(L, -2, "CODE_BLOCK_END");
    lua_pushinteger(L, MDF_TOKEN_CODE_TEXT); lua_setfield(L, -2, "CODE_TEXT");
    lua_pushinteger(L, MDF_TOKEN_THEMATIC_BREAK); lua_setfield(L, -2, "THEMATIC_BREAK");
    lua_pushinteger(L, MDF_TOKEN_DOCUMENT_END); lua_setfield(L, -2, "DOCUMENT_END");
    lua_pushinteger(L, MDF_TOKEN_CHART_BLOCK); lua_setfield(L, -2, "CHART_BLOCK");
    lua_setfield(L, -2, "token");
}

static void lua_mdf_set_status_constants(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, MDF_OK); lua_setfield(L, -2, "OK");
    lua_pushinteger(L, MDF_ERROR_INVALID); lua_setfield(L, -2, "INVALID");
    lua_pushinteger(L, MDF_ERROR_NOMEM); lua_setfield(L, -2, "NOMEM");
    lua_pushinteger(L, MDF_ERROR_IO); lua_setfield(L, -2, "IO");
    lua_pushinteger(L, MDF_ERROR_PARSE); lua_setfield(L, -2, "PARSE");
    lua_setfield(L, -2, "status");
}

static void lua_mdf_set_version_constants(lua_State *L)
{
    lua_pushstring(L, LIBMDF_VERSION);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, LIBMDF_VERSION_MAJOR);
    lua_setfield(L, -2, "version_major");
    lua_pushinteger(L, LIBMDF_VERSION_MINOR);
    lua_setfield(L, -2, "version_minor");
    lua_pushinteger(L, LIBMDF_VERSION_PATCH);
    lua_setfield(L, -2, "version_patch");
}

int luaopen_libmdf_core(lua_State *L)
{
    luaL_newmetatable(L, LUA_MDF_HANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, lua_mdf_methods, 0);
    lua_pop(L, 1);

    luaL_newlib(L, lua_mdf_funcs);
    lua_mdf_set_token_constants(L);
    lua_mdf_set_status_constants(L);
    lua_mdf_set_version_constants(L);
    return 1;
}
