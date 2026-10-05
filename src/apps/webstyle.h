/* CiukWeb's bounded CSS style worker ABI. */
#ifndef CIUKIOS_WEBSTYLE_H
#define CIUKIOS_WEBSTYLE_H

#include "app.h"

#define WEBSTYLE_ABI_VERSION 1
#define WEBSTYLE_CHUNK_MAX 1024
#define WEBSTYLE_TAG_MAX 16
#define WEBSTYLE_ID_MAX 40
#define WEBSTYLE_CLASSES_MAX 80
#define WEBSTYLE_INLINE_MAX 256

#define WEBSTYLE_OP_RESET      0
#define WEBSTYLE_OP_FEED       1
#define WEBSTYLE_OP_SHEET_END  2
#define WEBSTYLE_OP_ENTER      3
#define WEBSTYLE_OP_LEAVE      4
#define WEBSTYLE_OP_APPLY      5

#define WEBSTYLE_STATUS_OK       0
#define WEBSTYLE_STATUS_ERROR    1
#define WEBSTYLE_STATUS_LIMIT    2
#define WEBSTYLE_STATUS_BAD_ABI  3

#define WEBSTYLE_PROP_COLOR             0x00000001UL
#define WEBSTYLE_PROP_BACKGROUND_COLOR  0x00000002UL
#define WEBSTYLE_PROP_FONT_WEIGHT       0x00000004UL
#define WEBSTYLE_PROP_FONT_STYLE        0x00000008UL
#define WEBSTYLE_PROP_DISPLAY           0x00000010UL
#define WEBSTYLE_PROP_MARGIN_TOP        0x00000020UL
#define WEBSTYLE_PROP_MARGIN_RIGHT      0x00000040UL
#define WEBSTYLE_PROP_MARGIN_BOTTOM     0x00000080UL
#define WEBSTYLE_PROP_MARGIN_LEFT       0x00000100UL
#define WEBSTYLE_PROP_PADDING_TOP       0x00000200UL
#define WEBSTYLE_PROP_PADDING_RIGHT     0x00000400UL
#define WEBSTYLE_PROP_PADDING_BOTTOM    0x00000800UL
#define WEBSTYLE_PROP_PADDING_LEFT      0x00001000UL
#define WEBSTYLE_PROP_WIDTH             0x00002000UL
#define WEBSTYLE_PROP_HEIGHT            0x00004000UL
#define WEBSTYLE_PROP_TEXT_ALIGN        0x00008000UL
#define WEBSTYLE_PROP_BORDER_WIDTH      0x00010000UL
#define WEBSTYLE_PROP_BORDER_COLOR      0x00020000UL
#define WEBSTYLE_PROP_BORDER_STYLE      0x00040000UL

#define WEBSTYLE_WEIGHT_NORMAL 0
#define WEBSTYLE_WEIGHT_BOLD   1
#define WEBSTYLE_FONT_NORMAL  0
#define WEBSTYLE_FONT_ITALIC  1
#define WEBSTYLE_DISPLAY_INLINE 0
#define WEBSTYLE_DISPLAY_BLOCK  1
#define WEBSTYLE_DISPLAY_NONE   2
#define WEBSTYLE_ALIGN_LEFT   0
#define WEBSTYLE_ALIGN_RIGHT  1
#define WEBSTYLE_ALIGN_CENTER 2
#define WEBSTYLE_ALIGN_JUSTIFY 3
#define WEBSTYLE_BORDER_NONE   0
#define WEBSTYLE_BORDER_SOLID  1
#define WEBSTYLE_BORDER_DASHED 2
#define WEBSTYLE_BORDER_DOTTED 3

#pragma pack(push, 1)
struct webstyle_computed {
    u32 specified;
    u32 declared;          /* declarations selected on this element (not inherited) */
    u32 color;             /* 0x00RRGGBB */
    u32 background_color;  /* 0x00RRGGBB; 0xFFFFFFFF means transparent */
    u32 border_color;
    u8 font_weight;
    u8 font_style;
    u8 display;
    u8 text_align;
    u8 border_style;
    u8 reserved;
    short margin[4];       /* top, right, bottom, left; pixels */
    short padding[4];      /* top, right, bottom, left; pixels */
    short width;           /* -1 means auto or unsupported unit */
    short height;
    short border_width;
};

struct webstyle_request {
    u16 abi_version;
    u16 struct_bytes;
    u16 op;
    u16 status;
    u16 css_bytes;
    u16 flags;             /* ENTER: bit 0 means void element (do not push) */
    char css[WEBSTYLE_CHUNK_MAX];
    char tag[WEBSTYLE_TAG_MAX];
    char id[WEBSTYLE_ID_MAX];
    char classes[WEBSTYLE_CLASSES_MAX]; /* space-separated class names */
    char inline_style[WEBSTYLE_INLINE_MAX];
    struct webstyle_computed style;
};
#pragma pack(pop)

#endif
