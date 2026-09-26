#ifndef CIUKI_WOLF_SURFACE_API
#define CIUKI_WOLF_SURFACE_API
/* Narrow source compatibility for Wolf4SDL. Not a general SDL implementation.
   Software indexed surfaces + Ciuki graphics bridge; no audio or joystick. */
#include <stdint.h>
#include <stddef.h>
typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32;
typedef int16_t Sint16; typedef int32_t Sint32;
typedef int SDL_Keymod;
struct SDL_Color { Uint8 r,g,b,a; };
struct SDL_Palette { int ncolors; SDL_Color *colors; };
struct SDL_PixelFormat { int BitsPerPixel,BytesPerPixel; SDL_Palette *palette; };
struct SDL_Surface { int w,h,pitch; void *pixels; SDL_PixelFormat *format; };
struct SDL_Rect { int x,y,w,h; };
struct SDL_Window { int w,h; };
struct SDL_Renderer { int unused; }; struct SDL_Texture { int w,h; };
struct SDL_Joystick { int unused; };
struct SDL_Keysym { int scancode,sym; Uint16 mod; };
struct SDL_WindowEvent { Uint32 type,windowID; Uint8 event; int data1,data2; };
struct SDL_KeyboardEvent { Uint32 type; SDL_Keysym keysym; };
struct SDL_MouseButtonEvent { Uint32 type; Uint8 button; };
struct SDL_MouseWheelEvent { Uint32 type; int x,y; };
union SDL_Event { Uint32 type; SDL_WindowEvent window; SDL_KeyboardEvent key;
                 SDL_MouseButtonEvent button; SDL_MouseWheelEvent wheel; };
#define SDL_TRUE 1
#define SDL_FALSE 0
#define SDL_ENABLE 1
#define SDL_INIT_VIDEO 1
#define SDL_INIT_AUDIO 2
#define SDL_INIT_JOYSTICK 4
#define SDL_WINDOWPOS_CENTERED 0
#define SDL_WINDOW_ALLOW_HIGHDPI 1
#define SDL_WINDOW_RESIZABLE 2
#define SDL_WINDOW_FULLSCREEN_DESKTOP 4
#define SDL_RENDERER_PRESENTVSYNC 1
#define SDL_RENDERER_SOFTWARE 2
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_HINT_RENDER_SCALE_QUALITY "scale"
#define SDL_MUSTLOCK(s) 0
#define SDL_QUIT 1
#define SDL_KEYDOWN 2
#define SDL_KEYUP 3
#define SDL_WINDOWEVENT 4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP 6
#define SDL_MOUSEWHEEL 7
#define SDL_WINDOWEVENT_RESIZED 1
#define SDL_WINDOWEVENT_MINIMIZED 2
#define SDL_WINDOWEVENT_MAXIMIZED 3
#define SDL_WINDOWEVENT_RESTORED 4
#define SDL_WINDOWEVENT_FOCUS_GAINED 5
#define SDL_WINDOWEVENT_FOCUS_LOST 6
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON(n) (1u<<((n)-1))
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8
#define KMOD_LSHIFT 1
#define KMOD_RSHIFT 2
#define KMOD_SHIFT 3
#define KMOD_LCTRL 4
#define KMOD_RCTRL 8
#define KMOD_LALT 16
#define KMOD_RALT 32
#define KMOD_CAPS 64
#define KMOD_NUM 128
#define KMOD_LGUI 256
#define KMOD_RGUI 512
#define SDL_SCANCODE_RETURN 28
#define SDL_SCANCODE_KP_ENTER 284
#include "wolf_keys.h"
int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
SDL_Window *SDL_CreateWindow(const char *,int,int,int,int,Uint32);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *,int,Uint32);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *,Uint32,int,int,int);
void SDL_SetWindowMinimumSize(SDL_Window *,int,int);
void SDL_SetWindowSize(SDL_Window *,int,int);
int SDL_SetWindowFullscreen(SDL_Window *,Uint32);
void SDL_GetWindowSize(SDL_Window *,int *,int *);
Uint32 SDL_GetWindowID(SDL_Window *);
Uint32 SDL_GetWindowPixelFormat(SDL_Window *);
int SDL_RenderSetLogicalSize(SDL_Renderer *,int,int);
int SDL_SetHint(const char *,const char *);
int SDL_PixelFormatEnumToMasks(Uint32,int *,Uint32 *,Uint32 *,Uint32 *,Uint32 *);
SDL_Surface *SDL_CreateRGBSurface(Uint32,int,int,int,Uint32,Uint32,Uint32,Uint32);
void SDL_FreeSurface(SDL_Surface *);
int SDL_LockSurface(SDL_Surface *);
void SDL_UnlockSurface(SDL_Surface *);
SDL_Palette *SDL_AllocPalette(int);
int SDL_SetPaletteColors(SDL_Palette *,const SDL_Color *,int,int);
int SDL_SetSurfacePalette(SDL_Surface *,SDL_Palette *);
void SDL_FreePalette(SDL_Palette *);
int SDL_BlitSurface(SDL_Surface *,const SDL_Rect *,SDL_Surface *,SDL_Rect *);
int SDL_FillRect(SDL_Surface *,const SDL_Rect *,Uint32);
Uint32 SDL_MapRGB(SDL_PixelFormat *,Uint8,Uint8,Uint8);
int SDL_UpdateTexture(SDL_Texture *,const SDL_Rect *,const void *,int);
int SDL_RenderClear(SDL_Renderer *);
int SDL_RenderCopy(SDL_Renderer *,SDL_Texture *,const SDL_Rect *,const SDL_Rect *);
void SDL_RenderPresent(SDL_Renderer *);
int SDL_SaveBMP(SDL_Surface *,const char *);
int SDL_PollEvent(SDL_Event *);
int SDL_WaitEvent(SDL_Event *);
int SDL_PushEvent(SDL_Event *);
SDL_Keymod SDL_GetModState(void);
int SDL_SetRelativeMouseMode(int);
Uint32 SDL_GetRelativeMouseState(int *,int *);
void SDL_WarpMouseInWindow(SDL_Window *,int,int);
int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int);
void SDL_JoystickClose(SDL_Joystick *);
int SDL_JoystickEventState(int);
int SDL_JoystickNumButtons(SDL_Joystick *);
int SDL_JoystickNumHats(SDL_Joystick *);
int SDL_JoystickGetAxis(SDL_Joystick *,int);
int SDL_JoystickGetButton(SDL_Joystick *,int);
int SDL_JoystickGetHat(SDL_Joystick *,int);
void SDL_JoystickUpdate(void);
#endif
