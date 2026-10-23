/* SPDX-License-Identifier: MIT -- same catalogue keys as Setup configuration. */
#include "firstboot.h"
#include "catalog.h"
#include "../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
int fb_register_games(const char *directory,int (*query)(void *,unsigned,int,uint32_t *),void *ctx,char *text,size_t cap)
{
    static const char *const ids[]={"wolf3d-shareware","doom-shareware","quake-shareware"};
    struct reg_root *root=NULL;unsigned registered=0;int error=0;
    for(unsigned i=0;i<DESK_GAME_COUNT;i++){
        uint32_t bytes=0;int present=query(ctx,i+1,0,&bytes);
        if(present<0){error=present;break;}
        if(present!=1)continue; /* absent and pending removal stay absent */
        if(!root){root=reg_open_root_at("Programs",directory);if(!root){error=-errno;break;}}
        char key[96],command[128],name[96],image[64];int32_t installed=0;
        snprintf(key,sizeof(key),"Programs\\BundledGames\\%s",ids[i]);
        size_t nc=sizeof(command),nn=sizeof(name),ni=sizeof(image);
        if(!reg_get_int32(root,key,"Installed",&installed) && installed==1 &&
           !reg_get_string(root,key,"Name",name,&nn) && !strcmp(name,desk_games[i].label) &&
           !reg_get_string(root,key,"Command",command,&nc) && !strcmp(command,desk_games[i].command) &&
           !reg_get_string(root,key,"Image",image,&ni) && !strcmp(image,"C:\\Games\\CIUKIGAM.IMG")) {registered++;continue;}
        if(reg_create_key(root,key) || reg_set_string(root,key,"Name",desk_games[i].label) ||
           reg_set_string(root,key,"Command",desk_games[i].command) ||
           reg_set_string(root,key,"Image","C:\\Games\\CIUKIGAM.IMG") || reg_set_int32(root,key,"Installed",1)){error=-errno;break;}
        registered++;
    }
    if(root)reg_close_root(root);
    snprintf(text,cap,"%u bundled games registered in Programs > Games and Add/Remove Programs.",registered);
    return error?error:registered?0:1;
}
