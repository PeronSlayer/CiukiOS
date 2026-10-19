/* Classic USER controls and dialog/menu ABI. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_CONTROLS_H
#define CIUKI_W32_CONTROLS_H
#include <w32/user32.h>
typedef HANDLE HMENU, HACCEL, HFONT;
typedef intptr_t(W32API *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
#pragma pack(push, 2)
typedef struct { DWORD style, dwExtendedStyle; uint16_t cdit; int16_t x,y,cx,cy; } DLGTEMPLATE;
typedef struct { DWORD style, dwExtendedStyle; int16_t x,y,cx,cy; uint16_t id; } DLGITEMTEMPLATE;
typedef struct { uint8_t fVirt; uint16_t key, cmd; } ACCEL;
#pragma pack(pop)
typedef struct { UINT cbSize, fMask; int nMin,nMax; UINT nPage; int nPos,nTrackPos; } SCROLLINFO;
typedef struct { HWND hwndFrom; uintptr_t idFrom; UINT code; } NMHDR;
typedef struct { UINT CtlType,CtlID,itemID,itemAction,itemState; HWND hwndItem; HDC hDC; RECT rcItem; uintptr_t itemData; } DRAWITEMSTRUCT;
typedef struct { UINT CtlType,CtlID,itemID,itemWidth,itemHeight; uintptr_t itemData; } MEASUREITEMSTRUCT;
typedef struct { UINT cbSize,fMask,fType,fState,wID; HMENU hSubMenu; HANDLE hbmpChecked,hbmpUnchecked; uintptr_t dwItemData; char *dwTypeData; UINT cch; HANDLE hbmpItem; } MENUITEMINFOA;
typedef struct { UINT cbSize; RECT rcExclude; } TPMPARAMS;
#define WM_SETFONT 0x30u
#define WM_GETFONT 0x31u
#define WM_DRAWITEM 0x2bu
#define WM_MEASUREITEM 0x2cu
#define WM_DELETEITEM 0x2du
#define WM_NOTIFY 0x4eu
#define WM_CONTEXTMENU 0x7bu
#define WM_GETDLGCODE 0x87u
#define WM_INITDIALOG 0x110u
#define WM_COMMAND 0x111u
#define WM_HSCROLL 0x114u
#define WM_VSCROLL 0x115u
#define WM_INITMENU 0x116u
#define WM_INITMENUPOPUP 0x117u
#define WM_MENUSELECT 0x11fu
#define WM_ENTERIDLE 0x121u
#define WM_CTLCOLOREDIT 0x133u
#define WM_CTLCOLORLISTBOX 0x134u
#define WM_CTLCOLORBTN 0x135u
#define WM_CTLCOLORDLG 0x136u
#define WM_CTLCOLORSCROLLBAR 0x137u
#define WM_CTLCOLORSTATIC 0x138u
#define WM_FONTCHANGE 0x1du
#define WS_TABSTOP 0x10000u
#define WS_GROUP 0x20000u
#define WS_HSCROLL 0x100000u
#define WS_VSCROLL 0x200000u
#define WS_EX_CONTROLPARENT 0x10000u
#define DS_ABSALIGN 1u
#define DS_SETFONT 0x40u
#define DS_MODALFRAME 0x80u
#define DS_CONTROL 0x400u
#define DS_CENTER 0x800u
#define DS_SHELLFONT 0x48u
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define DM_GETDEFID WM_USER
#define DM_SETDEFID (WM_USER+1)
#define DC_HASDEFID 0x534bu
#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8
#define DLGC_WANTARROWS 1u
#define DLGC_WANTTAB 2u
#define DLGC_WANTALLKEYS 4u
#define DLGC_HASSETSEL 8u
#define DLGC_DEFPUSHBUTTON 0x10u
#define DLGC_UNDEFPUSHBUTTON 0x20u
#define DLGC_RADIOBUTTON 0x40u
#define DLGC_WANTCHARS 0x80u
#define DLGC_STATIC 0x100u
#define DLGC_BUTTON 0x2000u
#define BS_PUSHBUTTON 0u
#define BS_DEFPUSHBUTTON 1u
#define BS_CHECKBOX 2u
#define BS_AUTOCHECKBOX 3u
#define BS_RADIOBUTTON 4u
#define BS_3STATE 5u
#define BS_AUTO3STATE 6u
#define BS_GROUPBOX 7u
#define BS_AUTORADIOBUTTON 9u
#define BS_OWNERDRAW 11u
#define BS_LEFTTEXT 0x20u
#define BS_BITMAP 0x80u
#define BS_ICON 0x40u
#define BM_GETCHECK 0xf0u
#define BM_SETCHECK 0xf1u
#define BM_GETSTATE 0xf2u
#define BM_SETSTATE 0xf3u
#define BM_SETSTYLE 0xf4u
#define BM_CLICK 0xf5u
#define BM_GETIMAGE 0xf6u
#define BM_SETIMAGE 0xf7u
#define BST_UNCHECKED 0u
#define BST_CHECKED 1u
#define BST_INDETERMINATE 2u
#define BST_PUSHED 4u
#define BST_FOCUS 8u
#define BN_CLICKED 0u
#define SS_LEFT 0u
#define SS_CENTER 1u
#define SS_RIGHT 2u
#define SS_ICON 3u
#define SS_BLACKRECT 4u
#define SS_GRAYRECT 5u
#define SS_WHITERECT 6u
#define SS_BLACKFRAME 7u
#define SS_GRAYFRAME 8u
#define SS_WHITEFRAME 9u
#define SS_SIMPLE 11u
#define SS_LEFTNOWORDWRAP 12u
#define SS_BITMAP 14u
#define SS_ETCHEDHORZ 16u
#define SS_ETCHEDVERT 17u
#define SS_ETCHEDFRAME 18u
#define SS_NOPREFIX 0x80u
#define SS_NOTIFY 0x100u
#define SS_CENTERIMAGE 0x200u
#define SS_ENDELLIPSIS 0x4000u
#define SS_PATHELLIPSIS 0x8000u
#define SS_WORDELLIPSIS 0xc000u
#define STM_SETIMAGE 0x172u
#define STM_GETIMAGE 0x173u
#define STM_SETICON 0x170u
#define STM_GETICON 0x171u
#define ES_LEFT 0u
#define ES_CENTER 1u
#define ES_RIGHT 2u
#define ES_MULTILINE 4u
#define ES_UPPERCASE 8u
#define ES_LOWERCASE 16u
#define ES_PASSWORD 32u
#define ES_AUTOVSCROLL 64u
#define ES_AUTOHSCROLL 128u
#define ES_NOHIDESEL 256u
#define ES_READONLY 0x800u
#define ES_WANTRETURN 0x1000u
#define ES_NUMBER 0x2000u
#define EM_GETSEL 0xb0u
#define EM_SETSEL 0xb1u
#define EM_GETRECT 0xb2u
#define EM_SETRECT 0xb3u
#define EM_SETRECTNP 0xb4u
#define EM_SCROLL 0xb5u
#define EM_LINESCROLL 0xb6u
#define EM_SCROLLCARET 0xb7u
#define EM_GETMODIFY 0xb8u
#define EM_SETMODIFY 0xb9u
#define EM_GETLINECOUNT 0xbau
#define EM_LINEINDEX 0xbbu
#define EM_REPLACESEL 0xc2u
#define EM_GETLINE 0xc4u
#define EM_LIMITTEXT 0xc5u
#define EM_CANUNDO 0xc6u
#define EM_UNDO 0xc7u
#define EM_LINEFROMCHAR 0xc9u
#define EM_SETTABSTOPS 0xcbu
#define EM_SETPASSWORDCHAR 0xccu
#define EM_EMPTYUNDOBUFFER 0xcdu
#define EM_GETFIRSTVISIBLELINE 0xceu
#define EM_SETREADONLY 0xcfu
#define EM_GETPASSWORDCHAR 0xd2u
#define EM_GETLIMITTEXT 0xd5u
#define EM_LINELENGTH 0xc1u
#define LBS_NOTIFY 1u
#define LBS_SORT 2u
#define LBS_MULTIPLESEL 8u
#define LBS_OWNERDRAWFIXED 16u
#define LBS_OWNERDRAWVARIABLE 32u
#define LBS_HASSTRINGS 64u
#define LBS_USETABSTOPS 128u
#define LBS_EXTENDEDSEL 0x800u
#define LB_ADDSTRING 0x180u
#define LB_INSERTSTRING 0x181u
#define LB_DELETESTRING 0x182u
#define LB_SELITEMRANGEEX 0x183u
#define LB_RESETCONTENT 0x184u
#define LB_SETSEL 0x185u
#define LB_SETCURSEL 0x186u
#define LB_GETSEL 0x187u
#define LB_GETCURSEL 0x188u
#define LB_GETTEXT 0x189u
#define LB_GETTEXTLEN 0x18au
#define LB_GETCOUNT 0x18bu
#define LB_SELECTSTRING 0x18cu
#define LB_GETTOPINDEX 0x18eu
#define LB_FINDSTRING 0x18fu
#define LB_GETSELCOUNT 0x190u
#define LB_GETSELITEMS 0x191u
#define LB_SETTABSTOPS 0x192u
#define LB_GETHORIZONTALEXTENT 0x193u
#define LB_SETHORIZONTALEXTENT 0x194u
#define LB_SETCOLUMNWIDTH 0x195u
#define LB_SETTOPINDEX 0x197u
#define LB_GETITEMRECT 0x198u
#define LB_GETITEMDATA 0x199u
#define LB_SETITEMDATA 0x19au
#define LB_SELITEMRANGE 0x19bu
#define LB_SETANCHORINDEX 0x19cu
#define LB_GETANCHORINDEX 0x19du
#define LB_SETCARETINDEX 0x19eu
#define LB_GETCARETINDEX 0x19fu
#define LB_SETITEMHEIGHT 0x1a0u
#define LB_GETITEMHEIGHT 0x1a1u
#define LB_FINDSTRINGEXACT 0x1a2u
#define LB_ERR (-1)
#define LB_ERRSPACE (-2)
#define CBS_SIMPLE 1u
#define CBS_DROPDOWN 2u
#define CBS_DROPDOWNLIST 3u
#define CBS_OWNERDRAWFIXED 16u
#define CBS_OWNERDRAWVARIABLE 32u
#define CBS_SORT 0x100u
#define CBS_HASSTRINGS 0x200u
#define CB_GETEDITSEL 0x140u
#define CB_LIMITTEXT 0x141u
#define CB_SETEDITSEL 0x142u
#define CB_ADDSTRING 0x143u
#define CB_DELETESTRING 0x144u
#define CB_GETCOUNT 0x146u
#define CB_GETCURSEL 0x147u
#define CB_GETLBTEXT 0x148u
#define CB_GETLBTEXTLEN 0x149u
#define CB_INSERTSTRING 0x14au
#define CB_RESETCONTENT 0x14bu
#define CB_FINDSTRING 0x14cu
#define CB_SELECTSTRING 0x14du
#define CB_SETCURSEL 0x14eu
#define CB_SHOWDROPDOWN 0x14fu
#define CB_GETITEMDATA 0x150u
#define CB_SETITEMDATA 0x151u
#define CB_GETDROPPEDCONTROLRECT 0x152u
#define CB_SETITEMHEIGHT 0x153u
#define CB_GETITEMHEIGHT 0x154u
#define CB_GETDROPPEDSTATE 0x157u
#define CB_FINDSTRINGEXACT 0x158u
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SIF_RANGE 1u
#define SIF_PAGE 2u
#define SIF_POS 4u
#define SIF_DISABLENOSCROLL 8u
#define SIF_TRACKPOS 16u
#define SIF_ALL 23u
#define SBS_VERT 1u
#define SBM_SETPOS 0xe0u
#define SBM_GETPOS 0xe1u
#define SBM_SETRANGE 0xe2u
#define SBM_GETRANGE 0xe3u
#define SBM_SETSCROLLINFO 0xe9u
#define SBM_GETSCROLLINFO 0xeau
#define EDGE_RAISED 5u
#define EDGE_SUNKEN 10u
#define EDGE_ETCHED 6u
#define EDGE_BUMP 9u
#define BF_LEFT 1u
#define BF_TOP 2u
#define BF_RIGHT 4u
#define BF_BOTTOM 8u
#define BF_RECT 15u
#define BF_ADJUST 0x2000u
#define BF_FLAT 0x4000u
#define BF_MONO 0x8000u
#define DFC_CAPTION 1u
#define DFC_MENU 2u
#define DFC_SCROLL 3u
#define DFC_BUTTON 4u
#define DFCS_BUTTONCHECK 0u
#define DFCS_BUTTONRADIO 4u
#define DFCS_BUTTON3STATE 8u
#define DFCS_BUTTONPUSH 16u
#define DFCS_INACTIVE 0x100u
#define DFCS_PUSHED 0x200u
#define DFCS_CHECKED 0x400u
#define MF_BYCOMMAND 0u
#define MF_BYPOSITION 0x400u
#define MF_STRING 0u
#define MF_BITMAP 4u
#define MF_POPUP 0x10u
#define MF_MENUBARBREAK 0x20u
#define MF_MENUBREAK 0x40u
#define MF_SEPARATOR 0x800u
#define MF_CHECKED 8u
#define MF_UNCHECKED 0u
#define MF_GRAYED 1u
#define MF_DISABLED 2u
#define MF_ENABLED 0u
#define MF_OWNERDRAW 0x100u
#define MF_HILITE 0x80u
#define MIIM_STATE 1u
#define MIIM_ID 2u
#define MIIM_SUBMENU 4u
#define MIIM_CHECKMARKS 8u
#define MIIM_TYPE 16u
#define MIIM_DATA 32u
#define MIIM_STRING 64u
#define MIIM_BITMAP 128u
#define MIIM_FTYPE 256u
#define TPM_RIGHTBUTTON 2u
#define TPM_NONOTIFY 0x80u
#define TPM_RETURNCMD 0x100u
#define MB_OK 0u
#define MB_OKCANCEL 1u
#define MB_ABORTRETRYIGNORE 2u
#define MB_YESNOCANCEL 3u
#define MB_YESNO 4u
#define MB_RETRYCANCEL 5u
#define MB_ICONHAND 0x10u
#define MB_ICONQUESTION 0x20u
#define MB_ICONEXCLAMATION 0x30u
#define MB_ICONASTERISK 0x40u
#define MB_DEFBUTTON2 0x100u
#define MB_DEFBUTTON3 0x200u
#define MB_SYSTEMMODAL 0x1000u
#define MB_TASKMODAL 0x2000u

#define WM_NEXTDLGCTL 0x28u
#define WM_NCHITTEST 0x84u
#define WM_NCMOUSEMOVE 0xa0u
#define WM_NCLBUTTONDOWN 0xa1u
#define WM_NCLBUTTONUP 0xa2u
#define WM_NCLBUTTONDBLCLK 0xa3u
#define WM_NCRBUTTONDOWN 0xa4u
#define WM_NCRBUTTONUP 0xa5u
#define WM_UNINITMENUPOPUP 0x125u
#define WM_CTLCOLORMSGBOX 0x132u
#define WM_ENTERMENULOOP 0x211u
#define WM_EXITMENULOOP 0x212u
#define WM_COMPAREITEM 0x39u
#define WM_VKEYTOITEM 0x2eu
#define WM_CHARTOITEM 0x2fu
#define WM_QUERYDRAGICON 0x37u
#define HTERROR (-2)
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTBORDER 18
#define SC_SIZE 0xf000u
#define SC_MOVE 0xf010u
#define SC_MINIMIZE 0xf020u
#define SC_MAXIMIZE 0xf030u
#define SC_CLOSE 0xf060u
#define SC_MOUSEMENU 0xf090u
#define SC_KEYMENU 0xf100u
#define SC_RESTORE 0xf120u
#define MSGF_DIALOGBOX 0
#define MSGF_MENU 2
#define VK_BACK 8
#define VK_TAB 9
#define VK_RETURN 13
#define VK_SHIFT 16
#define VK_CONTROL 17
#define VK_MENU 18
#define VK_ESCAPE 27
#define VK_SPACE 32
#define VK_PRIOR 33
#define VK_NEXT 34
#define VK_END 35
#define VK_HOME 36
#define VK_LEFT 37
#define VK_UP 38
#define VK_RIGHT 39
#define VK_DOWN 40
#define VK_INSERT 45
#define VK_DELETE 46
#define VK_F4 0x73
#define VK_F10 0x79
#define VK_APPS 0x5d
#define COLOR_SCROLLBAR 0
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNFACE 15
#define COLOR_BTNSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18
#define COLOR_BTNHIGHLIGHT 20
#define COLOR_3DDKSHADOW 21
#define COLOR_3DLIGHT 22
#define COLOR_INFOTEXT 23
#define COLOR_INFOBK 24
#define SB_LINEUP 0
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_BOTTOM 7
#define SB_ENDSCROLL 8
#define ESB_ENABLE_BOTH 0u
#define ESB_DISABLE_LTUP 1u
#define ESB_DISABLE_RTDN 2u
#define ESB_DISABLE_BOTH 3u
#define BDR_RAISEDOUTER 1u
#define BDR_SUNKENOUTER 2u
#define BDR_RAISEDINNER 4u
#define BDR_SUNKENINNER 8u
#define BF_MIDDLE 0x800u
#define DFCS_SCROLLUP 0u
#define DFCS_SCROLLDOWN 1u
#define DFCS_SCROLLLEFT 2u
#define DFCS_SCROLLRIGHT 3u
#define DFCS_SCROLLCOMBOBOX 5u
#define DFCS_MENUARROW 0u
#define DFCS_MENUCHECK 1u
#define DFCS_MENUBULLET 2u
#define DFCS_CAPTIONCLOSE 0u
#define DFCS_CAPTIONMIN 1u
#define DFCS_CAPTIONMAX 2u
#define DFCS_CAPTIONRESTORE 3u
#define DFCS_ADJUSTRECT 0x2000u
#define DFCS_FLAT 0x4000u
#define DFCS_MONO 0x8000u
#define DST_COMPLEX 0u
#define DST_TEXT 1u
#define DST_PREFIXTEXT 2u
#define DST_ICON 3u
#define DST_BITMAP 4u
#define DSS_DISABLED 0x20u
#define MB_ICONMASK 0xf0u
#define MB_DEFMASK 0xf00u
#define MB_MODEMASK 0x3000u
#define MB_TYPEMASK 0xfu
#define MB_APPLMODAL 0u
#define MB_DEFBUTTON4 0x300u
#define MB_HELP 0x4000u
#define MB_NOFOCUS 0x8000u
#define MB_SETFOREGROUND 0x10000u
#define MB_TOPMOST 0x40000u
#define MB_RIGHT 0x80000u
#define IDCLOSE 8
#define IDHELP 9
#define MF_DEFAULT 0x1000u
#define MF_RIGHTJUSTIFY 0x4000u
#define MF_END 0x80u
#define MFT_RADIOCHECK 0x200u
#define MFS_DEFAULT 0x1000u
#define TPM_CENTERALIGN 4u
#define TPM_RIGHTALIGN 8u
#define TPM_VCENTERALIGN 0x10u
#define TPM_BOTTOMALIGN 0x20u
#define FVIRTKEY 1u
#define FNOINVERT 2u
#define FSHIFT 4u
#define FCONTROL 8u
#define FALT 0x10u
#define EN_SETFOCUS 0x100u
#define EN_KILLFOCUS 0x200u
#define EN_CHANGE 0x300u
#define EN_UPDATE 0x400u
#define EN_MAXTEXT 0x501u
#define LBN_SELCHANGE 1u
#define LBN_DBLCLK 2u
#define LBN_SETFOCUS 4u
#define LBN_KILLFOCUS 5u
#define CBN_SELCHANGE 1u
#define CBN_DBLCLK 2u
#define CBN_SETFOCUS 3u
#define CBN_KILLFOCUS 4u
#define CBN_EDITCHANGE 5u
#define CBN_DROPDOWN 7u
#define CBN_CLOSEUP 8u
#define CBN_SELENDOK 9u
#define CBN_SELENDCANCEL 10u
#define STN_CLICKED 0u
#define STN_DBLCLK 1u
#define BN_DBLCLK 5u
#define BS_TYPEMASK 15u
#define BS_LEFT 0x100u
#define BS_RIGHT 0x200u
#define BS_CENTER 0x300u
#define BS_PUSHLIKE 0x1000u
#define BS_MULTILINE 0x2000u
#define BS_NOTIFY 0x4000u
#define BS_FLAT 0x8000u
#define SS_TYPEMASK 0x1fu
#define SS_OWNERDRAW 13u
#define SS_ENHMETAFILE 15u
#define SS_SUNKEN 0x1000u
#define LBS_NOINTEGRALHEIGHT 0x100u
#define LBS_MULTICOLUMN 0x200u
#define LBS_WANTKEYBOARDINPUT 0x400u
#define LBS_DISABLENOSCROLL 0x1000u
#define LBS_NOSEL 0x4000u
#define LB_GETTEXT_ERR LB_ERR
#define CBS_AUTOHSCROLL 0x40u
#define CBS_OEMCONVERT 0x80u
#define CBS_NOINTEGRALHEIGHT 0x400u
#define CB_ERR (-1)
#define CB_ERRSPACE (-2)
#define CB_SETEXTENDEDUI 0x155u
#define CB_GETEXTENDEDUI 0x156u
#define CB_GETTOPINDEX 0x15bu
#define CB_SETTOPINDEX 0x15cu
#define CB_GETHORIZONTALEXTENT 0x15du
#define CB_SETHORIZONTALEXTENT 0x15eu
#define CB_GETDROPPEDWIDTH 0x15fu
#define CB_SETDROPPEDWIDTH 0x160u
#define CB_DIR 0x145u
#define LB_DIR 0x18du
#define LB_ADDFILE 0x196u
#define LB_SETCOLUMNWIDTH_ LB_SETCOLUMNWIDTH
#define LB_ITEMFROMPOINT 0x1a9u
#define EM_GETHANDLE 0xbdu
#define EM_SETHANDLE 0xbcu
#define EM_GETTHUMB 0xbeu
#define EM_FMTLINES 0xc8u
#define EM_SETWORDBREAKPROC 0xd0u
#define EM_SETMARGINS 0xd3u
#define EM_GETMARGINS 0xd4u
#define EM_POSFROMCHAR 0xd6u
#define EM_CHARFROMPOS 0xd7u
#define EM_SETLIMITTEXT EM_LIMITTEXT
#define DS_3DLOOK 4u
#define DS_FIXEDSYS 8u
#define DS_NOFAILCREATE 0x10u
#define DS_LOCALEDIT 0x20u
#define DS_NOIDLEMSG 0x100u
#define DS_SETFOREGROUND 0x200u
#define DS_CENTERMOUSE 0x1000u
#define DS_CONTEXTHELP 0x2000u
#define WS_EX_DLGMODALFRAME 1u
#define WS_EX_NOPARENTNOTIFY 4u
#define DLGC_WANTMESSAGE 4u
#define DLGC_BUTTONMASK 0x2070u
#define HWND_BROADCAST ((HWND)(uintptr_t)0xffff)
#define DWLP_MSGRESULT DWL_MSGRESULT
#define SBS_HORZ 0u
#define SBS_TOPALIGN 2u
#define SBS_LEFTALIGN 2u
#define SBS_SIZEBOX 8u
#define SBM_ENABLE_ARROWS 0xe4u
#define SBM_SETRANGEREDRAW 0xe6u

/* Explicit-length parser for resources and hostile-input host tests. */
HWND W32API w32_create_dialog(HINSTANCE,const void *,DWORD,HWND,DLGPROC,LPARAM);
HWND W32API CreateDialogIndirectParamA(HINSTANCE,const DLGTEMPLATE *,HWND,DLGPROC,LPARAM);
HWND W32API CreateDialogIndirectA(HINSTANCE,const DLGTEMPLATE *,HWND,DLGPROC);
HWND W32API CreateDialogParamA(HINSTANCE,const char *,HWND,DLGPROC,LPARAM);
HWND W32API CreateDialogA(HINSTANCE,const char *,HWND,DLGPROC);
intptr_t W32API DialogBoxIndirectParamA(HINSTANCE,const DLGTEMPLATE *,HWND,DLGPROC,LPARAM);
intptr_t W32API DialogBoxIndirectA(HINSTANCE,const DLGTEMPLATE *,HWND,DLGPROC);
intptr_t W32API DialogBoxParamA(HINSTANCE,const char *,HWND,DLGPROC,LPARAM);
intptr_t W32API DialogBoxA(HINSTANCE,const char *,HWND,DLGPROC);
BOOL W32API EndDialog(HWND,intptr_t);
LRESULT W32API DefDlgProcA(HWND,UINT,WPARAM,LPARAM);
BOOL W32API IsDialogMessageA(HWND,MSG *);
LONG W32API GetDialogBaseUnits(void);
BOOL W32API MapDialogRect(HWND,RECT *);
HWND W32API GetDlgItem(HWND,int);
int W32API GetDlgCtrlID(HWND);
UINT W32API GetDlgItemTextA(HWND,int,char *,int);
BOOL W32API SetDlgItemTextA(HWND,int,const char *);
UINT W32API GetDlgItemInt(HWND,int,BOOL *,BOOL);
BOOL W32API SetDlgItemInt(HWND,int,UINT,BOOL);
LRESULT W32API SendDlgItemMessageA(HWND,int,UINT,WPARAM,LPARAM);
BOOL W32API CheckDlgButton(HWND,int,UINT);
BOOL W32API CheckRadioButton(HWND,int,int,int);
UINT W32API IsDlgButtonChecked(HWND,int);
HWND W32API GetNextDlgTabItem(HWND,HWND,BOOL);
HWND W32API GetNextDlgGroupItem(HWND,HWND,BOOL);
BOOL W32API DrawEdge(HDC,RECT *,UINT,UINT);
BOOL W32API DrawFrameControl(HDC,RECT *,UINT,UINT);
BOOL W32API DrawFocusRect(HDC,const RECT *);
typedef BOOL(W32API *DRAWSTATEPROC)(HDC,LPARAM,WPARAM,int,int);
BOOL W32API DrawStateA(HDC,HBRUSH,DRAWSTATEPROC,LPARAM,WPARAM,int,int,int,int,UINT);
BOOL W32API CreateCaret(HWND,HANDLE,int,int);
BOOL W32API DestroyCaret(void);
BOOL W32API ShowCaret(HWND);
BOOL W32API HideCaret(HWND);
BOOL W32API SetCaretPos(int,int);
BOOL W32API GetCaretPos(POINT *);
BOOL W32API SetCaretBlinkTime(UINT);
UINT W32API GetCaretBlinkTime(void);
int W32API SetScrollInfo(HWND,int,const SCROLLINFO *,BOOL);
BOOL W32API GetScrollInfo(HWND,int,SCROLLINFO *);
int W32API SetScrollPos(HWND,int,int,BOOL);
int W32API GetScrollPos(HWND,int);
BOOL W32API SetScrollRange(HWND,int,int,int,BOOL);
BOOL W32API GetScrollRange(HWND,int,int *,int *);
BOOL W32API ShowScrollBar(HWND,int,BOOL);
BOOL W32API EnableScrollBar(HWND,UINT,UINT);
HMENU W32API CreateMenu(void);
HMENU W32API CreatePopupMenu(void);
BOOL W32API DestroyMenu(HMENU);
BOOL W32API IsMenu(HMENU);
HMENU W32API LoadMenuA(HINSTANCE,const char *);
HMENU W32API LoadMenuIndirectA(const void *);
HMENU W32API w32_load_menu(const void *,DWORD);
BOOL W32API AppendMenuA(HMENU,UINT,uintptr_t,const char *);
BOOL W32API InsertMenuA(HMENU,UINT,UINT,uintptr_t,const char *);
BOOL W32API ModifyMenuA(HMENU,UINT,UINT,uintptr_t,const char *);
BOOL W32API InsertMenuItemA(HMENU,UINT,BOOL,const MENUITEMINFOA *);
BOOL W32API SetMenuItemInfoA(HMENU,UINT,BOOL,const MENUITEMINFOA *);
BOOL W32API GetMenuItemInfoA(HMENU,UINT,BOOL,MENUITEMINFOA *);
BOOL W32API DeleteMenu(HMENU,UINT,UINT);
BOOL W32API RemoveMenu(HMENU,UINT,UINT);
DWORD W32API CheckMenuItem(HMENU,UINT,UINT);
BOOL W32API CheckMenuRadioItem(HMENU,UINT,UINT,UINT,UINT);
UINT W32API EnableMenuItem(HMENU,UINT,UINT);
BOOL W32API SetMenu(HWND,HMENU);
HMENU W32API GetMenu(HWND);
HMENU W32API GetSubMenu(HMENU,int);
int W32API GetMenuItemCount(HMENU);
UINT W32API GetMenuItemID(HMENU,int);
UINT W32API GetMenuState(HMENU,UINT,UINT);
int W32API GetMenuStringA(HMENU,UINT,char *,int,UINT);
LONG W32API GetMenuCheckMarkDimensions(void);
BOOL W32API SetMenuItemBitmaps(HMENU,UINT,UINT,HANDLE,HANDLE);
BOOL W32API DrawMenuBar(HWND);
BOOL W32API TrackPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT *);
BOOL W32API TrackPopupMenuEx(HMENU,UINT,int,int,HWND,const TPMPARAMS *);
HMENU W32API GetSystemMenu(HWND,BOOL);
HACCEL W32API CreateAcceleratorTableA(ACCEL *,int);
HACCEL W32API LoadAcceleratorsA(HINSTANCE,const char *);
int W32API CopyAcceleratorTableA(HACCEL,ACCEL *,int);
BOOL W32API DestroyAcceleratorTable(HACCEL);
int W32API TranslateAcceleratorA(HWND,HACCEL,MSG *);
int W32API MessageBoxA(HWND,const char *,const char *,UINT);
int W32API MessageBoxExA(HWND,const char *,const char *,UINT,uint16_t);
BOOL W32API MessageBeep(UINT);
HBRUSH W32API GetSysColorBrush(int);
int W32API DrawTextA(HDC, const char *, int, RECT *, UINT);
int W32API DrawTextW(HDC, const WCHAR *, int, RECT *, UINT);
int W32API DrawTextExA(HDC, char *, int, RECT *, UINT, void *);
int W32API DrawTextExW(HDC, WCHAR *, int, RECT *, UINT, void *);
LONG W32API TabbedTextOutA(HDC, int, int, const char *, int, int, const int *,
                           int);
DWORD W32API GetTabbedTextExtentA(HDC, const char *, int, int, const int *);
int W32API LoadStringA(HINSTANCE, UINT, char *, int);
int W32API LoadStringW(HINSTANCE, UINT, WCHAR *, int);
BOOL W32API HiliteMenuItem(HWND, HMENU, UINT, UINT);
BOOL W32API SetMenuDefaultItem(HMENU, UINT, UINT);
UINT W32API GetMenuDefaultItem(HMENU, UINT, UINT);
BOOL W32API EndMenu(void);
HANDLE W32API LoadBitmapA(HINSTANCE, const char *);
HANDLE W32API LoadImageA(HINSTANCE, const char *, UINT, int, int, UINT);
#endif
