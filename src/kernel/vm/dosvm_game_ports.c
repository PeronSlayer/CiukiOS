/* Per-game launch snapshot; trusted VMM physical services only.
 * Research and format: docs/design/game-properties.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/com.h>
#include <ciuki/lpt.h>
#include <ciuki/files.h>
#include <ciuki/game_ports_settings.h>
#include "dosvm_internal.h"

static void read_settings(struct dosvm *v,struct game_ports_settings *out)
{
    game_ports_defaults(out);
    for (unsigned slot=0;slot<2;slot++) {
        char path[96],text[GAME_PORTS_TEXT_MAX];struct game_ports_settings p;
        ksnprintf(path,sizeof(path),"C:\\SYSTEM\\SETTINGS\\dos-game-%u-ports.%u.ini",v->game,slot);
        int fd=vfs_open(&v->disk_table,path,VFS_READ,VFS_DENY_WRITE,0);
        if (fd==-FS_ENOENT) continue;
        size_t n=0;int e=fd<0 ? fd : vfs_read(&v->disk_table,fd,text,sizeof(text),&n);
        if (fd>=0) { int closed=vfs_close(&v->disk_table,fd);if (!e) e=closed; }
        if (e || !game_ports_decode(text,n,&p) || (p.sequence&1)!=slot) {
            v->ports_changed|=CIUKI_DOSVM_PORT_SETTINGS_INVALID;
            klog("[game-ports] rejected game=%u slot=%u; previous/default settings",v->game,slot);continue;
        }
        if (p.sequence>out->sequence) *out=p;
    }
}
int dosvm_game_ports_apply(struct dosvm *v)
{
    if (!v || !v->game || v->started || !v->table_ready) return -EINVAL;
    struct game_ports_settings p;read_settings(v,&p);
    for (unsigned slot=0;slot<4;slot++) {
        int physical=p.com[slot]==GAME_PORTS_NONE ? -2 : -1;
        if (p.com[slot]>GAME_PORTS_NULL) {
            for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
                struct ciuki_com_status s;
                if (com_snapshot(i,&s,0) && s.base==p.com[slot] &&
                    (s.flags&(CIUKI_COM_READY|CIUKI_COM_LOGGER|CIUKI_COM_OPEN))) { physical=(int)i;break; }
            }
            if (physical<0) v->ports_changed|=CIUKI_DOSVM_PORT_FALLBACK;
        }
        const char *reason=0;
        int e=dosvm_uart_bind(v->info.key,slot,physical,&reason);
        if (e) { klog("[game-ports] COM%u refused: %s (%d)",slot+1,reason,e);return e; }
    }
    for (unsigned i=0;i<3;i++) v->lpt[i].disabled=true;
    v->print_capture.enabled=p.lpt_mode==GAME_LPT_PRINTER;
    if (p.lpt_mode==GAME_LPT_FILE) {
        char path[132];path[0]='C';path[1]=':';
        unsigned i=0;
        for (;p.path[i];i++) path[i+2]=p.path[i]=='/' ? '\\' : p.path[i];
        path[i+2]=0;
        int fd=vfs_open(&v->disk_table,path,VFS_WRITE|VFS_CREATE,VFS_DENY_WRITE,0);
        if (fd>=0) {
            uint64_t at;
            int e=vfs_seek(&v->disk_table,fd,0,VFS_SEEK_END,&at);
            if (!e) { v->printer_fd=fd;v->printer_open=true; }
            else vfs_close(&v->disk_table,fd);
        }
        if (!v->printer_open) v->ports_changed|=CIUKI_DOSVM_PORT_FALLBACK;
    } else if (p.lpt_mode==GAME_LPT_RAW) {
        int physical=-1;
        for (unsigned i=0;i<CIUKI_LPT_MAX;i++) {
            struct ciuki_lpt_status s;
            if (lpt_snapshot(i,&s,0) && s.base==p.lpt_base && (s.flags&CIUKI_LPT_READY)) { physical=(int)i;break; }
        }
        if (physical<0) v->ports_changed|=CIUKI_DOSVM_PORT_FALLBACK;
        else {
            int e=dosvm_lpt_lease(v,0,(unsigned)physical,p.lpt_mode==GAME_LPT_RAW);
            if (e) return e;
            v->lpt[0].disabled=false;
        }
    }
    return dosvm_lpt_bda(v);
}
int dosvm_game_printer_bios(struct dosvm *v,unsigned fn,uint8_t byte)
{
    if (!v->printer_open || v->info.state>=DOSVM_STOPPING) return -ENODEV;
    if (fn) return 0;
    size_t n=0;int e=vfs_write(&v->disk_table,v->printer_fd,&byte,1,&n);
    return e ? px_error(e) : n==1 ? 0 : -EIO;
}
/* The VM worker is quiescent; do not close a file from dosvm_stop(). */
int dosvm_game_ports_close(struct dosvm *v)
{
    if (!v->printer_open) return 0;
    int e=vfs_commit(&v->disk_table,v->printer_fd);
    int closed=vfs_close(&v->disk_table,v->printer_fd);v->printer_open=false;
    return px_error(e ? e : closed);
}
