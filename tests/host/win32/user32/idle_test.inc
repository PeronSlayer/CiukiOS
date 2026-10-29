/* Production USER + kernel queues, with deterministic scheduler event times. */
static UINT idle_marks, idle_advance, idle_services, idle_noise, idle_probe_wait;
static int32_t idle_backend_call(void *record, DWORD bytes) {
  struct ciuki_w32_header *header=record;
  if(header->operation==W32_WAIT) {
    struct ciuki_w32_wait_args *a=record;
    if(idle_probe_wait){idle_probe_wait=0;CHECK(a->timeout==0);}
    if(idle_noise && (a->reserved&(W32_WAIT_SERVICES|W32_WAIT_SESSION))) {
      CHECK(++idle_services<32); /* turn a regression spin into a bounded failure */
      ui_kernel_pause(1);a->result=W32_WAIT_SERVICE;return 0;
    }
  }
  int32_t status=ui_kernel_call(record,bytes);
  if(header->operation==W32_MESSAGE &&
     ((struct ciuki_w32_message_args *)record)->action==W32_MSG_MARK &&
     !((struct ciuki_w32_message_args *)record)->tid &&
     ((struct ciuki_w32_message_args *)record)->message.wparam==(QS_PAINT|QS_TIMER) &&
     idle_marks && !--idle_marks) {
    ui_kernel_pause(idle_advance);idle_probe_wait=1;
  }
  if(header->operation==W32_WAIT &&
     ((struct ciuki_w32_wait_args *)record)->result==W32_WAIT_SERVICE)
    CHECK(++idle_services<32);
  return status;
}
static void test_idle_timer_race(void) {
  discard();
  u32_backend.call=idle_backend_call;
  const UINT periods[]={10,55,1000};
  for(UINT i=0;i<3;i++) {
    MSG m;uint64_t deadline;DWORD start=ui_kernel_ticks();
    CHECK(SetTimer(windows[0],91,periods[i],0)==91);
    CHECK(!PeekMessageA(&m,0,0,0,PM_REMOVE));
    /* Reach the timer deadline just after the wait publishes an empty queue,
     * in the same wait path used by GetMessage. */
    idle_marks=1;idle_advance=periods[i];
    ui_kernel_idle_begin(UINT32_MAX);
    CHECK(MsgWaitForMultipleObjectsEx(0,0,W32_INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==0);
    CHECK(!idle_marks && ui_kernel_ticks()==start+periods[i]);
    CHECK(ui_kernel_idle_end(&deadline)==0); /* never enroll, especially INFINITE */
    CHECK(PeekMessageA(&m,0,0,0,PM_REMOVE) && m.message==WM_TIMER && m.wParam==91);
    CHECK(KillTimer(windows[0],91));
  }
  u32_backend.call=ui_kernel_call;
  puts("TRACE fx-w32idle-r2 timer publication race: 10/55/1000 ms, no sleep");
}
static struct w32r_reply idle_session_call(UINT action, const DWORD *order, DWORD bytes) {
  struct {struct w32r_request r;char empty[2];uint8_t data[8];} request={
    .r={.version=1,.op=W32R_ENDSESSION,.index=action,.path_bytes=1,.name_bytes=1,
        .flags=action==W32R_SESSION_BEGIN ? W32R_SESSION_HOLD : 0,.data_bytes=bytes}};
  /* Wire tails have no C struct padding. */
  if(bytes)memcpy((char *)&request+sizeof(request.r)+2,order,bytes);
  struct w32r_reply reply;DWORD used;
  CHECK(!w32r_exchange(&request,sizeof(request.r)+2+bytes,&reply,sizeof(reply),&used));
  CHECK(used==sizeof(reply) && !reply.error);
  return reply;
}
static unsigned idle_callbacks, idle_notifications;
static WNDPROC idle_previous_proc;
static void idle_notify_once(uint32_t pid,uint32_t tid) {
  idle_notifications++;ui_kernel_session_notify(pid,tid);
}
static LRESULT W32API idle_session_proc(HWND h,UINT m,WPARAM wp,LPARAM lp) {
  if(m!=0x11 /* WM_QUERYENDSESSION */)return CallWindowProcA(idle_previous_proc,h,m,wp,lp);
  CHECK(++idle_callbacks==1 && u32_thread()->session_active);
  /* A notification published late by the server, after the first latch was
   * consumed, must be drained by this nested loop without recursive dispatch. */
  ui_kernel_session_notify(ui_kernel_pid(),ui_kernel_tid());
  DWORD start=ui_kernel_ticks();uint64_t deadline;
  ui_kernel_idle_begin(UINT32_MAX);
  CHECK(MsgWaitForMultipleObjects(0,0,0,30,QS_ALLINPUT)==W32_WAIT_TIMEOUT);
  CHECK(ui_kernel_idle_end(&deadline)==1 && deadline==start+30);
  CHECK(ui_kernel_ticks()==start+30 && idle_callbacks==1);
  return 1;
}
static void test_idle_nested_session(void) {
  discard();
  uint32_t writer=server.compat_writer_pid;server.compat_writer_pid=ui_kernel_pid();
  CHECK(!wr_tick(&server,ui_kernel_ticks()) && !wr_client_join(&server,ui_kernel_pid()));
  server.session_notify=idle_notify_once;idle_notifications=idle_callbacks=idle_services=0;
  idle_previous_proc=(WNDPROC)(uintptr_t)SetWindowLongA(windows[0],GWL_WNDPROC,(intptr_t)idle_session_proc);
  DWORD order[]={U32_PTR(windows[0]),ui_kernel_pid()};
  idle_session_call(W32R_SESSION_BEGIN,order,sizeof(order));
  CHECK(idle_notifications==1);
  wr_session_wake(&server);wr_session_wake(&server);
  unsigned pending_notifications=idle_notifications;
  u32_backend.call=idle_backend_call;
  u32_session_pump();
  CHECK(idle_callbacks==1 && !u32_thread()->session_active);
  CHECK(pending_notifications==1); /* dispatch and loop tails signal once */
  idle_session_call(W32R_SESSION_RESET,0,0);
  /* Drain any notification of the next window on the other owning thread. */
  ui_kernel_switch(1);u32_session_pump();ui_kernel_switch(0);u32_session_pump();
  SetWindowLongA(windows[0],GWL_WNDPROC,(intptr_t)idle_previous_proc);
  server.compat_writer_pid=writer;server.session_notify=ui_kernel_session_notify;
  u32_backend.call=ui_kernel_call;
  puts("TRACE fx-w32idle-r2 nested session duplicate consumed, callback once, timeout=30 ms");
}
static void test_idle_service_retries(void) {
  discard();u32_backend.call=idle_backend_call;idle_noise=1;idle_services=0;
  DWORD start=ui_kernel_ticks();
  CHECK(MsgWaitForMultipleObjectsEx(0,0,7,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==W32_WAIT_TIMEOUT);
  CHECK(ui_kernel_ticks()==start+7 && idle_services==7);
  idle_services=0;
  CHECK(PostMessageA(windows[0],WM_USER+91,0,0));
  CHECK(MsgWaitForMultipleObjectsEx(0,0,7,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==0);
  CHECK(idle_services==1);
  MSG m;CHECK(PeekMessageA(&m,0,0,0,PM_REMOVE) && m.message==WM_USER+91);
  idle_noise=0;u32_backend.call=ui_kernel_call;
  puts("TRACE fx-w32idle-r2 repeated internal wakes: queue checked, finite timeout=7 ms");
}
static DWORD idle_arrival;
static unsigned idle_kind;
static void idle_deliver(void) {
  if ((int32_t)(ui_kernel_ticks()-idle_arrival)<0) return;
  ui_kernel_schedule(0);
  ui_kernel_switch(1);
  if (idle_kind==0) CHECK(PostMessageA(windows[0],WM_USER+90,7,8));
  else if (idle_kind==1) {
    ui_kernel_schedule(schedule_peer);
    CHECK(SendMessageA(windows[0],WM_USER+7,0,0)==0x103);
    ui_kernel_schedule(0);
  } else if(idle_kind==2) CHECK(InvalidateRect(windows[0],0,0));
  else ui_kernel_session_notify(ui_kernel_pid(),GetWindowThreadProcessId(windows[0],0));
  ui_kernel_switch(0);
}
static void test_idle_wait(void) {
  discard();
  UINT polls=session_polls, requests=exchanges;
  DWORD start=ui_kernel_ticks();uint64_t deadline;
  ui_kernel_idle_begin(UINT32_MAX);
  CHECK(MsgWaitForMultipleObjectsEx(0,0,1000,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==W32_WAIT_TIMEOUT);
  CHECK(ui_kernel_idle_end(&deadline)==1 && deadline==start+1000);
  CHECK(session_polls==polls && exchanges==requests && ui_kernel_ticks()==start+1000);
  printf("TRACE fx-w32idle idle requests=0 elapsed_ms=1000 wait_enrollments=1\n");
  const UINT periods[]={10,55,1000};
  for (UINT i=0;i<3;i++) {
    MSG m;start=ui_kernel_ticks();
    CHECK(SetTimer(windows[0],90,periods[i],0)==90);
    ui_kernel_idle_begin(UINT32_MAX);
    CHECK(GetMessageA(&m,0,0,0) && m.message==WM_TIMER && m.wParam==90);
    CHECK(ui_kernel_idle_end(&deadline)==1 && deadline==start+periods[i]);
    CHECK(ui_kernel_ticks()-start==periods[i]);
    CHECK(KillTimer(windows[0],90));
    printf("TRACE fx-w32idle timer=%u latency_ms=0 wait_enrollments=1\n",periods[i]);
  }
  for (idle_kind=0;idle_kind<3;idle_kind++) {
    MSG m;discard();start=ui_kernel_ticks();idle_arrival=start+77;
    ui_kernel_idle_begin(77);ui_kernel_schedule(idle_deliver);
    CHECK(WaitMessage());
    CHECK(ui_kernel_idle_end(&deadline)==1 && deadline==UINT64_MAX);
    CHECK(ui_kernel_ticks()-idle_arrival<=1);
    if (!idle_kind) CHECK(PeekMessageA(&m,0,0,0,PM_REMOVE) && m.message==WM_USER+90);
    if (idle_kind==2) { CHECK(PeekMessageA(&m,0,0,0,PM_REMOVE) && m.message==WM_PAINT);ValidateRect(windows[0],0); }
    printf("TRACE fx-w32idle %s latency_ms=%u wait_enrollments=1\n",
           idle_kind==0 ? "PostMessage" : idle_kind==1 ? "SendMessage" : "paint",ui_kernel_ticks()-idle_arrival);
  }
  /* WaitMessage needs new input: an already observed due timer must not
   * turn the race fix into a zero-time retry loop. */
  discard();
  CHECK(SetTimer(windows[0],92,10,0)==92);ui_kernel_pause(10);
  CHECK(GetQueueStatus(QS_TIMER)&(QS_TIMER<<16));
  idle_kind=0;idle_arrival=ui_kernel_ticks()+77;
  ui_kernel_idle_begin(77);ui_kernel_schedule(idle_deliver);
  CHECK(WaitMessage());
  CHECK(ui_kernel_idle_end(&deadline)==1 && deadline==UINT64_MAX);
  CHECK(ui_kernel_ticks()==idle_arrival && KillTimer(windows[0],92));
  puts("TRACE fx-w32idle-r2 observed due timer: waits for new input, no spin");
  /* The lost desktop channel must not suppress session notifications or
   * turn its persistent hangup into an application-visible wake/spin. */
  discard();
  BOOL disconnected=u32_desktop.disconnected;u32_desktop.disconnected=1;
  idle_kind=3;idle_arrival=ui_kernel_ticks()+77;polls=session_polls;
  ui_kernel_idle_begin(77);ui_kernel_schedule(idle_deliver);
  CHECK(MsgWaitForMultipleObjectsEx(0,0,1000,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==W32_WAIT_TIMEOUT);
  CHECK(ui_kernel_idle_end(&deadline)==2 && session_polls==polls+1);
  u32_desktop.disconnected=disconnected;
  puts("TRACE fx-w32idle disconnected session wake drained, application timeout preserved");
  discard();
}
