/* SVGA3D (legacy VGPU9) protocol subset used by the CiukiOS backend.
 * Numeric values only; no upstream code. Every value names its pinned source
 * line: "mesa:" = Mesa 21.3.9 src/gallium/drivers/svga/include/, "linux:" =
 * Linux v6.1 drivers/gpu/drm/vmwgfx/device_include/. tests/host/
 * test_vmwaresvga3d.py re-checks each citation against the pinned files.
 * See docs/design/f7-family-vmware-svga3d.md. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VMWARESVGA3D_PROTOCOL_H
#define CIUKI_VMWARESVGA3D_PROTOCOL_H

/* Command ids (SVGA3dCmdHeader {id,size} precedes each body). */
#define VMSVGA3D_CMD_SURFACE_DEFINE 1040u   /* mesa:svga3d_cmd.h:61 linux:svga3d_cmd.h:46 SVGA_3D_CMD_SURFACE_DEFINE */
#define VMSVGA3D_CMD_SURFACE_DESTROY 1041u  /* mesa:svga3d_cmd.h:62 linux:svga3d_cmd.h:47 SVGA_3D_CMD_SURFACE_DESTROY */
#define VMSVGA3D_CMD_SURFACE_COPY 1042u     /* mesa:svga3d_cmd.h:63 linux:svga3d_cmd.h:48 SVGA_3D_CMD_SURFACE_COPY */
#define VMSVGA3D_CMD_SURFACE_DMA 1044u      /* mesa:svga3d_cmd.h:65 linux:svga3d_cmd.h:50 SVGA_3D_CMD_SURFACE_DMA */
#define VMSVGA3D_CMD_CONTEXT_DEFINE 1045u   /* mesa:svga3d_cmd.h:66 linux:svga3d_cmd.h:51 SVGA_3D_CMD_CONTEXT_DEFINE */
#define VMSVGA3D_CMD_CONTEXT_DESTROY 1046u  /* mesa:svga3d_cmd.h:67 linux:svga3d_cmd.h:52 SVGA_3D_CMD_CONTEXT_DESTROY */
#define VMSVGA3D_CMD_SETTRANSFORM 1047u     /* mesa:svga3d_cmd.h:68 linux:svga3d_cmd.h:53 SVGA_3D_CMD_SETTRANSFORM */
#define VMSVGA3D_CMD_SETZRANGE 1048u        /* mesa:svga3d_cmd.h:69 linux:svga3d_cmd.h:54 SVGA_3D_CMD_SETZRANGE */
#define VMSVGA3D_CMD_SETRENDERSTATE 1049u   /* mesa:svga3d_cmd.h:70 linux:svga3d_cmd.h:55 SVGA_3D_CMD_SETRENDERSTATE */
#define VMSVGA3D_CMD_SETRENDERTARGET 1050u  /* mesa:svga3d_cmd.h:71 linux:svga3d_cmd.h:56 SVGA_3D_CMD_SETRENDERTARGET */
#define VMSVGA3D_CMD_SETTEXTURESTATE 1051u  /* mesa:svga3d_cmd.h:72 linux:svga3d_cmd.h:57 SVGA_3D_CMD_SETTEXTURESTATE */
#define VMSVGA3D_CMD_SETVIEWPORT 1055u      /* mesa:svga3d_cmd.h:76 linux:svga3d_cmd.h:61 SVGA_3D_CMD_SETVIEWPORT */
#define VMSVGA3D_CMD_CLEAR 1057u            /* mesa:svga3d_cmd.h:78 linux:svga3d_cmd.h:63 SVGA_3D_CMD_CLEAR */
#define VMSVGA3D_CMD_DRAW_PRIMITIVES 1063u  /* mesa:svga3d_cmd.h:84 linux:svga3d_cmd.h:69 SVGA_3D_CMD_DRAW_PRIMITIVES */

/* Surface formats, flags and transfer direction. */
#define VMSVGA3D_INVALID_ID 0xffffffffu     /* mesa:svga3d_types.h:80 linux:svga3d_types.h:40 SVGA3D_INVALID_ID */
#define VMSVGA3D_A8R8G8B8 2u                /* mesa:svga3d_types.h:169 linux:svga3d_types.h:149 SVGA3D_A8R8G8B8 */
#define VMSVGA3D_Z_D16 8u                   /* mesa:svga3d_types.h:177 linux:svga3d_types.h:157 SVGA3D_Z_D16 */
#define VMSVGA3D_BUFFER 37u                 /* mesa:svga3d_types.h:219 linux:svga3d_types.h:196 SVGA3D_BUFFER */
#define VMSVGA3D_HINT_VERTEXBUFFER 0x10u    /* mesa:svga3d_types.h:359 linux:svga3d_types.h:335 SVGA3D_SURFACE_HINT_VERTEXBUFFER */
#define VMSVGA3D_HINT_TEXTURE 0x20u         /* mesa:svga3d_types.h:360 linux:svga3d_types.h:336 SVGA3D_SURFACE_HINT_TEXTURE */
#define VMSVGA3D_HINT_RENDERTARGET 0x40u    /* mesa:svga3d_types.h:361 linux:svga3d_types.h:337 SVGA3D_SURFACE_HINT_RENDERTARGET */
#define VMSVGA3D_HINT_DEPTHSTENCIL 0x80u    /* mesa:svga3d_types.h:362 linux:svga3d_types.h:338 SVGA3D_SURFACE_HINT_DEPTHSTENCIL */
#define VMSVGA3D_MAX_SURFACE_FACES 6u       /* mesa:svga3d_limits.h:82 linux:svga3d_limits.h:70 SVGA3D_MAX_SURFACE_FACES */
#define VMSVGA3D_WRITE_HOST_VRAM 1u         /* mesa:svga3d_types.h:1749 linux:svga3d_types.h:1409 SVGA3D_WRITE_HOST_VRAM */
#define VMSVGA3D_READ_HOST_VRAM 2u          /* mesa:svga3d_types.h:1750 linux:svga3d_types.h:1410 SVGA3D_READ_HOST_VRAM */

/* Render states (SVGA3dRenderState {state,value}). */
#define VMSVGA3D_RS_ZENABLE 1u              /* mesa:svga3d_types.h:829 linux:svga3d_types.h:618 SVGA3D_RS_ZENABLE */
#define VMSVGA3D_RS_ZWRITEENABLE 2u         /* mesa:svga3d_types.h:830 linux:svga3d_types.h:619 SVGA3D_RS_ZWRITEENABLE */
#define VMSVGA3D_RS_ALPHATESTENABLE 3u      /* mesa:svga3d_types.h:831 linux:svga3d_types.h:620 SVGA3D_RS_ALPHATESTENABLE */
#define VMSVGA3D_RS_DITHERENABLE 4u         /* mesa:svga3d_types.h:832 linux:svga3d_types.h:621 SVGA3D_RS_DITHERENABLE */
#define VMSVGA3D_RS_BLENDENABLE 5u          /* mesa:svga3d_types.h:833 linux:svga3d_types.h:622 SVGA3D_RS_BLENDENABLE */
#define VMSVGA3D_RS_FOGENABLE 6u            /* mesa:svga3d_types.h:834 linux:svga3d_types.h:623 SVGA3D_RS_FOGENABLE */
#define VMSVGA3D_RS_SPECULARENABLE 7u       /* mesa:svga3d_types.h:835 linux:svga3d_types.h:624 SVGA3D_RS_SPECULARENABLE */
#define VMSVGA3D_RS_STENCILENABLE 8u        /* mesa:svga3d_types.h:836 linux:svga3d_types.h:625 SVGA3D_RS_STENCILENABLE */
#define VMSVGA3D_RS_LIGHTINGENABLE 9u       /* mesa:svga3d_types.h:837 linux:svga3d_types.h:626 SVGA3D_RS_LIGHTINGENABLE */
#define VMSVGA3D_RS_SHADEMODE 30u           /* mesa:svga3d_types.h:858 linux:svga3d_types.h:647 SVGA3D_RS_SHADEMODE */
#define VMSVGA3D_RS_SRCBLEND 32u            /* mesa:svga3d_types.h:860 linux:svga3d_types.h:649 SVGA3D_RS_SRCBLEND */
#define VMSVGA3D_RS_DSTBLEND 33u            /* mesa:svga3d_types.h:861 linux:svga3d_types.h:650 SVGA3D_RS_DSTBLEND */
#define VMSVGA3D_RS_BLENDEQUATION 34u       /* mesa:svga3d_types.h:862 linux:svga3d_types.h:651 SVGA3D_RS_BLENDEQUATION */
#define VMSVGA3D_RS_CULLMODE 35u            /* mesa:svga3d_types.h:863 linux:svga3d_types.h:652 SVGA3D_RS_CULLMODE */
#define VMSVGA3D_RS_ZFUNC 36u               /* mesa:svga3d_types.h:864 linux:svga3d_types.h:653 SVGA3D_RS_ZFUNC */
#define VMSVGA3D_RS_ALPHAFUNC 37u           /* mesa:svga3d_types.h:865 linux:svga3d_types.h:654 SVGA3D_RS_ALPHAFUNC */
#define VMSVGA3D_RS_ALPHAREF 42u            /* mesa:svga3d_types.h:870 linux:svga3d_types.h:659 SVGA3D_RS_ALPHAREF (float 0..1) */
#define VMSVGA3D_RS_COLORWRITEENABLE 47u    /* mesa:svga3d_types.h:875 linux:svga3d_types.h:664 SVGA3D_RS_COLORWRITEENABLE */
#define VMSVGA3D_RS_SCISSORTESTENABLE 55u   /* mesa:svga3d_types.h:883 linux:svga3d_types.h:672 SVGA3D_RS_SCISSORTESTENABLE */
#define VMSVGA3D_RS_SEPARATEALPHABLENDENABLE 93u /* mesa:svga3d_types.h:932 linux:svga3d_types.h:711 SVGA3D_RS_SEPARATEALPHABLENDENABLE */

/* Texture stage states (SVGA3dTextureState {stage,name,value}). */
#define VMSVGA3D_TS_BIND_TEXTURE 1u         /* mesa:svga3d_types.h:1216 linux:svga3d_types.h:961 SVGA3D_TS_BIND_TEXTURE */
#define VMSVGA3D_TS_COLOROP 2u              /* mesa:svga3d_types.h:1217 linux:svga3d_types.h:962 SVGA3D_TS_COLOROP */
#define VMSVGA3D_TS_COLORARG1 3u            /* mesa:svga3d_types.h:1218 linux:svga3d_types.h:963 SVGA3D_TS_COLORARG1 */
#define VMSVGA3D_TS_COLORARG2 4u            /* mesa:svga3d_types.h:1219 linux:svga3d_types.h:964 SVGA3D_TS_COLORARG2 */
#define VMSVGA3D_TS_ALPHAOP 5u              /* mesa:svga3d_types.h:1220 linux:svga3d_types.h:965 SVGA3D_TS_ALPHAOP */
#define VMSVGA3D_TS_ALPHAARG1 6u            /* mesa:svga3d_types.h:1221 linux:svga3d_types.h:966 SVGA3D_TS_ALPHAARG1 */
#define VMSVGA3D_TS_ALPHAARG2 7u            /* mesa:svga3d_types.h:1222 linux:svga3d_types.h:967 SVGA3D_TS_ALPHAARG2 */
#define VMSVGA3D_TS_ADDRESSU 8u             /* mesa:svga3d_types.h:1223 linux:svga3d_types.h:968 SVGA3D_TS_ADDRESSU */
#define VMSVGA3D_TS_ADDRESSV 9u             /* mesa:svga3d_types.h:1224 linux:svga3d_types.h:969 SVGA3D_TS_ADDRESSV */
#define VMSVGA3D_TS_MIPFILTER 10u           /* mesa:svga3d_types.h:1225 linux:svga3d_types.h:970 SVGA3D_TS_MIPFILTER */
#define VMSVGA3D_TS_MAGFILTER 11u           /* mesa:svga3d_types.h:1226 linux:svga3d_types.h:971 SVGA3D_TS_MAGFILTER */
#define VMSVGA3D_TS_MINFILTER 12u           /* mesa:svga3d_types.h:1227 linux:svga3d_types.h:972 SVGA3D_TS_MINFILTER */
#define VMSVGA3D_TS_TEXCOORDINDEX 14u       /* mesa:svga3d_types.h:1229 linux:svga3d_types.h:974 SVGA3D_TS_TEXCOORDINDEX */

/* Texture combiners and arguments (D3D9 fixed-function semantics). */
#define VMSVGA3D_TC_DISABLE 1u              /* mesa:svga3d_types.h:1264 linux:svga3d_types.h:1000 SVGA3D_TC_DISABLE */
#define VMSVGA3D_TC_SELECTARG1 2u           /* mesa:svga3d_types.h:1265 linux:svga3d_types.h:1001 SVGA3D_TC_SELECTARG1 */
#define VMSVGA3D_TC_MODULATE 4u             /* mesa:svga3d_types.h:1267 linux:svga3d_types.h:1003 SVGA3D_TC_MODULATE */
#define VMSVGA3D_TC_ADD 5u                  /* mesa:svga3d_types.h:1268 linux:svga3d_types.h:1004 SVGA3D_TC_ADD */
#define VMSVGA3D_TC_BLENDTEXTUREALPHA 8u    /* mesa:svga3d_types.h:1271 linux:svga3d_types.h:1007 SVGA3D_TC_BLENDTEXTUREALPHA */
#define VMSVGA3D_TC_CAP_BIT(op) (1u << ((op) - 1u)) /* mesa:svga3d_types.h:1293 SVGA3D_TC_CAP_BIT */
#define VMSVGA3D_TA_PREVIOUS 2u             /* mesa:svga3d_types.h:1348 linux:svga3d_types.h:1078 SVGA3D_TA_PREVIOUS */
#define VMSVGA3D_TA_DIFFUSE 3u              /* mesa:svga3d_types.h:1349 linux:svga3d_types.h:1079 SVGA3D_TA_DIFFUSE */
#define VMSVGA3D_TA_TEXTURE 4u              /* mesa:svga3d_types.h:1350 linux:svga3d_types.h:1080 SVGA3D_TA_TEXTURE */
#define VMSVGA3D_TEX_ADDRESS_WRAP 1u        /* mesa:svga3d_types.h:1298 linux:svga3d_types.h:1035 SVGA3D_TEX_ADDRESS_WRAP */
#define VMSVGA3D_TEX_ADDRESS_CLAMP 3u       /* mesa:svga3d_types.h:1300 linux:svga3d_types.h:1037 SVGA3D_TEX_ADDRESS_CLAMP */
#define VMSVGA3D_TEX_FILTER_NONE 0u         /* mesa:svga3d_types.h:1312 linux:svga3d_types.h:1045 SVGA3D_TEX_FILTER_NONE */
#define VMSVGA3D_TEX_FILTER_NEAREST 1u      /* mesa:svga3d_types.h:1314 linux:svga3d_types.h:1047 SVGA3D_TEX_FILTER_NEAREST */
#define VMSVGA3D_TEX_FILTER_LINEAR 2u       /* mesa:svga3d_types.h:1315 linux:svga3d_types.h:1048 SVGA3D_TEX_FILTER_LINEAR */

/* Compare functions, blend factors/equation, face, shading. */
#define VMSVGA3D_CMP_NEVER 1u               /* mesa:svga3d_types.h:1077 linux:svga3d_types.h:848 SVGA3D_CMP_NEVER */
#define VMSVGA3D_CMP_LESS 2u                /* mesa:svga3d_types.h:1078 linux:svga3d_types.h:849 SVGA3D_CMP_LESS */
#define VMSVGA3D_CMP_EQUAL 3u               /* mesa:svga3d_types.h:1079 linux:svga3d_types.h:850 SVGA3D_CMP_EQUAL */
#define VMSVGA3D_CMP_LESSEQUAL 4u           /* mesa:svga3d_types.h:1080 linux:svga3d_types.h:851 SVGA3D_CMP_LESSEQUAL */
#define VMSVGA3D_CMP_GREATER 5u             /* mesa:svga3d_types.h:1081 linux:svga3d_types.h:852 SVGA3D_CMP_GREATER */
#define VMSVGA3D_CMP_NOTEQUAL 6u            /* mesa:svga3d_types.h:1082 linux:svga3d_types.h:853 SVGA3D_CMP_NOTEQUAL */
#define VMSVGA3D_CMP_GREATEREQUAL 7u        /* mesa:svga3d_types.h:1083 linux:svga3d_types.h:854 SVGA3D_CMP_GREATEREQUAL */
#define VMSVGA3D_CMP_ALWAYS 8u              /* mesa:svga3d_types.h:1084 linux:svga3d_types.h:855 SVGA3D_CMP_ALWAYS */
#define VMSVGA3D_BLENDOP_ZERO 1u            /* mesa:svga3d_types.h:1000 linux:svga3d_types.h:774 SVGA3D_BLENDOP_ZERO */
#define VMSVGA3D_BLENDOP_ONE 2u             /* mesa:svga3d_types.h:1001 linux:svga3d_types.h:775 SVGA3D_BLENDOP_ONE */
#define VMSVGA3D_BLENDOP_SRCCOLOR 3u        /* mesa:svga3d_types.h:1002 linux:svga3d_types.h:776 SVGA3D_BLENDOP_SRCCOLOR */
#define VMSVGA3D_BLENDOP_INVSRCCOLOR 4u     /* mesa:svga3d_types.h:1003 linux:svga3d_types.h:777 SVGA3D_BLENDOP_INVSRCCOLOR */
#define VMSVGA3D_BLENDOP_SRCALPHA 5u        /* mesa:svga3d_types.h:1004 linux:svga3d_types.h:778 SVGA3D_BLENDOP_SRCALPHA */
#define VMSVGA3D_BLENDOP_INVSRCALPHA 6u     /* mesa:svga3d_types.h:1005 linux:svga3d_types.h:779 SVGA3D_BLENDOP_INVSRCALPHA */
#define VMSVGA3D_BLENDOP_DESTALPHA 7u       /* mesa:svga3d_types.h:1006 linux:svga3d_types.h:780 SVGA3D_BLENDOP_DESTALPHA */
#define VMSVGA3D_BLENDOP_INVDESTALPHA 8u    /* mesa:svga3d_types.h:1007 linux:svga3d_types.h:781 SVGA3D_BLENDOP_INVDESTALPHA */
#define VMSVGA3D_BLENDOP_DESTCOLOR 9u       /* mesa:svga3d_types.h:1008 linux:svga3d_types.h:782 SVGA3D_BLENDOP_DESTCOLOR */
#define VMSVGA3D_BLENDOP_INVDESTCOLOR 10u   /* mesa:svga3d_types.h:1009 linux:svga3d_types.h:783 SVGA3D_BLENDOP_INVDESTCOLOR */
#define VMSVGA3D_BLENDEQ_ADD 1u             /* mesa:svga3d_types.h:1025 linux:svga3d_types.h:799 SVGA3D_BLENDEQ_ADD */
#define VMSVGA3D_FACE_NONE 1u               /* mesa:svga3d_types.h:1063 linux:svga3d_types.h:838 SVGA3D_FACE_NONE */
#define VMSVGA3D_SHADEMODE_SMOOTH 2u        /* mesa:svga3d_types.h:980 linux:svga3d_types.h:756 SVGA3D_SHADEMODE_SMOOTH */
#define VMSVGA3D_CLEAR_COLOR 1u             /* mesa:svga3d_types.h:1148 linux:svga3d_types.h:905 SVGA3D_CLEAR_COLOR */
#define VMSVGA3D_CLEAR_DEPTH 2u             /* mesa:svga3d_types.h:1149 linux:svga3d_types.h:906 SVGA3D_CLEAR_DEPTH */
#define VMSVGA3D_RT_DEPTH 0u                /* mesa:svga3d_types.h:1161 linux:svga3d_types.h:913 SVGA3D_RT_DEPTH */
#define VMSVGA3D_RT_COLOR0 2u               /* mesa:svga3d_types.h:1164 linux:svga3d_types.h:916 SVGA3D_RT_COLOR0 */

/* Vertex declarations; usages are positional enumerators (POSITION=0). */
#define VMSVGA3D_DECLTYPE_FLOAT2 1u         /* mesa:svga3d_types.h:1407 linux:svga3d_types.h:1124 SVGA3D_DECLTYPE_FLOAT2 */
#define VMSVGA3D_DECLTYPE_FLOAT4 3u         /* mesa:svga3d_types.h:1409 linux:svga3d_types.h:1126 SVGA3D_DECLTYPE_FLOAT4 */
#define VMSVGA3D_DECLTYPE_D3DCOLOR 4u       /* mesa:svga3d_types.h:1410 linux:svga3d_types.h:1127 SVGA3D_DECLTYPE_D3DCOLOR */
#define VMSVGA3D_DECLMETHOD_DEFAULT 0u      /* mesa:svga3d_types.h:1395 linux:svga3d_types.h:1113 SVGA3D_DECLMETHOD_DEFAULT */
#define VMSVGA3D_DECLUSAGE_POSITION 0u      /* mesa:svga3d_types.h:1377 linux:svga3d_types.h:1095 SVGA3D_DECLUSAGE_POSITION */
#define VMSVGA3D_DECLUSAGE_TEXCOORD 5u      /* mesa:svga3d_types.h:1382 linux:svga3d_types.h:1100 SVGA3D_DECLUSAGE_TEXCOORD */
#define VMSVGA3D_DECLUSAGE_COLOR 10u        /* mesa:svga3d_types.h:1387 linux:svga3d_types.h:1105 SVGA3D_DECLUSAGE_COLOR */
#define VMSVGA3D_TRANSFORM_WORLD 1u         /* mesa:svga3d_types.h:1519 linux:svga3d_types.h:1215 SVGA3D_TRANSFORM_WORLD */
#define VMSVGA3D_TRANSFORM_VIEW 2u          /* mesa:svga3d_types.h:1521 linux:svga3d_types.h:1217 SVGA3D_TRANSFORM_VIEW */
#define VMSVGA3D_TRANSFORM_PROJECTION 3u    /* mesa:svga3d_types.h:1522 linux:svga3d_types.h:1218 SVGA3D_TRANSFORM_PROJECTION */
#define VMSVGA3D_PRIMITIVE_TRIANGLELIST 1u  /* mesa:svga3d_types.h:1463 linux:svga3d_types.h:1159 SVGA3D_PRIMITIVE_TRIANGLELIST */
#define VMSVGA3D_MAX_VERTEX_ARRAYS 32u      /* mesa:svga3d_limits.h:95 linux:svga3d_limits.h:76 SVGA3D_MAX_VERTEX_ARRAYS */

/* Device capabilities (FIFO SVGA3dCapsRecord pairs {index,value}). */
#define VMSVGA3D_DEVCAP_3D 0u               /* mesa:svga3d_devcaps.h:73 linux:svga3d_devcaps.h:58 SVGA3D_DEVCAP_3D */
#define VMSVGA3D_DEVCAP_MAX_TEXTURES 2u     /* mesa:svga3d_devcaps.h:83 linux:svga3d_devcaps.h:61 SVGA3D_DEVCAP_MAX_TEXTURES */
#define VMSVGA3D_DEVCAP_D16_BUFFER_FORMAT 12u /* mesa:svga3d_devcaps.h:93 linux:svga3d_devcaps.h:71 SVGA3D_DEVCAP_D16_BUFFER_FORMAT */
#define VMSVGA3D_DEVCAP_MAX_TEXTURE_WIDTH 19u /* mesa:svga3d_devcaps.h:100 linux:svga3d_devcaps.h:78 SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH */
#define VMSVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT 20u /* mesa:svga3d_devcaps.h:101 linux:svga3d_devcaps.h:79 SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT */
#define VMSVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT 25u /* mesa:svga3d_devcaps.h:106 linux:svga3d_devcaps.h:84 SVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT */
#define VMSVGA3D_DEVCAP_MAX_VERTEX_INDEX 26u /* mesa:svga3d_devcaps.h:107 linux:svga3d_devcaps.h:85 SVGA3D_DEVCAP_MAX_VERTEX_INDEX */
#define VMSVGA3D_DEVCAP_TEXTURE_OPS 31u     /* mesa:svga3d_devcaps.h:112 linux:svga3d_devcaps.h:90 SVGA3D_DEVCAP_TEXTURE_OPS */
#define VMSVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8 33u /* mesa:svga3d_devcaps.h:114 linux:svga3d_devcaps.h:92 SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8 */
#define VMSVGA3D_DEVCAP_SURFACEFMT_Z_D16 43u /* mesa:svga3d_devcaps.h:124 linux:svga3d_devcaps.h:102 SVGA3D_DEVCAP_SURFACEFMT_Z_D16 */
#define VMSVGA3D_DEVCAP_MAX_CONTEXT_IDS 77u /* mesa:svga3d_devcaps.h:182 linux:svga3d_devcaps.h:143 SVGA3D_DEVCAP_MAX_CONTEXT_IDS */
#define VMSVGA3D_DEVCAP_MAX_SURFACE_IDS 78u /* mesa:svga3d_devcaps.h:188 linux:svga3d_devcaps.h:145 SVGA3D_DEVCAP_MAX_SURFACE_IDS */
#define VMSVGA3D_FORMAT_OP_TEXTURE 0x1u     /* mesa:svga3d_types.h:656 linux:svga3d_types.h:525 SVGA3DFORMAT_OP_TEXTURE */
#define VMSVGA3D_FORMAT_OP_OFFSCREEN_RENDERTARGET 0x8u /* mesa:svga3d_types.h:659 linux:svga3d_types.h:528 SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET */
#define VMSVGA3D_FORMAT_OP_ZSTENCIL 0x40u   /* mesa:svga3d_types.h:661 linux:svga3d_types.h:530 SVGA3DFORMAT_OP_ZSTENCIL */
/* Guest-backed-object hosts expose devcaps through an index/value register
 * (Linux v6.1 vmwgfx_devcaps.c:85-100); their FIFO caps block may be empty. */
#define VMSVGA3D_REG_DEV_CAP 52u            /* mesa:svga_reg.h:187 linux:svga_reg.h:165 SVGA_REG_DEV_CAP */
#define VMSVGA3D_CAP_GBOBJECTS 0x08000000u  /* mesa:svga_reg.h:648 linux:svga_reg.h:422 SVGA_CAP_GBOBJECTS */
/* Caps block records: VirtualBox b6bea72 vmsvga_include/svga3d_caps.h:66-109
 * (header {length in words, type}; DEVCAPS types 0x100..0x1ff; zero ends). */
#define VMSVGA3D_CAPS_RECORD_DEVCAPS_MIN 0x100u
#define VMSVGA3D_CAPS_RECORD_DEVCAPS_MAX 0x1ffu
/* Minimum host: SVGA3D_HWVERSION_WS8_B1 = MAKE_HWVERSION(2,1); the Linux v6.1
 * vmw_supports_3d gate (vmwgfx_cmd.c:72) and VirtualBox's reported version. */
#define VMSVGA3D_HWVERSION_WS8_B1 0x00020001u /* mesa:svga3d_devcaps.h:63 linux:svga3d_devcaps.h:51 SVGA3D_HWVERSION_WS8_B1 */

#endif
