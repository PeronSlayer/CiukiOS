/* SPDX-License-Identifier: MIT */
#include "terminal.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static struct terminal *owner(lua_State *L) { return *(struct terminal **)lua_getextraspace(L); }
static void hook(lua_State *L,lua_Debug *debug)
{
    (void)debug; struct terminal *t=owner(L);
    if(t->host.pump)t->host.pump(t->host.ctx);
    if(t->interrupted || t->quit)luaL_error(L,"interrupted");
}
static int print(lua_State *L)
{
    struct terminal *t=owner(L); int n=lua_gettop(L);
    for(int i=1;i<=n;i++) { size_t size; const char *s=luaL_tolstring(L,i,&size); (void)size;
        if(i>1)term_write(t,"\t"); term_write(t,s); lua_pop(L,1); }
    term_write(t,"\n"); return 0;
}
static int run(lua_State *L)
{
    struct terminal *t=owner(L); const char *s=luaL_checkstring(L,1); int error;
    char *out=term_capture(t,s,&error); if(!out)return luaL_error(L,"command capture failed (%d)",-error);
    if(error==-EOVERFLOW) { free(out); return luaL_error(L,"command output exceeds 1 MiB"); }
    lua_pushstring(L,out); lua_pushinteger(L,error); free(out); return 2;
}
static int create(struct terminal *t)
{
    if(t->lua)return 0;
    lua_State *L=luaL_newstate(); if(!L)return -ENOMEM;
    t->lua=L; *(struct terminal **)lua_getextraspace(L)=t; luaL_openlibs(L);
    lua_pushcfunction(L,print); lua_setglobal(L,"print");
    lua_newtable(L); lua_pushcfunction(L,print); lua_setfield(L,-2,"print"); lua_pushcfunction(L,run); lua_setfield(L,-2,"run"); lua_setglobal(L,"term");
    lua_sethook(L,hook,LUA_MASKCOUNT,1000); return 0;
}
static int execute(struct terminal *t,int status)
{
    lua_State *L=t->lua; int previous=t->busy; t->busy=2;
    if(status==LUA_OK)status=lua_pcall(L,0,LUA_MULTRET,0);
    if(status!=LUA_OK)term_printf(t,"Lua: %s\n",lua_tostring(L,-1));
    else if(lua_gettop(L)) { lua_pushcfunction(L,print); lua_insert(L,1); status=lua_pcall(L,lua_gettop(L)-1,0,0);
        if(status!=LUA_OK)term_printf(t,"Lua: %s\n",lua_tostring(L,-1)); }
    lua_settop(L,0); t->busy=previous; return status==LUA_OK ? 0 : t->interrupted ? -EINTR : -EINVAL;
}
int term_lua(struct terminal *t,const char *name,int argc,char **argv)
{
    int r=create(t); if(r)return r;
    char path[FM_PATH]; if((r=term_path(t,name,path)))return r;
    struct fm_meta meta; if((r=t->host.fs.stat_path(t->host.fs.ctx,path,&meta)))return r;
    if(meta.type!=FM_FILE)return -EISDIR;
    if(meta.size>TERM_CAPTURE)return -EFBIG;
    int fd=t->host.fs.open(t->host.fs.ctx,path,0); if(fd<0)return fd;
    char *source=malloc((size_t)meta.size+1); if(!source) { t->host.fs.close(t->host.fs.ctx,fd); return -ENOMEM; }
    unsigned used=0;
    while(used<meta.size) { r=t->host.fs.read(t->host.fs.ctx,fd,source+used,(unsigned)meta.size-used); if(r<=0)break; used+=(unsigned)r;
        if(t->host.pump)t->host.pump(t->host.ctx); if(t->interrupted) { r=-EINTR; break; } }
    int close=t->host.fs.close(t->host.fs.ctx,fd); if(r<0 || used<meta.size || close) { free(source); return r<0 ? r : close ? close : -EIO; }
    source[used]=0; lua_State *L=t->lua; lua_settop(L,0); lua_newtable(L);
    lua_pushstring(L,path); lua_rawseti(L,-2,0); for(int i=0;i<argc;i++) { lua_pushstring(L,argv[i]); lua_rawseti(L,-2,i+1); } lua_setglobal(L,"arg");
    /* luaL_loadfile's shebang handling, with the same VFS as the commands. */
    if(used && source[0]=='#') { char *end=memchr(source,'\n',used); size_t n=end ? (size_t)(end-source) : used; memset(source,' ',n); }
    r=luaL_loadbuffer(L,source,used,path); free(source); return execute(t,r);
}
int term_lua_line(struct terminal *t,const char *line)
{
    if(!strcmp(line,".exit")) { t->lua_mode=0; free(t->chunk); t->chunk=NULL; t->chunk_length=0; return 0; }
    int r=create(t); if(r)return r;
    size_t n=strlen(line); if(t->chunk_length+n+2>TERM_CAPTURE)return -E2BIG;
    char *p=realloc(t->chunk,t->chunk_length+n+2); if(!p)return -ENOMEM; t->chunk=p;
    memcpy(p+t->chunk_length,line,n); t->chunk_length+=n; p[t->chunk_length++]='\n'; p[t->chunk_length]=0;
    lua_State *L=t->lua; lua_settop(L,0);
    /* Expressions, like the upstream interactive interpreter, print results. */
    if(!strchr(p,'\n') || t->chunk_length==n+1) { char *expression=malloc(n+8);
        if(expression) { memcpy(expression,"return ",7); memcpy(expression+7,line,n+1); r=luaL_loadbuffer(L,expression,n+7,"=terminal"); free(expression); }
        else r=LUA_ERRMEM;
    } else r=LUA_ERRSYNTAX;
    if(r!=LUA_OK) { lua_settop(L,0); r=luaL_loadbuffer(L,p,t->chunk_length,"=terminal"); }
    if(r==LUA_ERRSYNTAX) { const char *error=lua_tostring(L,-1); size_t size=strlen(error);
        if(size>=5 && !strcmp(error+size-5,"<eof>")) { lua_settop(L,0); return 0; } }
    free(t->chunk); t->chunk=NULL; t->chunk_length=0; return execute(t,r);
}
void term_lua_close(struct terminal *t) { if(t->lua)lua_close(t->lua); t->lua=NULL; }
