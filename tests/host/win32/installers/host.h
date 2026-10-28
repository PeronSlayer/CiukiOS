/* Linux adapter boundary; production shell code is compiled unchanged. */
#ifndef INSTALLER_HOST_H
#define INSTALLER_HOST_H
#include <w32/shell.h>
extern char host_root[1024];
extern unsigned host_cancel,host_full,host_refusals,host_spawned,host_closed;
void host_init(const char *);
void host_finish(void);
void host_profile(const char *,const char *);
void host_put(const char *,const char *);
void host_registry(const char *,const char *,const char *);
void host_manifest(const char *);
/* fx-q3vise: USER stand-ins for the shell's folder windows (execute.c). */
struct host_window { char klass[64],title[260]; DWORD style; int live; };
extern struct host_window host_windows[16];
extern unsigned host_window_count,host_folder_launches,host_folder_refuse;
extern char host_folder_launch[16][260];
#endif
