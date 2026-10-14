/* SPDX-License-Identifier: MIT
 * Use a distinct filename: FreeType's quoted option include otherwise finds
 * the upstream ftoption.h beside ftconfig.h before searching our -I path. */
#include <freetype/config/ftoption.h>
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_SYSTEM_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_INCREMENTAL
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_POSTSCRIPT_NAMES
#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST
#undef FT_CONFIG_OPTION_SUBPIXEL_RENDERING
#undef TT_CONFIG_OPTION_EMBEDDED_BITMAPS
#undef TT_CONFIG_OPTION_COLOR_LAYERS
#undef TT_SUPPORT_COLRV1
#undef TT_CONFIG_OPTION_SFNT_NAMES
#undef TT_CONFIG_OPTION_BDF
#undef TT_CONFIG_OPTION_SUBPIXEL_HINTING
#undef TT_SUPPORT_SUBPIXEL_HINTING_MINIMAL
#undef TT_SUPPORT_SUBPIXEL_HINTING_INFINALITY
#undef AF_CONFIG_OPTION_CJK
#undef AF_CONFIG_OPTION_INDIC
/* TrueType bytecode and GX variations stay enabled. Disabling subpixel
 * hinting selects the v35 interpreter; libfont also checks it explicitly. */
