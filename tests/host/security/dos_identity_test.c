/* H06 (sec-k05): DOS VMs and the shared policy. Production k01 credentials
 * and sessions, k02 resolved-object policy/owner labels, native namespace,
 * VFS/FAT (the RK files fixtures), the DOS identity adapter and the live
 * DFV2 IFS backend (dos_unified_ifs.c) over synthetic guest memory shaped
 * like K2043's SDA/SFT/CDS; production per-user store. Expectations are
 * written from the contract (user-security-contract.md 8.4,
 * user-security-kernel.md "Removable media, DOS and Win32"), not derived
 * from the implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"
#include "../../../src/kernel/fs/owner.h"
#include "../../../src/kernel/vm/dos_identity.h"
#include "../../../src/kernel/vm/dos_unified_ifs.h"
#include "../../../src/kernel/vm/dos_file_volume.h"
#include "../../../src/kernel/vm/dos_save_store.h"


/* ---- accounts and sessions ------------------------------------------ */
struct user { const char *name, *root; uint32_t uid; bool admin, guest; };
static const struct user ALICE={"alice","/Windows/Profiles/u00000003",1007,false,false}; /* migrated: retained root */
static const struct user BOB={"bob","/Windows/Profiles/u000003ef",1001,false,false};
static const struct user ADMIN={"admin","/Windows/Profiles/u000003e8",1000,true,false};
static const struct user GUEST={"guest","/Windows/Profiles/u000001f5",501,false,true};
struct actor { struct security_subject subject; struct process process; uint32_t session; };
static struct ciuki_credential credential_of(const struct user *u)
{
    struct ciuki_credential c={.version=1,.size=64,.uid=u->uid,.primary_gid=545,.group_count=1,.groups={545},
        .flags=CIUKI_CRED_INTERACTIVE};
    if (u->admin) { c.group_count=2; c.groups[0]=544; c.groups[1]=545; }
    if (u->guest) { c.primary_gid=546; c.groups[0]=546; }
    return c;
}
static struct ciuki_security_profile profile_of(const struct user *u,unsigned key)
{
    struct ciuki_security_profile p={.version=1,.size=112,.uid=u->uid,.volume_id_lo=31,.owner_generation_lo=1,
        .owner_key_lo=key,.path_bytes=(uint32_t)strlen(u->root)+1};
    memcpy(p.path,u->root,p.path_bytes); return p;
}
/* A live process of the logged-on user, holding the real retained subject. */
static void logon(struct actor *a,const struct user *u,unsigned key)
{
    struct ciuki_credential c=credential_of(u); struct ciuki_security_profile p=profile_of(u,key);
    int e=security_session_create(&c,&p,&a->session);
    if (e) printf("logon %s: error %d\n",u->name,e);
    CHECK(!e);
    struct security_session *s=session_find(a->session); CHECK(s);
    a->subject=(struct security_subject){.credential=s->credential,.session=s,.refs=1,.live=true,.pid=900+key};
    s->refs++;
    memset(&a->process,0,sizeof(a->process)); a->process.subject=&a->subject;
    a->process.pid=900+key; a->process.state=PROC_LIVE;
}
/* Logoff: the session leaves ACTIVE (k01 teardown); the actor's process
 * then exits, dropping its subject. Every DOS reference must be back. */
static void end_session(struct actor *a)
{ uint32_t state=0; CHECK(!security_session_end(a->session,true,&state) && state==CIUKI_SECURITY_SESSION_DEAD); }
static void finish(struct actor *a)
{
    uint32_t state=0; CHECK(a->subject.refs==1);
    struct security_session *s=a->subject.session; a->subject.live=false;
    CHECK(s->refs); s->refs--;
    CHECK(!security_session_end(a->session,false,&state));
}
static void logoff(struct actor *a) { end_session(a); finish(a); }

/* ---- synthetic guest memory and K2043-shaped structures --------------- */
#define GUEST_BYTES 0x10000u
#define SDA_SEG 0x0100u
#define CDS_SEG 0x0200u
#define SFT_SEG 0x0300u
#define DTA_SEG 0x0400u
#define DATA_SEG 0x0500u
#define SFT_SLOTS 20u
static uint8_t guest[GUEST_BYTES];
static bool g_check(void *a,uint32_t lin,size_t n,bool w) { (void)a; (void)w; return n && lin<GUEST_BYTES && n<=GUEST_BYTES-lin; }
static void g_read(void *a,uint32_t lin,void *b,size_t n) { (void)a; memcpy(b,guest+lin,n); }
static void g_write(void *a,uint32_t lin,const void *b,size_t n) { (void)a; memcpy(guest+lin,b,n); }
static void g_release(void *a) { (void)a; }
static uint64_t g_wall(void *a) { (void)a; return 1000; }
static void g_pause(void *a) { (void)a; }
static bool g_stopped(void *a) { (void)a; return false; }
static uint32_t lin(uint16_t seg,uint16_t off) { return ((uint32_t)seg<<4)+off; }
static void put16(uint32_t at,uint16_t v) { guest[at]=(uint8_t)v; guest[at+1]=(uint8_t)(v>>8); }
static uint16_t get16(uint32_t at) { return (uint16_t)(guest[at]|guest[at+1]<<8); }

/* ---- the VM: identity, gate, private C: provider, host D:/E: --------- */
#define C_SECTORS 8192u
static uint8_t c_disk[C_SECTORS*512u];
static int c_read(struct blkdev *d,uint64_t lba,uint32_t n,void *b)
{ int e=blkdev_range(d,lba,n); if (!e) memcpy(b,c_disk+lba*512u,(size_t)n*512u); return e; }
static int c_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *b)
{ int e=blkdev_range(d,lba,n); if (!e) memcpy(c_disk+lba*512u,b,(size_t)n*512u); return e; }
static int c_flush(struct blkdev *d) { (void)d; return 0; }
static struct blkdev CPROV={.read=c_read,.write=c_write,.flush=c_flush,.capacity=C_SECTORS,.sector_size=512,
    .write_cache_state=BLKDEV_CACHE_DISABLED,.logical=true};
static void c_format(void)
{
    const unsigned fat_sectors=C_SECTORS/256u; memset(c_disk,0,sizeof(c_disk)); uint8_t *b=c_disk;
    b[0]=0xeb; b[1]=0x3c; b[2]=0x90; memcpy(b+3,"CIUKIC  ",8);
    fs_wr16(b+11,512); b[13]=1; fs_wr16(b+14,1); b[16]=2; fs_wr16(b+17,512); fs_wr16(b+19,(uint16_t)C_SECTORS);
    b[21]=0xf8; fs_wr16(b+22,(uint16_t)fat_sectors); fs_wr16(b+24,32); fs_wr16(b+26,4); b[38]=0x29;
    fs_wr32(b+39,0x0c0c0c0c); memcpy(b+43,"PRIVATE C: ",11); memcpy(b+54,"FAT16   ",8); b[510]=0x55; b[511]=0xaa;
    for (unsigned i=0;i<2;i++) { fs_wr16(c_disk+(1+i*fat_sectors)*512,0xfff8); fs_wr16(c_disk+(1+i*fat_sectors)*512+2,0xffff); }
}
static struct vfs_table *host;           /* the native table D:/E: views use */
static struct dos_file_volume cvol;
static struct dos_unified U;
static struct dfv2_ifs IFS;
static struct dfv2_backend BACK;
static struct dos_identity VMID;
static struct dos_host_gate GATE;
static bool e_present;
static int h_mount(void *a,struct dfv2_ifs *s)
{
    (void)a;
    int e=dos_file_volume_open(&cvol,&cache,&CPROV,true);
    if (!e) e=dfv2_ifs_map(s,2,&cvol.table,2,false);            /* VM-private C: */
    if (!e) e=dfv2_ifs_map_host(s,3,host,2,&GATE);               /* D: system view */
    if (!e && e_present) e=dfv2_ifs_map_host(s,4,host,3,&GATE);  /* E: removable */
    if (e) (void)dos_file_volume_close(&cvol);
    return e;
}
static void h_unmount(void *a,struct dfv2_ifs *s) { (void)a; (void)s; CHECK(!dos_file_volume_close(&cvol)); }
static void vm_start(struct actor *owner)
{
    CHECK(!dos_identity_capture(&VMID,&owner->process)); dos_identity_gate(&VMID,&GATE);
    memset(guest,0,sizeof(guest)); memset(&U,0,sizeof(U));
    struct dfv2_layout *l=&U.layout;
    l->sda=(struct dos_file_far){0,SDA_SEG}; l->sda_bytes=0x300; l->cds=(struct dos_file_far){0,CDS_SEG};
    l->cds_count=26; l->cds_stride=88; l->sft_count=SFT_SLOTS; l->driver_seg=0x600;
    for (unsigned i=0;i<SFT_SLOTS;i++) l->slot[i]=(struct dos_file_far){(uint16_t)(i*59),SFT_SEG};
    uint32_t c=lin(CDS_SEG,2*88); guest[c]='C'; guest[c+1]=':'; guest[c+2]='\\'; put16(c+0x43,0x4000); put16(c+0x4f,2);
    struct dos_file_ops ops={.check=g_check,.read=g_read,.write=g_write,.release=g_release,.wall=g_wall,.monitor=g_wall,
        .pause=g_pause,.stopped=g_stopped};
    struct dfv2_ifs_host h={h_mount,h_unmount,0};
    dfv2_ifs_init(&IFS,&U,&ops,&h,1,7); dfv2_ifs_backend(&IFS,&BACK);
    U.staged=&U.layout;
    CHECK(!BACK.prepare(BACK.arg,DFV2_EVENT_BIND,0x60,0)); BACK.commit(BACK.arg,DFV2_EVENT_BIND,0x60);
    U.staged=0;
    CHECK(IFS.active && IFS.owned==((1u<<2)|(1u<<3)|(e_present ? 1u<<4 : 0)));
    put16(lin(SDA_SEG,DFV2_SDA_PSP),0x60);
}
static void vm_stop(void)
{
    BACK.destroy(BACK.arg); CHECK(!dfv2_ifs_held(&IFS) && !IFS.mounted);
    dos_identity_release(&VMID);
    host->request=0; host->owner=0;   /* production gives each VM its own native table */
}
/* One redirector call as K2043 issues it; DOS error or 0. */
static uint16_t last_cx;
static int ifs(uint16_t ax,uint16_t cx,uint16_t arg,unsigned slot)
{
    struct dfv2_call k={.role=DFV2_ROLE_LOCAL,.ax=ax,.cx=cx,.arg=arg,.es=SFT_SEG,.di=(uint16_t)(slot*59),.flags=0x202};
    struct dfv2_critical crit={0};
    enum dos_file_result r=BACK.request(BACK.arg,DFV2_ROLE_LOCAL,&k,&crit);
    CHECK(r==DOS_FILE_DONE);           /* an owned drive never chains to a local FAT */
    CHECK(!crit.raise);                /* denial is not a critical error */
    last_cx=k.cx; return (k.flags&1) ? k.ax : 0;
}
static void fn1(const char *path)
{ uint32_t at=lin(SDA_SEG,DFV2_SDA_FN1); memset(guest+at,0,128); CHECK(strlen(path)<128); memcpy(guest+at,path,strlen(path)); }
static void dta(uint16_t seg) { put16(lin(SDA_SEG,DFV2_SDA_DTA),0); put16(lin(SDA_SEG,DFV2_SDA_DTA)+2,seg); }
static int dos_open(const char *path,unsigned slot)
{
    uint32_t at=lin(SFT_SEG,(uint16_t)(slot*59)); memset(guest+at,0,59);
    fn1(path); int e=ifs(0x1116,0,0x40,slot);   /* read, deny none */
    if (!e) { put16(at,1); put16(at+0x31,0x60); }
    return e;
}
static int dos_read(unsigned slot,unsigned bytes,char *out)
{
    dta(DATA_SEG); memset(guest+lin(DATA_SEG,0),0,bytes+1);
    int e=ifs(0x1108,(uint16_t)bytes,0,slot);
    if (!e && out) { memcpy(out,guest+lin(DATA_SEG,0),last_cx); out[last_cx]=0; }
    return e;
}
static int dos_create(const char *path,unsigned slot)
{
    uint32_t at=lin(SFT_SEG,(uint16_t)(slot*59)); memset(guest+at,0,59); put16(at+2,0x0042);
    fn1(path); int e=ifs(0x1117,0,0,slot);
    if (!e) { put16(at,1); put16(at+0x31,0x60); }
    return e;
}
static int dos_write(unsigned slot,const char *text)
{
    dta(DATA_SEG); memcpy(guest+lin(DATA_SEG,0),text,strlen(text));
    return ifs(0x1109,(uint16_t)strlen(text),0,slot);
}
static int dos_path(uint16_t ax,const char *path) { fn1(path); return ifs(ax,0,0,0); }
/* FindFirst/FindNext; the 8.3 names DOS shows, space-separated. */
static int dos_list(const char *pattern,char *names,size_t size)
{
    names[0]=0; fn1(pattern); guest[lin(SDA_SEG,DFV2_SDA_ATTR)]=0x16; dta(DTA_SEG);
    int e=ifs(0x111b,0,0,0);
    for (unsigned n=0;!e && n<64;n++) {
        const uint8_t *f=guest+lin(DTA_SEG,21); char name[13]; unsigned k=0;
        for (unsigned i=0;i<8 && f[i]!=' ';i++) name[k++]=(char)f[i];
        if (f[8]!=' ') { name[k++]='.'; for (unsigned i=8;i<11 && f[i]!=' ';i++) name[k++]=(char)f[i]; }
        name[k]=0; size_t used=strlen(names); CHECK(used+k+2<size);
        snprintf(names+used,size-used,"%s%s",used ? " " : "",name);
        e=ifs(0x111c,0,0,0);
    }
    return e==18 ? 0 : e;
}
static char alias_of(const char *native,char out[16])
{
    struct fat_entry entry; CHECK(!vfs_stat(host,native,&entry));
    snprintf(out,16,"%s",entry.alias[0] ? entry.alias : entry.name); return out[0];
}

/* ---- fixture namespace and policy ---------------------------------------- */
static void file_put(const char *path,const char *text)
{
    int fd=vfs_open(host,path,VFS_CREATE|VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0); CHECK(fd>=0);
    size_t n; CHECK(!vfs_write(host,fd,text,strlen(text),&n) && n==strlen(text)); CHECK(!vfs_close(host,fd));
}
static void record_profile(struct owner_store *owners,unsigned index,const struct user *u)
{
    struct px_path r; CHECK(!resolve_path(u->root,&r));
    struct owner_record *rec=&owners->profiles[index]; memset(rec,0,sizeof(*rec));
    rec->binding=(struct owner_binding){.uid=u->uid,.action=1,.volume=31,.key=index+1,.locator=index+1,.generation=1};
    memcpy(rec->binding.path,u->root,strlen(u->root)+1); CHECK(!px_object_proof_locked(r.node,rec->proof));
}

static struct actor alice,bob,admin,guest_actor;
static char a_alias[16],b_alias[16];

static void test_identity_capture(void)
{
    struct dos_identity id; char root[DOS_IDENTITY_PATH];
    /* SYSTEM (PID 1, session 0) has no DOS identity of its own. */
    CHECK(dos_identity_capture(&id,proc_supervisor())==-EPERM && !id.subject);
    logon(&alice,&ALICE,1);
    CHECK(!dos_identity_capture(&id,&alice.process) && alice.subject.refs==2);
    CHECK(id.credential.uid==1007 && id.credential.session_id==alice.session && !id.admin && !id.system && !id.guest);
    /* A2-8: the committed retained root, never u000003ef derived from 1007. */
    CHECK(!dos_identity_root(&id,DOS_ROOT_STORE,root,sizeof(root)) && !strcmp(root,"C:\\Windows\\Profiles\\u00000003\\Settings\\DOS"));
    CHECK(!dos_identity_root(&id,DOS_ROOT_SCRATCH,root,sizeof(root)) && !strcmp(root,"C:\\Windows\\Profiles\\u00000003\\Settings\\DOS"));
    CHECK(!dos_identity_root(&id,DOS_ROOT_EXPORT,root,sizeof(root)) && !strcmp(root,"C:\\Windows\\Profiles\\u00000003\\Documents\\Saves"));
    char name[32]; CHECK(!dos_identity_export_name(&id,"C:\\Windows\\Profiles\\u00000003\\Documents\\Saves\\G1-0",name) &&
        !strcmp(name,"Documents\\Saves\\G1-0"));
    CHECK(dos_identity_export_name(&id,"C:\\Windows\\Profiles\\u000003ef\\Documents\\Saves\\G1-0",name)==-FS_EINVAL);
    /* Immutable: a copy holds its own reference; the owner is the same
     * credential and live session only, never just a pid or a uid. */
    struct dos_identity copy; CHECK(!dos_identity_copy(&copy,&id) && alice.subject.refs==3);
    CHECK(dos_identity_owner(&copy,&alice.process) && !dos_identity_owner(&copy,proc_supervisor()));
    struct process impostor={.pid=alice.process.pid,.state=PROC_LIVE};
    struct security_subject forged={.credential=alice.subject.credential,.live=true,.refs=1};
    forged.credential.session_id++; impostor.subject=&forged;
    CHECK(!dos_identity_owner(&copy,&impostor));
    dos_identity_release(&copy); dos_identity_release(&id); CHECK(alice.subject.refs==1);
    /* A closing/dead session admits no new identity; a retained one is no
     * longer live or the owner of anything. */
    CHECK(!dos_identity_capture(&id,&alice.process) && dos_identity_live(&id));
    end_session(&alice);
    CHECK(!dos_identity_live(&id) && !dos_identity_owner(&id,&alice.process));
    struct dos_identity late; CHECK(dos_identity_capture(&late,&alice.process)==-EPERM && !late.subject);
    dos_identity_release(&id); finish(&alice);
    /* Guest keeps its own private data in its own profile. */
    logon(&guest_actor,&GUEST,4);
    CHECK(!dos_identity_capture(&id,&guest_actor.process) && id.guest && !id.admin);
    CHECK(!dos_identity_root(&id,DOS_ROOT_STORE,root,sizeof(root)) && !strcmp(root,"C:\\Windows\\Profiles\\u000001f5\\Settings\\DOS"));
    dos_identity_release(&id); logoff(&guest_actor);
    /* The interim bootstrap desktop (no committed profile) is the legacy
     * administrator; a Standard session without a profile is refused. */
    struct ciuki_credential c=credential_of(&ADMIN); struct ciuki_security_profile none={.version=1,.size=112,.uid=1000};
    uint32_t sid; CHECK(!session_reserve(&c,&none,&sid)); session_find(sid)->bootstrap=true;
    struct actor boot={.subject={.credential=session_find(sid)->credential,.session=session_find(sid),.refs=1,.live=true}};
    boot.subject.session->refs++; boot.process=(struct process){.pid=950,.state=PROC_LIVE,.subject=&boot.subject}; boot.session=sid;
    CHECK(!dos_identity_capture(&id,&boot.process) && id.admin && !id.profile[0]);
    CHECK(!dos_identity_root(&id,DOS_ROOT_STORE,root,sizeof(root)) && !strcmp(root,DOS_SAVE_ROOT));
    dos_identity_release(&id); logoff(&boot);
    c=credential_of(&BOB); none.uid=1001; CHECK(!session_reserve(&c,&none,&sid));
    struct actor plain={.subject={.credential=session_find(sid)->credential,.session=session_find(sid),.refs=1,.live=true}};
    plain.subject.session->refs++; plain.process=(struct process){.pid=951,.state=PROC_LIVE,.subject=&plain.subject}; plain.session=sid;
    CHECK(dos_identity_capture(&id,&plain.process)==-EACCES && !id.subject && plain.subject.refs==1);
    logoff(&plain);
    puts("H06 identity: PASS (interactive session only, retained-root profile locations, own reference/copy, owner = credential+live session, revocation at logoff, Guest own profile, bootstrap legacy, Standard without profile refused)");
}

static void test_host_views(void)
{
    char text[128],names[512],path[160];
    logon(&alice,&ALICE,1);
    c_format(); vm_start(&alice);
    /* Own profile through its 8.3 alias: readable, then each transfer is
     * rechecked against the live subject. */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\PRIVATE.TXT",a_alias);
    unsigned opens=IFS.stats.opens,denied=IFS.stats.denied;
    CHECK(!dos_open(path,1) && !dos_read(1,64,text) && !strcmp(text,"alice private"));
    CHECK(!ifs(0x1106,0,0,1));
    /* Another user's private data: DOS error 5 before any native open. */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\PRIVATE.TXT",b_alias);
    CHECK(dos_open(path,2)==5 && IFS.stats.opens==opens+1 && IFS.stats.denied==denied+1);
    CHECK(dos_path(0x110f,path)==5);                                      /* attributes */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s",b_alias);
    CHECK(dos_path(0x1105,path)==5);                                      /* CHDIR */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\*.*",b_alias);
    CHECK(dos_list(path,names,sizeof(names))==5);                         /* listing */
    /* A traversal through someone else's directory never collapses away. */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\..\\%s\\PRIVATE.TXT",b_alias,a_alias);
    CHECK(dos_open(path,2)==5);
    /* Filtered enumeration: only accessible entries, by their real aliases. */
    unsigned hidden=IFS.stats.hidden;
    CHECK(!dos_list("D:\\WINDOWS\\PROFILES\\*.*",names,sizeof(names)));
    char expect[64]; snprintf(expect,sizeof(expect),". .. %s",a_alias);
    CHECK(!strcmp(names,expect) && IFS.stats.hidden>=hidden+2);
    CHECK(!dos_list("D:\\SYSTEM\\*.*",names,sizeof(names)) && !strstr(names,"SECURITY"));
    CHECK(dos_open("D:\\SYSTEM\\SECURITY\\ACCOUNTS.DB",2)==5);
    /* Public system content stays readable; D: is never writable, including
     * a date set on a read handle (5701h) applied at close. */
    CHECK(!dos_open("D:\\WINDOWS\\WIN.INI",3) && !dos_read(3,64,text) && !strcmp(text,"[windows]") && !ifs(0x1106,0,0,3));
    struct fat_entry before,after; CHECK(!vfs_stat(host,"C:\\Windows\\WIN.INI",&before));
    CHECK(!dos_open("D:\\WINDOWS\\WIN.INI",3)); unsigned held=dfv2_ifs_held(&IFS);
    uint32_t sft=lin(SFT_SEG,3*59); put16(sft+5,(uint16_t)(get16(sft+5)|0x4000)); put16(sft+0x0f,0x2121); put16(sft+0x0d,0x0842);
    CHECK(ifs(0x1106,0,0,3)==5 && dfv2_ifs_held(&IFS)==held-1);
    CHECK(!vfs_stat(host,"C:\\Windows\\WIN.INI",&after) && after.times.write_date==before.times.write_date &&
          after.times.write_time==before.times.write_time);
    CHECK(dos_create("D:\\WINDOWS\\PROFILES\\NEW.TXT",4)==5 && dos_create("D:\\NEW.TXT",4)==5);
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\NEW.TXT",a_alias);
    CHECK(dos_create(path,4)==5 && dos_path(0x1103,"D:\\WORK")==5 && dos_path(0x1113,"D:\\WINDOWS\\WIN.INI")==5);
    /* The private C: is the VM's own FAT16: short work/temp paths, writable,
     * never the host system volume. */
    CHECK(!dos_path(0x1103,"C:\\WORK") && !dos_path(0x1103,"C:\\TEMP"));
    CHECK(!dos_create("C:\\TEMP\\SETUP.TMP",5) && !dos_write(5,"extracted") && last_cx==9 && !ifs(0x1106,0,0,5));
    CHECK(!dos_open("C:\\TEMP\\SETUP.TMP",5) && !dos_read(5,32,text) && !strcmp(text,"extracted") && !ifs(0x1106,0,0,5));
    struct fat_entry entry; CHECK(vfs_stat(host,"C:\\TEMP\\SETUP.TMP",&entry)==-FS_ENOENT && vfs_stat(host,"C:\\WORK",&entry)==-FS_ENOENT);
    /* Revocation: a handle opened while ACTIVE reads nothing after logoff. */
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\PRIVATE.TXT",a_alias);
    CHECK(!dos_open(path,6));
    end_session(&alice);
    CHECK(dos_read(6,64,text)==5 && dos_open("D:\\WINDOWS\\WIN.INI",7)==5);
    CHECK(!ifs(0x1106,0,0,6));
    vm_stop(); finish(&alice);
    /* Administrator: other users' ordinary profile data, never SYSTEM secrets. */
    logon(&admin,&ADMIN,3); c_format(); vm_start(&admin);
    snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\PRIVATE.TXT",b_alias);
    CHECK(!dos_open(path,1) && !dos_read(1,64,text) && !strcmp(text,"bob private") && !ifs(0x1106,0,0,1));
    CHECK(dos_open("D:\\SYSTEM\\SECURITY\\ACCOUNTS.DB",2)==5 && dos_create("D:\\ADMIN.TXT",2)==5);
    CHECK(!dos_list("D:\\SYSTEM\\*.*",names,sizeof(names)) && !strstr(names,"SECURITY"));
    vm_stop(); logoff(&admin);
    puts("H06 D: view: PASS (own alias readable, other profile/secret denied with DOS 5 and no native open, unauthorized ancestor not collapsed, filtered 8.3 enumeration, read-only, private C:\\WORK/C:\\TEMP not on the host, per-transfer revocation at logoff, administrator scope)");
}

static void test_removable_view(void)
{
    char text[64],names[256];
    /* E: present, its volume not yet bound: an unmanaged removable volume
     * (user-security-kernel.md rule table, "Unmanaged removable"; the k03
     * resolver's unmanaged_check). The live session reads; the read-only
     * provider still never writes. */
    e_present=true; logon(&bob,&BOB,2); c_format(); vm_start(&bob);
    CHECK(!dos_open("E:\\SHORT.BIN",1) && !dos_read(1,4,text) && !ifs(0x1106,0,0,1));
    CHECK(!dos_list("E:\\*.*",names,sizeof(names)) && strstr(names,"SHORT.BIN"));
    CHECK(dos_create("E:\\BOB.TXT",2)==5 && dos_path(0x1103,"E:\\DIR")==5);
    vm_stop();
    struct policy_volume *removable=calloc(1,sizeof(*removable)); CHECK(removable);
    CHECK(!policy_init(removable,33,3,false,true,false,0));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,3,removable)); vfs_unlock(&vfs);
    c_format(); vm_start(&bob);
    CHECK(!dos_open("E:\\SHORT.BIN",1) && !dos_read(1,4,text) && !ifs(0x1106,0,0,1));
    CHECK(!dos_list("E:\\*.*",names,sizeof(names)) && strstr(names,"SHORT.BIN"));
    /* Security never widens the read-only removable provider. */
    CHECK(dos_create("E:\\BOB.TXT",2)==5 && dos_path(0x1103,"E:\\DIR")==5);
    vm_stop(); logoff(&bob);
    logon(&guest_actor,&GUEST,4); c_format(); vm_start(&guest_actor);
    CHECK(!dos_open("E:\\SHORT.BIN",1) && !ifs(0x1106,0,0,1) && dos_create("E:\\GUEST.TXT",2)==5);
    vm_stop(); logoff(&guest_actor);
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,3); vfs_unlock(&vfs); free(removable); e_present=false;
    puts("H06 E: view: PASS (unbound removable readable by the live session, bound removable read for Standard/Guest, read-only provider never widened)");
}

static void test_gate_swap(void)
{
    logon(&alice,&ALICE,1);
    struct dos_identity id; CHECK(!dos_identity_capture(&id,&alice.process));
    struct dos_host_gate gate; dos_identity_gate(&id,&gate);
    int other=vfs_open(host,"C:\\Windows\\WIN.INI",VFS_READ,VFS_DENY_NONE,0); CHECK(other>=0);
    void *object=0;
    /* A handle naming a different object than the authorized path. */
    CHECK(gate.retain(&gate,host,"C:\\Windows\\Profiles\\u00000003\\private.txt",POLICY_READ,other,&object)==-FS_EACCES && !object);
    CHECK(!gate.retain(&gate,host,"C:\\Windows\\WIN.INI",POLICY_READ,other,&object) && object);
    CHECK(!gate.recheck(&gate,host,object,POLICY_READ) && gate.recheck(&gate,host,object,POLICY_WRITE)==-FS_EACCES);
    gate.release(&gate,host,object); CHECK(!vfs_close(host,other));
    /* A system identity has no gate subject: every gate call denies. */
    struct dos_identity internal; dos_identity_internal(&internal); dos_identity_gate(&internal,&gate);
    CHECK(gate.check(&gate,host,"C:\\Windows\\WIN.INI",POLICY_READ)==-FS_EACCES);
    dos_identity_release(&internal);
    dos_identity_release(&id); logoff(&alice);
    puts("H06 gate: PASS (open/path swap refused, retained object rechecked, internal identity never a user gate)");
}

/* Two users' private C: data: own store under PROFILE(uid), never another
 * user's overlay; the shared policy denies cross-user access to it. */
static void test_private_stores(void)
{
    logon(&alice,&ALICE,1);
    struct dos_identity a; CHECK(!dos_identity_capture(&a,&alice.process));
    struct dos_save_identity title={.game=1,.kernel=2}; memset(title.base,'a',64); title.base[64]=0;
    char root[DOS_IDENTITY_PATH],path[DOS_SAVE_PATH],cow_a[DOS_SAVE_PATH]; unsigned archived;
    CHECK(!dos_identity_root(&a,DOS_ROOT_STORE,root,sizeof(root)) && !dos_identity_prepare(&a,host,DOS_ROOT_STORE));
    static struct dos_save save; struct dos_save_file file={.table=host};
    uint8_t sector[512]; memset(sector,0x5a,sizeof(sector));
    CHECK(!dos_save_store_open(&file,&save,&title,root,cow_a,&archived) && !archived && !save.rows);
    CHECK(!dos_save_write(&save,100,sector) && !dos_save_commit(&save,true)); dos_save_release(&save); CHECK(!vfs_close(host,file.fd));
    struct dos_host_gate gate; dos_identity_gate(&a,&gate);
    CHECK(!gate.check(&gate,host,cow_a,POLICY_READ|POLICY_WRITE));
    dos_identity_release(&a); logoff(&alice);
    logon(&bob,&BOB,2);
    struct dos_identity b; CHECK(!dos_identity_capture(&b,&bob.process)); dos_identity_gate(&b,&gate);
    CHECK(!dos_identity_root(&b,DOS_ROOT_STORE,root,sizeof(root)) && strcmp(root,"C:\\Windows\\Profiles\\u00000003\\Settings\\DOS"));
    CHECK(!dos_identity_prepare(&b,host,DOS_ROOT_STORE));
    save=(struct dos_save){0}; file=(struct dos_save_file){.table=host};
    CHECK(!dos_save_store_open(&file,&save,&title,root,path,&archived) && !save.rows && strcmp(path,cow_a));
    uint8_t got[512]; CHECK(dos_save_read(&save,100,got)==-FS_ENOENT);
    dos_save_release(&save); CHECK(!vfs_close(host,file.fd));
    CHECK(gate.check(&gate,host,cow_a,POLICY_READ)==-FS_EACCES);           /* Bob: Alice's overlay */
    dos_identity_release(&b); logoff(&bob);
    logon(&guest_actor,&GUEST,4); struct dos_identity g; CHECK(!dos_identity_capture(&g,&guest_actor.process));
    dos_identity_gate(&g,&gate); CHECK(gate.check(&gate,host,cow_a,POLICY_READ)==-FS_EACCES);
    dos_identity_release(&g); logoff(&guest_actor);
    logon(&admin,&ADMIN,3); struct dos_identity m; CHECK(!dos_identity_capture(&m,&admin.process));
    dos_identity_gate(&m,&gate); CHECK(!gate.check(&gate,host,cow_a,POLICY_READ));  /* explicit file management */
    dos_identity_release(&m); logoff(&admin);
    /* Alice again: her data persisted in her own profile. */
    logon(&alice,&ALICE,1); CHECK(!dos_identity_capture(&a,&alice.process));
    CHECK(!dos_identity_root(&a,DOS_ROOT_STORE,root,sizeof(root)));
    save=(struct dos_save){0}; file=(struct dos_save_file){.table=host};
    CHECK(!dos_save_store_open(&file,&save,&title,root,path,&archived) && save.rows==1 && !strcmp(path,cow_a));
    CHECK(!dos_save_read(&save,100,got) && !memcmp(got,sector,512));
    dos_save_release(&save); CHECK(!vfs_close(host,file.fd));
    dos_identity_release(&a); logoff(&alice);
    puts("H06 stores: PASS (Alice/Bob separate COW under their own profiles, persistence across sessions, Bob/Guest denied Alice's overlay, administrator file management)");
}

#ifdef CIUKI_POLICY_COVERAGE
/* ---- sec-k08 H13 (tests/host/security/test_policy_coverage.py): the DOS
 * VM frontend. A Standard user's (Bob's) and Guest's VM try another user's
 * (Alice's) profile, hive and crash logs, SYSTEM state and the sealed
 * boot/service set through the production DFV2 redirector on D:; each
 * attempt fails with DOS error 5 (or 3/2 where the name is filtered), four
 * repetitions change no VM hold, IFS statistic other than the denial count,
 * open description, pin, page or heap block, and the VM's teardown returns
 * every hold and the subject reference (finish() requires refs==1). ---- */
static unsigned h13_cases;
struct h13_state { unsigned pages,heap,held,opens; uint32_t descriptions,pins; };
static void h13_snapshot(struct h13_state *s)
{
    struct file_ledger l; files_snapshot(space,&l);
    *s=(struct h13_state){pages_used,heap_blocks,dfv2_ifs_held(&IFS),IFS.stats.opens,l.descriptions,l.pins};
}
static void h13_refused(const char *what,uint16_t ax,const char *path,unsigned slot)
{
    int first=ax==0x1116 ? dos_open(path,slot) : ax==0x1117 ? dos_create(path,slot) : dos_path(ax,path);
    if (first!=5 && first!=3 && first!=2) printf("H13 dos %s %04x %s: %d\n",what,ax,path,first);
    CHECK(first==5 || first==3 || first==2);
    struct h13_state b,e; h13_snapshot(&b);
    for (unsigned k=0;k<3;k++) {
        int r=ax==0x1116 ? dos_open(path,slot) : ax==0x1117 ? dos_create(path,slot) : dos_path(ax,path);
        CHECK(r==first);
    }
    h13_snapshot(&e);
    if (memcmp(&b,&e,sizeof(b))) printf("H13 dos leak %s: pages %u/%u heap %u/%u held %u/%u opens %u/%u descriptions %u/%u pins %u/%u\n",what,
        b.pages,e.pages,b.heap,e.heap,b.held,e.held,b.opens,e.opens,b.descriptions,e.descriptions,b.pins,e.pins);
    CHECK(!memcmp(&b,&e,sizeof(b))); h13_cases++;
}
static void h13_dos(void)
{
    static const char *const dirs[]={"/Windows/Profiles/u00000003/Local Settings","/Windows/Profiles/u00000003/Local Settings/Application Data",
        "/Windows/Profiles/u00000003/Local Settings/Application Data/CiukiOS","/Windows/Profiles/u00000003/Local Settings/Application Data/CiukiOS/Logs",
        "/System/Logs","/System/Services"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    file_put("C:\\Windows\\Profiles\\u00000003\\USER.CRG","hive");
    file_put("C:\\Windows\\Profiles\\u00000003\\Local Settings\\Application Data\\CiukiOS\\Logs\\CRASH1.TXT","crash");
    file_put("C:\\Windows\\USER.CRG","hive"); file_put("C:\\System\\Logs\\BOOT.LOG","boot");
    file_put("C:\\System\\VMM.ELF","kernel"); file_put("C:\\System\\BOOT.CFG","safe=0"); file_put("C:\\System\\Services\\LOGON.ELF","logon");
    char path[192],ls[16];
    { struct fat_entry entry; CHECK(!vfs_stat(host,"C:\\Windows\\Profiles\\u00000003\\Local Settings",&entry));
      snprintf(ls,sizeof(ls),"%s",entry.alias[0] ? entry.alias : entry.name); }
    const struct user *who[]={&BOB,&GUEST}; struct actor *actors[]={&bob,&guest_actor};
    for (unsigned w=0;w<2;w++) {
        logon(actors[w],who[w],2+2*w); c_format(); vm_start(actors[w]);
        /* Alice's profile, hive and crash log. */
        static const char *const leaves[]={"PRIVATE.TXT","USER.CRG"};
        for (unsigned i=0;i<2;i++) {
            snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\%s",a_alias,leaves[i]);
            h13_refused("Alice's file",0x1116,path,1); h13_refused("Alice's file attributes",0x110f,path,0);
            h13_refused("Alice's file delete",0x1113,path,0);
        }
        snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\%s\\APPLIC~1\\CIUKIOS\\LOGS\\CRASH1.TXT",a_alias,ls);
        h13_refused("Alice's crash log",0x1116,path,1);
        snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s",a_alias);
        h13_refused("Alice's profile chdir",0x1105,path,0); h13_refused("Alice's profile rmdir",0x1101,path,0);
        snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\PLANTED.TXT",a_alias);
        h13_refused("plant in Alice's profile",0x1117,path,4);
        snprintf(path,sizeof(path),"D:\\WINDOWS\\PROFILES\\%s\\NEWDIR",a_alias);
        h13_refused("mkdir in Alice's profile",0x1103,path,0);
        /* SYSTEM state. */
        h13_refused("accounts",0x1116,"D:\\SYSTEM\\SECURITY\\ACCOUNTS.DB",1);
        h13_refused("system log",0x1116,"D:\\SYSTEM\\LOGS\\BOOT.LOG",1);
        h13_refused("machine hive",0x1116,"D:\\WINDOWS\\USER.CRG",1);
        h13_refused("security folder",0x1105,"D:\\SYSTEM\\SECURITY",0);
        /* The sealed boot/service set (D: is read-only for every VM). */
        static const char *const sealed[]={"D:\\SYSTEM\\VMM.ELF","D:\\SYSTEM\\BOOT.CFG","D:\\SYSTEM\\SERVICES\\LOGON.ELF"};
        for (unsigned i=0;i<3;i++) {
            h13_refused("sealed delete",0x1113,sealed[i],0); h13_refused("sealed create",0x1117,sealed[i],4);
        }
        h13_refused("sealed plant",0x1117,"D:\\SYSTEM\\SERVICES\\EVIL.ELF",4);
        h13_refused("sealed rmdir",0x1101,"D:\\SYSTEM\\SERVICES",0);
        unsigned denied=IFS.stats.denied; CHECK(denied);
        vm_stop(); CHECK(!dfv2_ifs_held(&IFS));
        logoff(actors[w]);   /* finish(): the subject is back to its own reference */
    }
    printf("H13 dos: %u refusals (Standard user and Guest VMs: another user's profile/hive/crash log, SYSTEM state, sealed boot and service set on D:) without hold/description/pin/page/heap leaks; VM teardown returns every hold and subject reference PASS\n",h13_cases);
}
/* ---- end sec-k08 ---- */
#endif
int main(int argc,char **argv)
{
    CHECK(argc==3); setvbuf(stdout,0,_IONBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    CHECK(!fake_open(&disk[0],argv[1]) && !fake_open(&disk[1],argv[2])); disk[0].undo_enabled=disk[1].undo_enabled=true;
    CHECK(!cache_init(&cache,0)); vfs_init(&vfs);
    for(unsigned i=0;i<2;i++) { CHECK(!fat_mount(&volumes[i],&cache,&disk[i].dev,0,disk[i].dev.capacity,true)); CHECK(!vfs_attach(&vfs,volume_drive(i),&volumes[i])); }
    CHECK(!files_attach(&vfs,&space)); client_thread=make_process(proc_supervisor(),&client); g_current=client_thread->task; g_current->state=T_RUNNING;
    client->cwd=space->root; px_retain(client->cwd); client->cwd_retain=px_retain; client->cwd_release=px_release;
    host=calloc(1,sizeof(*host)); CHECK(host); CHECK(!vfs_table_init(&vfs,host,2));
    const char *dirs[]={"/Windows","/Windows/Profiles",ALICE.root,BOB.root,ADMIN.root,GUEST.root,"/System","/System/Security"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) { int e=make_dir(dirs[i]); CHECK(!e || e==-EEXIST); }
    file_put("C:\\Windows\\Profiles\\u00000003\\private.txt","alice private");
    file_put("C:\\Windows\\Profiles\\u000003ef\\private.txt","bob private");
    file_put("C:\\Windows\\WIN.INI","[windows]");
    file_put("C:\\System\\Security\\accounts.db","verifiers");
    alias_of("C:\\Windows\\Profiles\\u00000003",a_alias); alias_of("C:\\Windows\\Profiles\\u000003ef",b_alias);
    CHECK(strcmp(a_alias,b_alias) && strlen(a_alias)<=12);
    struct owner_store *owners=calloc(1,sizeof(*owners)); CHECK(owners); owners->ready=true; owners->volume=31; owners->sequence=1;
    owners->profile_count=4; record_profile(owners,0,&ALICE); record_profile(owners,1,&BOB); record_profile(owners,2,&ADMIN);
    record_profile(owners,3,&GUEST);
    struct policy_volume *policy=calloc(1,sizeof(*policy)); CHECK(policy);
    CHECK(!policy_init(policy,31,1,true,false,false,owners));
    vfs_lock(&vfs,0); CHECK(!policy_bind_locked(&vfs,2,policy)); vfs_unlock(&vfs);
    /* SESSION_CREATE verifies each profile against the committed bindings
     * through the production k02 verifier (owner.c). */
    CHECK(!owner_security_bind(owners));
    test_identity_capture();
    test_host_views();
    test_removable_view();
    test_gate_swap();
    test_private_stores();
#ifdef CIUKI_POLICY_COVERAGE
    h13_dos();   /* sec-k08 */
#endif
    CHECK(!owner_security_bind(0));
    vfs_lock(&vfs,0); policy_unbind_locked(&vfs,2); vfs_unlock(&vfs);
    vfs_table_destroy(host); free(host); free(policy); free(owners);
    printf("H06 final: checks=%u %s\n",checks,"PASS");
    free(ram); return 0;
}
