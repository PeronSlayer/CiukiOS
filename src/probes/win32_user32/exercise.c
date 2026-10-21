/* Real USER sends, queued input, paint validation and window teardown. LGPL-2.1-or-later */
#include <w32/user32.h>
#include "../win32_core/fixture.h"
static unsigned sends,keys,paints;
static LRESULT W32API procedure(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_USER+17){sends++;return (LRESULT)(w+l+37);}
    if(m==WM_KEYDOWN){keys++;return 0;}
    if(m==WM_PAINT) {
        PAINTSTRUCT p;
        HDC dc=BeginPaint(h,&p);
        CHECK(2,dc && p.rcPaint.right>p.rcPaint.left && p.rcPaint.bottom>p.rcPaint.top);
        CHECK(2,EndPaint(h,&p));paints++;return 0;
    }
    return DefWindowProcA(h,m,w,l);
}
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_USER32);
    WNDCLASSA c={.lpfnWndProc=procedure,.lpszClassName="F521BUser"};
    CHECK(0,RegisterClassA(&c));
    HWND h=CreateWindowExA(0,c.lpszClassName,"Ciuki USER fixture",WS_VISIBLE|WS_CAPTION,
        32,32,160,96,0,0,0,0);
    CHECK(0,h);
    CHECK(0,SendMessageA(h,WM_USER+17,10,20)==67 && sends==1);
    result->row[0].value[0]=sends==1 && !result->row[0].errors;
    /* A queued key exercises USER queue classification and dispatch. It
     * does not claim PS/2 or raw DirectInput motion coverage. */
    CHECK(1,PostMessageA(h,WM_KEYDOWN,'C',1));
    MSG m;unsigned found=0;
    for(unsigned i=0;i<1000 && !found;i++) {
        while(PeekMessageA(&m,h,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE)) {
            CHECK(1,m.hwnd==h && m.wParam=='C');DispatchMessageA(&m);found++;
        }
        if(!found)Sleep(1);
    }
    CHECK(1,found==1 && keys==1);result->row[1].value[0]=found==1 && keys==1;
    RECT r={0,0,16,16};CHECK(2,InvalidateRect(h,&r,1));CHECK(2,UpdateWindow(h));
    CHECK(2,paints>0);result->row[2].value[0]=paints>0 && !result->row[2].errors;
    CHECK(3,DestroyWindow(h));CHECK(3,!IsWindow(h));
    CHECK(3,UnregisterClassA(c.lpszClassName,0));
    result->row[3].value[0]=!result->row[3].errors;
    fixture_finish();
}
