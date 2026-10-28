/* H05: actual grant enforcement; hardware keyboard update is a spy. */
#define CIUKI_DESKTOP_HOST_MAIN sec_k04_unused_desktop_main
#include "../proc/desktop_test.c"
static unsigned keyboard_changes;
int input_keyboard_settings(const struct ciuki_keyboard_settings *s)
{ CHECK(s->version==1 && s->size==sizeof(*s));keyboard_changes++;return 0; }
static void admit(struct process *p,uint32_t uid)
{
    p->subject->credential=(struct ciuki_credential){.version=1,.size=64,.uid=uid,.session_id=1,
        .flags=CIUKI_CRED_INTERACTIVE,.primary_gid=uid==501 ? 546 : 545,.group_count=1,
        .groups={uid==501 ? 546 : 545}};
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);
    controller.state=T_RUNNING;g_current=&controller;proc_init();
    struct process *p,*other;struct proc_thread *pt=make_process(proc_supervisor(),&p);
    struct proc_thread *ot=make_process(proc_supervisor(),&other);admit(p,1001);admit(other,1002);
    g_current=pt->task;g_current->state=T_RUNNING;
    int32_t fds[2];CHECK(!grants_install(p,fds));
    struct ciuki_keyboard_settings keyboard={.version=1,.size=sizeof(keyboard)};
    CHECK(!ua_write(p->memory,CIUKI_IMAGE_BASE+PAGE_SIZE,&keyboard,sizeof(keyboard)));
    CHECK(!grant_input_devctl(p,fds[1],INPUT_KEYBOARD_SET,CIUKI_IMAGE_BASE+PAGE_SIZE,sizeof(keyboard)));
    CHECK(keyboard_changes==1);
    admit(p,501);
    CHECK(!grant_input_devctl(p,fds[1],INPUT_KEYBOARD_SET,CIUKI_IMAGE_BASE+PAGE_SIZE,sizeof(keyboard)) && keyboard_changes==2);
    CHECK(grant_input_devctl(p,fds[0],INPUT_KEYBOARD_SET,CIUKI_IMAGE_BASE+PAGE_SIZE,sizeof(keyboard))==-EBADF);
    CHECK(grant_input_devctl(p,fds[1],INPUT_HID_CALIBRATE,0,sizeof(struct ciuki_hid_calibration))==-EOPNOTSUPP);
    struct ciuki_joystick_control joy={.version=1,.size=sizeof(joy),.operation=CIUKI_JOY_SAVE_GAME};
    CHECK(!ua_write(p->memory,CIUKI_IMAGE_BASE+PAGE_SIZE,&joy,sizeof(joy)));
    CHECK(grant_input_devctl(p,fds[1],INPUT_JOY_CONTROL,CIUKI_IMAGE_BASE+PAGE_SIZE,sizeof(joy))==-EOPNOTSUPP);
    /* A copied descriptor still cannot claim another process's input owner. */
    struct proc_object *input=p->fds[fds[1]].object;input->retain(input);other->fds[10]=(struct proc_fd){input,0};
    g_current=ot->task;g_current->state=T_RUNNING;
    CHECK(grant_input_devctl(other,10,INPUT_KEYBOARD_SET,0,sizeof(keyboard))==-EACCES && keyboard_changes==2);
    CHECK(!desktop_close(other,10));
    g_current=pt->task;g_current->state=T_RUNNING;p->subject->live=false;
    CHECK(grant_input_devctl(p,fds[1],INPUT_KEYBOARD_SET,0,sizeof(keyboard))==-EACCES && keyboard_changes==2);
    p->subject->live=true;
    stop_collect(other);stop_collect(p);CHECK(heap_blocks==1 && !pages_used && !desktop_objects.grants);
    free(ram);printf("H05 input: PASS checks=%u, Standard/Guest keyboard, foreign/stale grants, machine persistence denied\n",checks);
    return 0;
}
