/* Focused host reference: unchanged Lua 5.4.8 and files.lua, pinned newlib,
 * production SDK wrappers, modeled CiukiOS syscall boundary. Never gate evidence.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_files_data.h"

static int run(lua_State *L,const unsigned char *text,size_t size,const char *name)
{
    if(luaL_loadbuffer(L,(const char *)text,size,name)||lua_pcall(L,0,0,0)) {
        fprintf(stderr,"%s\n",lua_tostring(L,-1));return 1;
    }
    return 0;
}
int main(void)
{
    lua_State *L=luaL_newstate();if(!L)return 2;
    luaL_openlibs(L);
    int failed=run(L,setup_lua,sizeof(setup_lua)-1,"@all.lua-switches") ||
               run(L,files_lua,sizeof(files_lua)-1,"@files.lua");
    /* Also cover contract behavior outside files.lua's active assertions. */
    static const unsigned char extra[]=
        "local f=assert(io.tmpfile()); f:write('live'); "
        "local n=os.tmpname(); local g=assert(io.open(n,'w+')); g:write('other'); g:close(); "
        "f:seek('set'); assert(f:read('a')=='live'); "
        "assert(f:seek('set',10000)==10000); assert(f:read(1)==nil); "
        "f:write('x'); f:seek('set',9999); assert(f:read('a')=='\\0x'); f:close(); "
        "assert(os.remove(n)); "
        "for _,d in ipairs{{year=2000,month=2,day=29,hour=0},{year=2040,month=1,day=1,hour=0}} do "
        "local t=os.time(d); assert(os.time(os.date('!*t',t))==t); "
        "assert(os.time(os.date('*t',t))==t) end; assert(os.clock()>=0); "
        "assert(io.stdin:read(1)==nil); local p,m,e=io.stdin:seek('set',1000); "
        "assert(p==nil and type(m)=='string' and type(e)=='number'); "
        "io.stdout:write('host stdout\\n'); io.stderr:write('host stderr\\n'); assert(io.flush()); "
        "print('host-reference files.lua PASS')";
    if(!failed)failed=run(L,extra,sizeof(extra)-1,"@file-contract-extras");
    lua_close(L);return failed;
}
