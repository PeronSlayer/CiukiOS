/* Private common spawn admission. SPDX-License-Identifier: GPL-2.0-only */
static int is5_kernel_string(const struct proc_strings *s, uint32_t offset,
                             char *out, unsigned cap) {
    for (unsigned n=0;n<cap && offset<s->used;n++,offset++) {
        out[n]=((const char *)P2V(s->pages[offset/PAGE_SIZE]))[offset%PAGE_SIZE];
        if(!out[n])return 0;
    }
    return -ENAMETOOLONG;
}
static unsigned is5_word(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static bool is5_cleanup_evidence(struct process *p,const char *source) {
    const char *base=source;
    for(const char *q=source;*q;q++)if(*q=='/' || *q=='\\')base=q+1;
    const char name[]="_ISDEL.EXE";
    unsigned i=0;while(base[i] && name[i] && path_upper((uint8_t)base[i])==name[i])i++;
    if(base[i] || name[i])return false;
    struct ciuki_file f={0};
    if(w32_loader_open(p,"C:\\WINDOWS\\_delis43.ini",&f))return false;
    char *text=w32_alloc(4097);bool matched=false;
    if(text && f.bytes && f.bytes<=4096 && !f.read(f.cookie,0,text,f.bytes)) {
        text[f.bytes]=0;
        const char *fields[]={"\nmain=","\nexeostype=1\r\n","\nalt0=","\nalt1="};
        matched=!strncmp(text,"[file",5);
        for(unsigned j=0;matched && j<ARRAY_SIZE(fields);j++) {
            bool found=false;unsigned n=(unsigned)strlen(fields[j]);
            for(unsigned at=0;at+n<=f.bytes;at++)if(!memcmp(text+at,fields[j],n)){found=true;break;}
            if(!found)matched=false;
        }
    }
    if(text)w32_free(text,4097);
    if(f.close)f.close(f.cookie);
    return matched;
}
static int is5_redirect(struct process *p,const struct ciuki_file *file,
                        const struct proc_strings *s,uint64_t mask) {
    uint8_t dos[64],ne[64];
    if(file->bytes<64 || file->read(file->cookie,0,dos,64) || is5_word(dos)!=0x5a4d)return 1;
    uint32_t h=is5_word(dos+60)|(uint32_t)is5_word(dos+62)<<16;
    if(h<64 || h>file->bytes || file->bytes-h<64 || file->read(file->cookie,h,ne,64) ||
       is5_word(ne)!=0x454e || ne[54]!=2 || (is5_word(ne+12)&0x8000))return 1;
    char *buffer=w32_alloc(FS_PATH_BYTES*2+32768);
    if(!buffer)return -ENOMEM;
    char *source=buffer,*sibling=source+FS_PATH_BYTES,*command=sibling+FS_PATH_BYTES;
    int err=is5_kernel_string(s,s->argv[0],source,FS_PATH_BYTES);
    unsigned prefix=0;
    if(!err)for(unsigned i=0;source[i];i++)if(source[i]=='/' || source[i]=='\\')prefix=i+1;
    const char *names[]={"SETUP.INS","_INST32I.EX_","_INST32A.EX_","_INST32M.EX_","_INST32P.EX_"};
    bool script=false,archive=false;
    for(unsigned i=0;!err && i<ARRAY_SIZE(names);i++) {
        if(prefix+strlen(names[i])+1>FS_PATH_BYTES){err=-ENAMETOOLONG;break;}
        memcpy(sibling,source,prefix);memcpy(sibling+prefix,names[i],strlen(names[i])+1);
        struct ciuki_file f={0};
        if(!w32_loader_open(p,sibling,&f)) {
            if(i)archive=true;else script=true;
            if(f.close)f.close(f.cookie);
        }
    }
    if(err || (!(script && archive) && !is5_cleanup_evidence(p,source))){w32_free(buffer,FS_PATH_BYTES*2+32768);return err?err:1;}
    /* The original image path is explicit even when CreateProcess supplied
     * an empty command line or a different argv[0]. */
    unsigned n=(unsigned)strlen(source);
    if(n+4>=32768){err=-ENAMETOOLONG;goto release;}
    command[0]='"';memcpy(command+1,source,n);command[n+1]='"';command[n+2]=' ';command[n+3]=0;
    if(s->argc==2) {
        char *original=w32_alloc(32768);
        if(!original){err=-ENOMEM;goto release;}
        err=is5_kernel_string(s,s->argv[1],original,32768);
        const char *tail=original;
        if(!err) {
            while(*tail==' ' || *tail=='\t')tail++;
            if(*tail=='"'){tail++;while(*tail && *tail!='"')tail++;if(*tail)tail++;}
            else while(*tail && *tail!=' ' && *tail!='\t')tail++;
            while(*tail==' ' || *tail=='\t')tail++;
            if(strlen(tail)>=32768-n-3)err=-ENAMETOOLONG;
            else memcpy(command+n+3,tail,strlen(tail)+1);
        }
        w32_free(original,32768);if(err)goto release;
    }
    struct proc_strings *next=proc_strings_new();
    if(!next){err=-ENOMEM;goto release;}
    const char helper[]="C:\\System\\Win32\\is5boot.exe";
    err=proc_strings_add(next,helper,sizeof(helper),false);
    if(!err)err=proc_strings_add(next,command,(uint32_t)strlen(command)+1,false);
    for(unsigned i=0;!err && i<s->envc;i++) {
        err=is5_kernel_string(s,s->envp[i],command,32768);
        if(!err)err=proc_strings_add(next,command,(uint32_t)strlen(command)+1,true);
    }
    struct ciuki_file replacement={0};
    if(!err)err=w32_loader_open(p,helper,&replacement);
    if(!err)err=w32_spawn(p,&replacement,next,mask);
    if(replacement.close)replacement.close(replacement.cookie);
    proc_strings_free(next);
release:
    w32_free(buffer,FS_PATH_BYTES*2+32768);return err;
}
