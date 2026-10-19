/* Owned DOS file personality. Internal, versioned guest binding is f3-50.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_FILE_H
#define CIUKI_DOS_FILE_H
#include "../fs/vfs.h"
#define DOS_FILE_JFT 232u
#define DOS_FILE_SFT 128u
#define DOS_FILE_PSPS 16u
#define DOS_FILE_SEARCHES 32u
#define DOS_FILE_TRACE 128u
#define DOS_FILE_OBSERVERS 16u
#define DOS_FILE_WAITS 16u
#define DOS_FILE_DEVICES 22u
#define DOS_FILE_BUDGET_US 2000000u
struct dos_file_far { uint16_t off, seg; };
struct dos_file_frame {
    uint16_t ax,bx,cx,dx,ds,es,si,di,bp,flags,cs,ip,ss,sp;
};
enum dos_file_origin { DOS_FILE_V86, DOS_FILE_DPMI_0300, DOS_FILE_EXTENDER,
                       DOS_FILE_HOST, DOS_FILE_HOOK };
enum dos_file_result { DOS_FILE_CHAIN, DOS_FILE_DONE, DOS_FILE_PENDING };
enum dos_file_event { DOS_FILE_PSP_CREATE=1, DOS_FILE_PSP_SELECT,
    DOS_FILE_EXEC_BEGIN, DOS_FILE_EXEC_COMMIT, DOS_FILE_EXEC_ROLLBACK,
    DOS_FILE_PROCESS_EXIT, DOS_FILE_TSR, DOS_FILE_VM_DESTROY };
enum dos_file_kind { DOS_FILE_REGULAR, DOS_FILE_NUL, DOS_FILE_CON, DOS_FILE_DEVICE };
struct dos_file_error { uint16_t code; uint8_t class_,action,locus; };
struct dos_file;
struct dos_file_ops {
    /* check pins the entire extent for this transaction. release ends all
     * such pins. read/write cannot fail after a successful check. No host
     * pointer is ever stored in guest memory. Time units are microseconds. */
    bool (*check)(void *,uint32_t,size_t,bool);
    void (*read)(void *,uint32_t,void *,size_t);
    void (*write)(void *,uint32_t,const void *,size_t);
    void (*release)(void *);
    uint64_t (*wall)(void *), (*monitor)(void *);
    void (*pause)(void *);
    /* Schedule the guest callback, never call it under a VFS lock. The
     * controller returns its AL through dos_file_complete(token, AL). */
    bool (*critical)(void *,uint64_t,const struct dos_file_frame *,uint8_t,uint8_t);
    void (*abort_process)(void *,uint16_t);
    bool (*stopped)(void *);
    void *arg;
};
struct dos_file_binding {
    struct dos_file_far sda, lists, sft, indos;
    uint16_t sda_bytes, sft_count;
    struct dos_file_far critical_return,critical_device,media;
    struct dos_file_far selected_psp,dta;
};
struct dos_file_view { struct vfs_table *table; unsigned drive; uint32_t generation; bool readonly; };
struct dos_file_process {
    bool used,resident,provisional;
    uint16_t psp,parent,count;
    struct dos_file_far dta,jft,expanded;
    uint8_t handles[DOS_FILE_JFT];
    struct dos_file_error error;
};
struct dos_file_description {
    unsigned refs,kind,mode;
    struct vfs_table *table; int handle;
    uint16_t owner;
    uint8_t drive,device;
    uint32_t generation;
    bool negative_position,written;
};
struct dos_file_device { char name[9]; struct dos_file_far header; uint16_t info; bool used; };
struct dos_file_cursor { bool used; uint32_t id,nonce; uint16_t psp; uint8_t drive;
    struct vfs_find find; struct vfs_table *table; };
struct dos_file_trace {
    struct dos_file_frame frame;
    uint64_t sequence,wall_us,monitor_us;
    uint32_t generation,bytes,dropped;
    uint16_t psp; uint8_t origin,vector; bool returning;
};
struct dos_file_observer { bool used; struct dos_file_frame input;
    uint64_t sequence,wall,monitor; uint16_t psp; uint8_t origin,vector; uint32_t bytes; };
struct dos_file_wait { struct vfs_wait_info info; uint64_t sequence; uint32_t generation; uint16_t psp; uint8_t drive; };
struct dos_file {
    struct dos_file_ops ops; struct dos_file_binding binding;
    uint32_t id,generation; bool bound,a20,dead,trace_enabled,critical,pending_commit;
    uint8_t drive,active_drive; uint16_t selected,owning,aborted_psp;
    uint16_t retry_pause,retry_count;
    int cleanup_status; /* first cleanup error, retained even after PSP release */
    uint64_t sequence,request_sequence,deadline,token,request_wall,request_monitor;
    enum dos_file_origin origin; uint8_t vector;
    uint32_t transferred,search_sequence,temp_sequence;
    struct dos_file_frame pending,callback;
    struct vfs_request request;
    struct vfs_wait_info wait;
    struct dos_file_wait waits[DOS_FILE_WAITS]; unsigned wait_head,wait_count;
    struct dos_file_process process[DOS_FILE_PSPS];
    struct dos_file_description sft[DOS_FILE_SFT];
    struct dos_file_view view[26];
    struct dos_file_device device[DOS_FILE_DEVICES];
    struct dos_file_far dpb[26];
    struct dos_file_cursor cursor[DOS_FILE_SEARCHES];
    struct dos_file_trace trace[DOS_FILE_TRACE];
    unsigned trace_head,trace_count; uint32_t dropped;
    struct dos_file_observer observer[DOS_FILE_OBSERVERS];
    uint8_t transfer[4096];
};
void dos_file_init(struct dos_file *,uint32_t vm,uint32_t generation,const struct dos_file_ops *);
int dos_file_bind(struct dos_file *,const struct dos_file_binding *);
int dos_file_map(struct dos_file *,unsigned guest,struct vfs_table *,unsigned native,bool readonly);
int dos_file_native_views(struct dos_file *,struct vfs_table *,bool removable_writable);
/* f3-50 imports real personality driver/DPB addresses before root PSP creation.
 * Never synthesize a driver address or infer redirection from a handle number. */
int dos_file_register_device(struct dos_file *,const char *,struct dos_file_far,uint16_t info);
int dos_file_register_dpb(struct dos_file *,unsigned drive,struct dos_file_far);
int dos_file_lifecycle(struct dos_file *,enum dos_file_event,uint16_t psp,uint16_t parent,struct dos_file_far expanded);
enum dos_file_result dos_file_dispatch(struct dos_file *,struct dos_file_frame *,enum dos_file_origin,unsigned vector);
enum dos_file_result dos_file_complete(struct dos_file *,uint64_t token,unsigned reply,struct dos_file_frame *);
void dos_file_cancel(struct dos_file *);
bool dos_file_poll(struct dos_file *,struct dos_file_frame *);
void dos_file_observe_return(struct dos_file *,const struct dos_file_frame *);
bool dos_file_trace_pop(struct dos_file *,struct dos_file_trace *);
bool dos_file_wait_pop(struct dos_file *,struct dos_file_wait *);
void dos_file_error_from_fs(struct dos_file_error *,int error,bool writing);
bool dos_file_serves(unsigned vector,uint16_t ax);
#endif
