/* Production USER + kernel queues, with deterministic scheduler event times. */
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
