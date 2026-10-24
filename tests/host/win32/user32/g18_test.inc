/* G18 uses production USER and kernel queues; peer packets are inspected. */
#include <w32/ntdll.h>
extern _Thread_local struct w32_teb *w32_model_teb;
extern BOOL W32API CiukiShellNotify(DWORD,void *,BOOL);
extern BOOL W32API FlashWindow(HWND,BOOL);
static void test_g18_shell(void) {
  struct u32_window *w=u32_window(windows[0]); CHECK(w && w->wire);
  uint8_t a[488]={0};
  w32_put32(a,88); w32_put32(a+4,U32_PTR(w->handle)); w32_put32(a+8,0xfedcba98u);
  w32_put32(a+12,7); w32_put32(a+16,WM_USER+70); w32_put32(a+20,0);
  memcpy(a+24,"Player \x80",9);
  UINT begin=outgoing_count;
  CHECK(CiukiShellNotify(0,a,0)); CHECK(outgoing_count>begin);
  const struct ciuki_message *sent=&outgoing[outgoing_count-1];
  CHECK(sent->data[1]==DESK_V3_TRAY_ICON && sent->fd_count==1);
  CHECK(desk_get32(sent->data+20)==0xfedcba98u && desk_get32(sent->data+28)==WM_USER+70);
  CHECK(!memcmp(sent->data+36,"Player \xe2\x82\xac",10));
  CHECK(!CiukiShellNotify(3,a,0));
  w32_put32(a,87); CHECK(!CiukiShellNotify(0,a,0)); w32_put32(a,88);
  w32_put32(a+12,4); memset(a+24,'x',64); CHECK(!CiukiShellNotify(1,a,0));
  memset(a+24,0,64); a[24]='x';
  CHECK(CiukiShellNotify(1,a,0));
  w32_put32(a,488); w32_put32(a+12,16); memcpy(a+160,"Download complete",18); memcpy(a+420,"Player",7);
  w32_put32(a+416,500); CHECK(CiukiShellNotify(1,a,0));
  sent=&outgoing[outgoing_count-1]; CHECK(sent->data[1]==DESK_V3_TRAY_BALLOON && desk_get32(sent->data+28)==10000);
  uint8_t wide[152]={0}; w32_put32(wide,152); w32_put32(wide+4,U32_PTR(w->handle)); w32_put32(wide+8,6); w32_put32(wide+12,4);
  w32_put16(wide+24,0x03a9); CHECK(CiukiShellNotify(0,wide,1));
  sent=&outgoing[outgoing_count-1]; CHECK(!memcmp(sent->data+36,"\xce\xa9",2));
  w32_put16(wide+24,0x85); CHECK(CiukiShellNotify(1,wide,1));
  CHECK(outgoing[outgoing_count-1].data[36]==' ');
  discard();
  struct desk_packet event={.opcode=DESK_V3_TRAY_EVENT,.window_id=w->wire,.generation=w->generation,.serial=WM_USER+70,.index=0xfedcba98u,.flags=0x205,.x=700,.y=14};
  u32_event(&event);
  MSG message; CHECK(PeekMessageA(&message,w->handle,WM_USER+70,WM_USER+70,1));
  CHECK(message.wParam==0xfedcba98u && message.lParam==0x205);
  POINT tray_point; CHECK(GetCursorPos(&tray_point) && tray_point.x==700 && tray_point.y==14);
  event.generation++; u32_event(&event); CHECK(!PeekMessageA(&message,w->handle,WM_USER+70,WM_USER+70,1));
  BOOL prior=(u32_foreground==w->handle);
  CHECK(FlashWindow(w->handle,1)==prior && w->flash);
  CHECK(FlashWindow(w->handle,1)==!prior && !w->flash);
  CHECK(FlashWindow(w->handle,0)==prior && !w->flash);
  CHECK(outgoing[outgoing_count-1].data[1]==DESK_V3_FLASH);
  SetWindowLongA(w->handle,GWL_EXSTYLE,(LONG)(w->exstyle|0x10u));
  CHECK(w->exstyle&0x10u);
  int accepted=0; for(UINT k=begin;k<outgoing_count;k++) accepted|=outgoing[k].data[1]==DESK_ACCEPT_FILES; CHECK(accepted);
  discard();
  char first_drag[]="X\nF/Documents/song.txt\nF/mnt/d/music/tune.mid\n";
  int first_fd=g18_snapshot(first_drag);
  /* A later publication must not replace the first event's open snapshot. */
  memcpy(first_drag,"Y\n",2);
  int second_fd=g18_snapshot("X\nF/Documents/second.txt\n");
  unsigned closed=g18_snapshot_closes,mapped=g18_snapshot_maps,unmapped=g18_snapshot_unmaps;
  u32_drop_files(w,12,34,first_fd);
  CHECK(g18_snapshot_closes==closed+1);
  CHECK(PeekMessageA(&message,w->handle,0x233,0x233,1) && !message.lParam);
  HANDLE drop=(HANDLE)message.wParam; uint8_t *bytes=GlobalLock(drop);
  CHECK(bytes && w32_u32(bytes)==20 && w32_u32(bytes+4)==12 && w32_u32(bytes+8)==34 && w32_u32(bytes+16)==1);
  const char *expected[]={"C:\\Documents\\song.txt","D:\\music\\tune.mid"}; UINT at=20;
  for (unsigned k=0;k<2;k++) { for (unsigned j=0;expected[k][j];j++,at+=2) CHECK(w32_u16(bytes+at)==(uint8_t)expected[k][j]); CHECK(!w32_u16(bytes+at)); at+=2; }
  CHECK(!w32_u16(bytes+at)); GlobalUnlock(drop); GlobalFree(drop);
  u32_drop_files(w,56,78,second_fd);
  CHECK(g18_snapshot_closes==closed+2);
  CHECK(PeekMessageA(&message,w->handle,0x233,0x233,1));
  drop=(HANDLE)message.wParam;bytes=GlobalLock(drop);CHECK(bytes && w32_u32(bytes+4)==56 && w32_u32(bytes+8)==78);
  const char *second="C:\\Documents\\second.txt";at=20;
  for(unsigned j=0;second[j];j++,at+=2)CHECK(w32_u16(bytes+at)==(uint8_t)second[j]);
  CHECK(!w32_u16(bytes+at) && !w32_u16(bytes+at+2));GlobalUnlock(drop);GlobalFree(drop);
  u32_drop_files(w,0,0,g18_snapshot("X\nF/Documents/good\nFrelative\n"));
  CHECK(!PeekMessageA(&message,w->handle,0x233,0x233,1));
  u32_drop_files(w,0,0,g18_snapshot("X\nF/Documents/bad\xff\n"));
  CHECK(!PeekMessageA(&message,w->handle,0x233,0x233,1));
  CHECK(g18_snapshot_closes==closed+4);
  w->exstyle&=~0x10u;
  u32_drop_files(w,0,0,g18_snapshot("X\nF/Documents/declined.txt\n"));
  CHECK(g18_snapshot_closes==closed+5 && !PeekMessageA(&message,w->handle,0x233,0x233,1));
  w->exstyle|=0x10u;
  /* A valid transport packet transfers its native descriptor to USER. */
  CHECK(in_head==in_tail);
  struct ciuki_message *wire_drop=&incoming[in_tail++%256];
  desk_v3_message(wire_drop,DESK_DROP_FILES,12,0,w->wire,w->generation);
  desk_put32(wire_drop->data+16,1);desk_put32(wire_drop->data+20,91);desk_put32(wire_drop->data+24,92);
  int wire_fd=g18_snapshot("X\nF/Documents/wire.txt\n");wire_drop->fd_count=1;wire_drop->fds[0]=wire_fd;
  struct desk_packet drop_packet;
  CHECK(du3_next(&u32_desktop,&drop_packet)==1 && drop_packet.attachment_fd==wire_fd && g18_snapshot_closes==closed+5);
  u32_event(&drop_packet);
  CHECK(g18_snapshot_closes==closed+6 && PeekMessageA(&message,w->handle,0x233,0x233,1));
  drop=(HANDLE)message.wParam;bytes=GlobalLock(drop);
  CHECK(bytes && w32_u32(bytes+4)==91 && w32_u32(bytes+8)==92);GlobalUnlock(drop);GlobalFree(drop);
  drop_packet.attachment_fd=g18_snapshot("X\nF/Documents/stale.txt\n");drop_packet.generation++;
  u32_event(&drop_packet);
  CHECK(g18_snapshot_closes==closed+7 && !PeekMessageA(&message,w->handle,0x233,0x233,1));
  /* Malformed packets do not leak any of their received attachments. */
  wire_drop=&incoming[in_tail++%256];
  desk_v3_message(wire_drop,DESK_DROP_FILES,12,0,w->wire,w->generation);
  desk_put32(wire_drop->data+16,2);wire_drop->fd_count=2;
  wire_drop->fds[0]=g18_snapshot("X\nF/Documents/one.txt\n");
  wire_drop->fds[1]=g18_snapshot("X\nF/Documents/two.txt\n");
  struct du3_connection isolated=u32_desktop;
  CHECK(du3_next(&isolated,&drop_packet)<0 && isolated.disconnected && !u32_desktop.disconnected && g18_snapshot_closes==closed+9);
  int bad_length=g18_snapshot("X\nF/Documents/bad-length.txt\n");
  g18_snapshot_length(bad_length,4093);u32_drop_files(w,0,0,bad_length);
  CHECK(g18_snapshot_closes==closed+10 && !PeekMessageA(&message,w->handle,0x233,0x233,1));
  bad_length=g18_snapshot("X\nF/Documents/overflow.txt\n");
  g18_snapshot_length(bad_length,0xffffffffu);u32_drop_files(w,0,0,bad_length);
  CHECK(g18_snapshot_closes==closed+11 && !PeekMessageA(&message,w->handle,0x233,0x233,1));
  const char *bad_paths[]={"X\nF/Documents/../secret\n","X\nF/Documents/./secret\n",
    "X\nF/Documents//secret\n","X\nF/Documents/secret/\n","X\nF/Documents/a\\b\n","X\nF/Documents/a:b\n"};
  for(unsigned k=0;k<sizeof(bad_paths)/sizeof(*bad_paths);k++) {
    u32_drop_files(w,0,0,g18_snapshot(bad_paths[k]));
    CHECK(!PeekMessageA(&message,w->handle,0x233,0x233,1));
  }
  u32_drop_files(w,0,0,g18_snapshot("X\nF/Documents/.name.txt\nD/mnt/d\nF/Documents/nested/path.txt\nD/\n"));
  CHECK(PeekMessageA(&message,w->handle,0x233,0x233,1));drop=(HANDLE)message.wParam;bytes=GlobalLock(drop);
  const char *identity[]={"C:\\Documents\\.name.txt","D:\\","C:\\Documents\\nested\\path.txt","C:\\"};at=20;
  for(unsigned k=0;k<4;k++) {
    for(unsigned j=0;identity[k][j];j++,at+=2)CHECK(w32_u16(bytes+at)==(uint8_t)identity[k][j]);
    CHECK(!w32_u16(bytes+at));at+=2;
  }
  CHECK(!w32_u16(bytes+at));GlobalUnlock(drop);GlobalFree(drop);
  CHECK(g18_snapshot_closes==closed+18 && g18_snapshot_maps==mapped+14 && g18_snapshot_unmaps==unmapped+14);
  /* Startup flag overrides the first call once; SW_SHOWDEFAULT rereads it. */
  struct w32_process_parameters params={.WindowFlags=1,.ShowWindowFlags=7};
  struct w32_peb peb={.ProcessParameters=&params}; struct w32_teb teb={.ProcessEnvironmentBlock=&peb};
  w32_model_teb=&teb; u32_startup_reset();
  CHECK(u32_startup_show(1)==7 && u32_startup_show(3)==3 && u32_startup_show(10)==7);
  params.ShowWindowFlags=3; u32_startup_reset(); CHECK(u32_startup_show(1)==3);
  w32_model_teb=NULL; u32_startup_reset();
  CHECK(CiukiShellNotify(2,a,0));
  discard();
  puts("PASS G18 USER: ANSI/Unicode tray packets, flags, callbacks/generation, balloons, flash return, startup show, native file list to HDROP and atomic malformed refusal");
}
