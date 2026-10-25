/* SPDX-License-Identifier: GPL-2.0-only */
#include "program_tree.h"
#define PROGRAM_DEPTH 16u
#define PROGRAM_ENTRIES 16384u
struct frame { struct fat_entry entry; uint32_t cursor; bool counted; };
int program_tree(struct fat_volume *v,unsigned game,bool remove,uint32_t *size)
{
    static const char *const names[]={"WOLF3D","DOOM","QUAKE","WAR2"};
    if(!v || !size || !game || game>4)return -FS_EINVAL;
    *size=0;
    if(remove && v->readonly)return -FS_EROFS;
    size_t bytes=sizeof(struct frame)*PROGRAM_DEPTH;
    struct frame *stack=fs_contig_alloc(bytes);if(!stack)return -FS_ENOMEM;
    memset(stack,0,bytes);
    int e=fat_lookup(v,v->root,names[game-1],&stack[0].entry);
    if(!e && !(stack[0].entry.attr&FAT_ATTR_DIR))e=-FS_ENOTDIR;
    unsigned depth=e ? 0 : 1,budget=PROGRAM_ENTRIES;
    uint64_t total=0;
    while(depth && !e) {
        struct frame *f=&stack[depth-1];
        if(!f->counted) {
            f->counted=true;
            uint32_t c=f->entry.first,chain_budget=v->clusters;
            while(c>=2 && c<v->clusters+2) {
                if(!chain_budget--){e=-FS_ELOOP;break;}
                total+=(uint32_t)v->spc*512u;
                e=fat_get_cluster(v,c,&c);if(e)break;
            }
            if(e)break;
        }
        struct fat_entry child;
        if(f->entry.attr&FAT_ATTR_DIR) {
            e=fat_next(v,f->entry.first,&f->cursor,&child);
            if(!e) {
                if(path_equal(child.alias,".") || path_equal(child.alias,".."))continue;
                if(!budget-- || depth==PROGRAM_DEPTH){e=-FS_ELOOP;break;}
                stack[depth++]=(struct frame){.entry=child};continue;
            }
            if(e!=-FS_ENOENT)break;
            e=0;
        }
        if(remove)e=fat_remove(v,&f->entry,(f->entry.attr&FAT_ATTR_DIR)!=0);
        depth--;
    }
    if(!e && total>UINT32_MAX)e=-FS_EFBIG;
    if(!e)*size=(uint32_t)total;
    fs_contig_free(stack,bytes);return e;
}
