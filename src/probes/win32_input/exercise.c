/* Public DirectInput calls; no AdapterInput host injection. LGPL-2.1-or-later */
#include <w32/dinput.h>
#include "../win32_core/fixture.h"
extern int W32API GetKeyboardLayoutList(int,uintptr_t *);
extern UINT W32API MapVirtualKeyExA(UINT,UINT,uintptr_t);
extern int W32API ToAsciiEx(UINT,UINT,const uint8_t *,uint16_t *,UINT,uintptr_t);
static LRESULT W32API procedure(HWND h,UINT m,WPARAM w,LPARAM l)
{return DefWindowProcA(h,m,w,l);}
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_INPUT);
    uintptr_t layouts[2]={0};uint8_t keys[256]={0};
    CHECK(0,GetKeyboardLayoutList(2,layouts)==2 && layouts[0]!=layouts[1]);
    for(unsigned i=0;i<2;i++) {
        uint16_t ch=0;unsigned scan=MapVirtualKeyExA('A',0,layouts[i]);
        CHECK(0,scan==0x1e && ToAsciiEx('A',scan,keys,&ch,0,layouts[i])==1 && ch=='a');
    }
    result->row[0].value[0]=!result->row[0].errors;
    WNDCLASSA c={.lpfnWndProc=procedure,.lpszClassName="F521BInput"};
    CHECK(3,RegisterClassA(&c));
    HWND window=CreateWindowExA(0,c.lpszClassName,"Ciuki input fixture",WS_VISIBLE|WS_CAPTION,
        32,32,160,96,0,0,0,0);
    CHECK(3,window);
    /* Use copied, application-owned GUIDs/data format, avoiding PE data imports. */
    GUID iid={0xbf798030,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0}};
    GUID mouse_id={0x6f1d2b60,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0}};
    IDirectInput *input=0;IDirectInputDevice *mouse=0;
    CHECK(1,DirectInput8Create(0,0x800,&iid,(void **)&input,0)==DI_OK && input);
    if(input)CHECK(1,input->lpVtbl->CreateDevice(input,&mouse_id,&mouse,0)==DI_OK && mouse);
    if(mouse) {
        GUID x={0xa36d02e0,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0}};
        DIOBJECTDATAFORMAT axis={&x,0,DIDFT_RELAXIS|DIDFT_MAKEINSTANCE(0),0};
        DIDATAFORMAT format={sizeof(format),sizeof(axis),DIDF_RELAXIS,4,1,&axis};
        const IDirectInputDeviceVtbl *v=mouse->lpVtbl;
        CHECK(1,v->SetDataFormat(mouse,&format)==DI_OK);
        DIDATAFORMAT bad=format;bad.dwDataSize=1;
        CHECK(1,v->SetDataFormat(mouse,&bad)==DIERR_INVALIDPARAM);
        DIPROPDWORD buffer={{sizeof(buffer),sizeof(buffer.diph),0,DIPH_DEVICE},8};
        CHECK(2,v->SetProperty(mouse,DIPROP_BUFFERSIZE,&buffer.diph)==DI_OK);
        DIPROPDWORD got={{sizeof(got),sizeof(got.diph),0,DIPH_DEVICE},0};
        CHECK(2,v->GetProperty(mouse,DIPROP_BUFFERSIZE,&got.diph)==DI_OK && got.dwData==8);
        CHECK(3,v->SetCooperativeLevel(mouse,window,DISCL_EXCLUSIVE|DISCL_BACKGROUND)==DIERR_UNSUPPORTED);
        CHECK(3,v->SetCooperativeLevel(mouse,window,DISCL_NONEXCLUSIVE|DISCL_BACKGROUND)==DI_OK);
        CHECK(3,v->Acquire(mouse)==DI_OK);
        CHECK(1,v->SetDataFormat(mouse,&format)==DIERR_ACQUIRED);
        LONG state=0;CHECK(3,v->GetDeviceState(mouse,sizeof(state),&state)==DI_OK);
        DIDEVICEOBJECTDATA data[8];DWORD count=8;
        CHECK(2,v->GetDeviceData(mouse,sizeof(*data),data,&count,DIGDD_PEEK)==DI_OK && count<=8);
        CHECK(3,v->Unacquire(mouse)==DI_OK);
        CHECK(3,v->GetDeviceState(mouse,sizeof(state),&state)==DIERR_NOTACQUIRED);
        CHECK(5,v->Release(mouse)==0);
    }
    if(input)CHECK(5,input->lpVtbl->Release(input)==0);
    if(window)CHECK(5,DestroyWindow(window));
    CHECK(5,UnregisterClassA(c.lpszClassName,0));
    result->row[5].value[0]=input && mouse && window && !result->row[5].errors;
    result->row[1].value[0]=mouse && !result->row[1].errors;
    /* A buffer configuration/empty read does not prove raw motion ordering,
     * overflow/peek or foreground exclusive loss/reacquisition. */
    fixture_unsupported(2);fixture_unsupported(3);fixture_unsupported(4);
    fixture_finish();
}
