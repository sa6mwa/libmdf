#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fault_allocator {
    int failing;
    size_t live;
} fault_allocator;

static void *allocate(void *userdata, void *ptr, size_t old_size, size_t size)
{
    fault_allocator *fault = (fault_allocator *)userdata;
    void *next;

    if (size == 0) {
        if (ptr != NULL) fault->live--;
        free(ptr);
        return NULL;
    }
    if (fault->failing && (ptr == NULL || size > old_size)) return NULL;
    next = realloc(ptr, size);
    if (next != NULL && ptr == NULL) fault->live++;
    return next;
}

static int arm(lua_State *L)
{
    void *userdata;

    lua_getallocf(L, &userdata);
    ((fault_allocator *)userdata)->failing = 1;
    return 0;
}

static void run(const char *name, const char *body, const char *recovery)
{
    fault_allocator fault = {0, 0};
    lua_State *L = lua_newstate(allocate, &fault, 0);
    int status;

    assert(L != NULL);
    luaL_openlibs(L);
    lua_pushcfunction(L, arm);
    lua_setglobal(L, "arm");
    assert(luaL_loadstring(L, body) == LUA_OK);
    status = lua_pcall(L, 0, 0, 0);
    fault.failing = 0;
    if (status != LUA_ERRMEM) {
        fprintf(stderr, "%s: expected LUA_ERRMEM, got %d: %s\n", name, status, lua_tostring(L, -1));
        abort();
    }
    lua_settop(L, 0);
    if (recovery != NULL && luaL_dostring(L, recovery) != LUA_OK) {
        fprintf(stderr, "%s recovery: %s\n", name, lua_tostring(L, -1));
        abort();
    }
    lua_close(L);
    assert(fault.live == 0);
    printf("%s: allocation failure and cleanup passed\n", name);
}

#define START "local m=require('libmdf'); local data=string.rep('a',1000)..'\\n'; "
#define READER "function() if once then return nil end; once=true; arm(); return data end"

int main(int argc, char **argv)
{
    run("sink-copy", START "local once=false; m.render_stream(" READER
        ",function() return true end,{width=1200})", NULL);
    if (argc == 2 && strcmp(argv[1], "sink-copy") == 0) return 0;
    run("source-callback", START "m.render_stream(function() arm();return string.rep('x',2000) end,"
        "function() return true end)", NULL);
    run("sink-callback", START "local once=false; m.render_stream(function() if once then return nil end;"
        "once=true;return data end,function() arm();return string.rep('x',2000) end,{width=1200})", NULL);
    run("trace-copy", START "local once=false;m.render_stream(function() if once then return nil end;"
        "once=true;return data end,function() arm();return true end,"
        "{width=1200,write_trace=function() return true end})", NULL);
    run("trace-callback", START "m.render(data,{width=1200,write_trace=function() arm();"
        "return string.rep('x',2000) end})", NULL);
    run("result-copy", START "local n=0;m.render(data,{width=1200,"
        "write_trace=function() n=n+1 end});local count=0;m.render(data,{width=1200,"
        "write_trace=function() count=count+1;if count==n then arm() end end})", NULL);
    run("handle-result", START "handle=m.new({width=1200});arm();handle:render(data)",
        "assert(type(handle:render('healthy'))=='string');handle:close();handle:close()");
    run("handle-sink", START "handle=m.new({width=1200});handle:set_sink(function() return true end);"
        "local once=false;handle:render_stream(" READER ")", "handle:reset();handle:close()");
    run("document-sink", START "handle=m.document_stream({width=1200},function() return true end);"
        "arm();handle:write(data);handle:finish_document()", "handle:reset();handle:close()");
    run("handle-trace", START "handle=m.new({width=1200,write_trace=function() arm();"
        "return string.rep('x',2000) end});handle:render(data)", "handle:close()");
    if (argc == 2) {
        fault_allocator fault = {0, 0};
        lua_State *L = lua_newstate(allocate, &fault, 0);
        assert(L != NULL);
        luaL_openlibs(L);
        if (luaL_dofile(L, argv[1]) != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            abort();
        }
        lua_close(L);
        assert(fault.live == 0);
    }
    return 0;
}
