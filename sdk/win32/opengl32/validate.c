/* Argument validation shared by direct calls and display-list compilation.
 * State-dependent errors (stack depth, texture contents) remain at execution.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#define I(n) ((int)a[n].i)
#define P(n) (a[n].p)
static int cap(int n) {
  return n == GL_TEXTURE_1D || n == GL_TEXTURE_2D || n == GL_DEPTH_TEST ||
         n == GL_STENCIL_TEST || n == GL_ALPHA_TEST || n == GL_BLEND || n == GL_SCISSOR_TEST ||
         n == GL_FOG || n == GL_LIGHTING || n == GL_NORMALIZE ||
         n == GL_CULL_FACE || n == GL_COLOR_MATERIAL ||
         n == GL_POLYGON_OFFSET_FILL || n == GL_POLYGON_OFFSET_LINE ||
         n == GL_POLYGON_OFFSET_POINT ||
         (n >= GL_LIGHT0 && n < GL_LIGHT0 + MAX_LIGHTS) ||
         (n >= GL_CLIP_PLANE0 && n < GL_CLIP_PLANE0 + 6) ||
         (n >= GL_TEXTURE_GEN_S && n <= GL_TEXTURE_GEN_Q);
}
static int target(int n) { return n == GL_TEXTURE_1D || n == GL_TEXTURE_2D; }
static int internal(int n) {
  return (n >= 1 && n <= 4) || n == GL_ALPHA || n == GL_RGB || n == GL_RGBA ||
         n == GL_LUMINANCE || n == GL_LUMINANCE_ALPHA ||
         (n >= 0x803b && n <= 0x805b) || (n >= 0x84e9 && n <= 0x84ee);
}
static int scalar_type(int t) {
  return t == GL_BYTE || t == GL_UNSIGNED_BYTE                  ? 1
         : t == GL_SHORT || t == GL_UNSIGNED_SHORT              ? 2
         : t == GL_INT || t == GL_UNSIGNED_INT || t == GL_FLOAT ? 4
                                                                : 0;
}
unsigned gl_validate(struct gl_context *c, unsigned id, const union gl_arg *a) {
  (void)c;
  switch (id) {
#include "family_validation.inc"
  case ID_glBegin:
    return I(0) < GL_POINTS || I(0) > GL_POLYGON ? GL_INVALID_ENUM : 0;
  case ID_glEnd:
  case ID_glCallList:
  case ID_glListBase:
  case ID_glClearColor:
  case ID_glClearDepth:
  case ID_glClearStencil:
  case ID_glStencilMask:
  case ID_glColorMask:
  case ID_glDepthMask:
  case ID_glDepthRange:
  case ID_glPolygonOffset:
  case ID_glLoadIdentity:
  case ID_glPushMatrix:
  case ID_glPopMatrix:
  case ID_glTranslatef:
  case ID_glTranslated:
  case ID_glScalef:
  case ID_glScaled:
  case ID_glRotatef:
  case ID_glRotated:
  case ID_glPopAttrib:
  case ID_glPixelZoom:
    return 0;
  case ID_glEdgeFlag:
    return 0;
  case ID_glEdgeFlagv:
    return P(0) ? 0 : GL_INVALID_VALUE;
  case ID_glCallLists:
    if (I(0) < 0 || I(0) > 1048576)
      return GL_INVALID_VALUE;
    if (!scalar_type(I(1)) && I(1) != GL_2_BYTES && I(1) != GL_3_BYTES &&
        I(1) != GL_4_BYTES)
      return GL_INVALID_ENUM;
    return I(0) && !P(2) ? GL_INVALID_VALUE : 0;
  case ID_glEnable:
  case ID_glDisable:
    return cap(I(0)) ? 0 : GL_INVALID_ENUM;
  case ID_glActiveTextureARB:
    return I(0) < GL_TEXTURE0_ARB || I(0) > GL_TEXTURE0_ARB + 1
               ? GL_INVALID_ENUM
               : 0;
  case ID_glStencilFunc:
  case ID_glDepthFunc:
  case ID_glAlphaFunc:
    return I(0) < GL_NEVER || I(0) > GL_ALWAYS ? GL_INVALID_ENUM : 0;
  case ID_glStencilOp:
    for (int i = 0; i < 3; i++) {
      int v = I(i);
      if (v != GL_KEEP && v != GL_ZERO && v != GL_REPLACE &&
          v != GL_INCR && v != GL_DECR && v != GL_INVERT)
        return GL_INVALID_ENUM;
    }
    return 0;
  case ID_glBlendFunc:
    if (I(0) != GL_ZERO && I(0) != GL_ONE && I(0) != GL_DST_COLOR &&
        I(0) != GL_ONE_MINUS_DST_COLOR && I(0) != GL_SRC_ALPHA &&
        I(0) != GL_ONE_MINUS_SRC_ALPHA && I(0) != GL_DST_ALPHA &&
        I(0) != GL_ONE_MINUS_DST_ALPHA && I(0) != GL_SRC_ALPHA_SATURATE)
      return GL_INVALID_ENUM;
    if (I(1) != GL_ZERO && I(1) != GL_ONE && I(1) != GL_SRC_COLOR &&
        I(1) != GL_ONE_MINUS_SRC_COLOR && I(1) != GL_SRC_ALPHA &&
        I(1) != GL_ONE_MINUS_SRC_ALPHA && I(1) != GL_DST_ALPHA &&
        I(1) != GL_ONE_MINUS_DST_ALPHA)
      return GL_INVALID_ENUM;
    return 0;
  case ID_glClear:
    if (I(0) & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT))
      return GL_INVALID_VALUE;
    return 0;
  case ID_glViewport:
    return I(2) < 0 || I(3) < 0 ? GL_INVALID_VALUE : 0;
  case ID_glScissor:
    if (I(2) < 0 || I(3) < 0 || I(2) > 65536 || I(3) > 65536)
      return GL_INVALID_VALUE;
    return 0;
  case ID_glMatrixMode:
    return I(0) != GL_MODELVIEW && I(0) != GL_PROJECTION && I(0) != GL_TEXTURE
               ? GL_INVALID_ENUM
               : 0;
  case ID_glLoadMatrixf:
  case ID_glMultMatrixf:
  case ID_glLoadMatrixd:
  case ID_glMultMatrixd:
    return P(0) ? 0 : GL_INVALID_VALUE;
  case ID_glOrtho:
  case ID_glFrustum:
    return a[0].f == a[1].f || a[2].f == a[3].f || a[4].f == a[5].f ||
                   (id == ID_glFrustum && (a[4].f <= 0 || a[5].f <= 0))
               ? GL_INVALID_VALUE
               : 0;
  case ID_glShadeModel:
    return I(0) != GL_FLAT && I(0) != GL_SMOOTH ? GL_INVALID_ENUM : 0;
  case ID_glFrontFace:
    return I(0) != GL_CW && I(0) != GL_CCW ? GL_INVALID_ENUM : 0;
  case ID_glCullFace:
    return I(0) != GL_FRONT && I(0) != GL_BACK && I(0) != GL_FRONT_AND_BACK
               ? GL_INVALID_ENUM
               : 0;
  case ID_glPolygonMode:
    if (I(0) != GL_FRONT && I(0) != GL_BACK && I(0) != GL_FRONT_AND_BACK)
      return GL_INVALID_ENUM;
    return I(1) != GL_POINT && I(1) != GL_LINE && I(1) != GL_FILL
               ? GL_INVALID_ENUM
               : 0;
  case ID_glLineWidth:
    return !(a[0].f > 0) ? GL_INVALID_VALUE : 0;
  case ID_glPointSize:
    return !(a[0].f > 0 && a[0].f <= 64) ? GL_INVALID_VALUE : 0;
  case ID_glPushAttrib:
    return (unsigned)I(0) & ~GL_ALL_ATTRIB_BITS ? GL_INVALID_VALUE : 0;
  case ID_glClipPlane:
    if (I(0) < GL_CLIP_PLANE0 || I(0) >= GL_CLIP_PLANE0 + 6)
      return GL_INVALID_ENUM;
    return P(1) ? 0 : GL_INVALID_VALUE;
  case ID_glTexGend:
  case ID_glTexGenf:
  case ID_glTexGeni:
  case ID_glTexGendv:
  case ID_glTexGenfv:
  case ID_glTexGeniv: {
    int vector =
        id == ID_glTexGendv || id == ID_glTexGenfv || id == ID_glTexGeniv;
    if (I(0) < GL_S || I(0) > GL_Q ||
        (I(1) != GL_TEXTURE_GEN_MODE && I(1) != GL_OBJECT_PLANE &&
         I(1) != GL_EYE_PLANE) ||
        (!vector && I(1) != GL_TEXTURE_GEN_MODE))
      return GL_INVALID_ENUM;
    if (vector && !P(2))
      return GL_INVALID_VALUE;
    if (I(1) == GL_TEXTURE_GEN_MODE) {
      double mode = vector ? id == ID_glTexGendv   ? *(const double *)P(2)
                             : id == ID_glTexGenfv ? *(const float *)P(2)
                                                   : *(const int *)P(2)
                    : id == ID_glTexGeni ? a[2].i
                                         : a[2].f;
      if (mode != GL_OBJECT_LINEAR && mode != GL_EYE_LINEAR &&
          (mode != GL_SPHERE_MAP || I(0) > GL_T))
        return GL_INVALID_ENUM;
    }
    return 0;
  }
  case ID_glBindTexture:
    return target(I(0)) ? 0 : GL_INVALID_ENUM;
  case ID_glTexImage1D:
  case ID_glTexImage2D:
  case ID_glTexSubImage1D:
  case ID_glTexSubImage2D: {
    int one = id == ID_glTexImage1D || id == ID_glTexSubImage1D;
    int sub = id == ID_glTexSubImage1D || id == ID_glTexSubImage2D;
    if (I(0) != (one ? GL_TEXTURE_1D : GL_TEXTURE_2D))
      return GL_INVALID_ENUM;
    int w = I(sub ? one ? 3 : 4 : 3), h = one ? 1 : I(sub ? 5 : 4);
    int f = I(sub   ? one ? 4 : 6
              : one ? 5
                    : 6),
        t = I(sub   ? one ? 5 : 7
              : one ? 6
                    : 7);
    if (I(1) < 0 || I(1) >= SW_LEVELS || w < 0 || w > 2048 || h < 0 || h > 2048)
      return GL_INVALID_VALUE;
    if (gl_pixel_bytes(f, t) <= 0)
      return GL_INVALID_ENUM;
    if (!sub && ((w && (w & (w - 1))) || (h && (h & (h - 1))) ||
                 I(one ? 4 : 5) || !internal(I(2))))
      return GL_INVALID_VALUE;
    if (sub && (I(2) < 0 || (!one && I(3) < 0)))
      return GL_INVALID_VALUE;
    return 0;
  }
  case ID_glTexParameteri:
  case ID_glTexParameterf:
  case ID_glTexParameteriv:
  case ID_glTexParameterfv: {
    if (!target(I(0)))
      return GL_INVALID_ENUM;
    int vector = id == ID_glTexParameteriv || id == ID_glTexParameterfv;
    if (vector && !P(2))
      return GL_INVALID_VALUE;
    if (I(1) == GL_TEXTURE_BORDER_COLOR)
      return vector ? 0 : GL_INVALID_ENUM;
    int v = vector ? id == ID_glTexParameteriv ? *(const int *)P(2)
                                               : (int)*(const float *)P(2)
            : id == ID_glTexParameteri ? I(2)
                                       : (int)a[2].f;
    if (I(1) == GL_TEXTURE_WRAP_S || I(1) == GL_TEXTURE_WRAP_T)
      return v == GL_CLAMP || v == GL_REPEAT || v == GL_CLAMP_TO_EDGE
                 ? 0
                 : GL_INVALID_ENUM;
    if (I(1) == GL_TEXTURE_MAG_FILTER)
      return v == GL_NEAREST || v == GL_LINEAR ? 0 : GL_INVALID_ENUM;
    if (I(1) == GL_TEXTURE_MIN_FILTER)
      return v == GL_NEAREST || v == GL_LINEAR ||
                     (v >= GL_NEAREST_MIPMAP_NEAREST &&
                      v <= GL_LINEAR_MIPMAP_LINEAR)
                 ? 0
                 : GL_INVALID_ENUM;
    return GL_INVALID_ENUM;
  }
  case ID_glTexEnvi:
  case ID_glTexEnvf:
  case ID_glTexEnviv:
  case ID_glTexEnvfv: {
    if (I(0) != GL_TEXTURE_ENV)
      return GL_INVALID_ENUM;
    int vector = id == ID_glTexEnviv || id == ID_glTexEnvfv;
    if (vector && !P(2))
      return GL_INVALID_VALUE;
    if (I(1) == GL_TEXTURE_ENV_COLOR)
      return vector ? 0 : GL_INVALID_ENUM;
    int v = vector ? id == ID_glTexEnviv ? *(const int *)P(2)
                                         : (int)*(const float *)P(2)
            : id == ID_glTexEnvi ? I(2)
                                 : (int)a[2].f;
    if (I(1) == GL_TEXTURE_ENV_MODE)
      return v == GL_MODULATE || v == GL_DECAL || v == GL_BLEND ||
                     v == GL_REPLACE || v == GL_ADD || v == GL_COMBINE_ARB
                 ? 0
                 : GL_INVALID_ENUM;
    if (I(1) == GL_COMBINE_RGB_ARB || I(1) == GL_COMBINE_ALPHA_ARB)
      return v == GL_REPLACE || v == GL_MODULATE || v == GL_ADD ||
                     v == GL_ADD_SIGNED_ARB || v == GL_INTERPOLATE_ARB ||
                     v == GL_SUBTRACT_ARB
                 ? 0
                 : GL_INVALID_ENUM;
    if (I(1) == GL_RGB_SCALE_ARB || I(1) == GL_ALPHA_SCALE)
      return v == 1 || v == 2 || v == 4 ? 0 : GL_INVALID_VALUE;
    if ((I(1) >= GL_SOURCE0_RGB_ARB && I(1) < GL_SOURCE0_RGB_ARB + 3) ||
        (I(1) >= GL_SOURCE0_ALPHA_ARB && I(1) < GL_SOURCE0_ALPHA_ARB + 3))
      return v == GL_TEXTURE || v == GL_CONSTANT_ARB ||
                     v == GL_PRIMARY_COLOR_ARB || v == GL_PREVIOUS_ARB
                 ? 0
                 : GL_INVALID_ENUM;
    if ((I(1) >= GL_OPERAND0_RGB_ARB && I(1) < GL_OPERAND0_RGB_ARB + 3) ||
        (I(1) >= GL_OPERAND0_ALPHA_ARB && I(1) < GL_OPERAND0_ALPHA_ARB + 3))
      return v == GL_SRC_ALPHA || v == GL_ONE_MINUS_SRC_ALPHA ||
                     (I(1) < GL_OPERAND0_ALPHA_ARB &&
                      (v == GL_SRC_COLOR || v == GL_ONE_MINUS_SRC_COLOR))
                 ? 0
                 : GL_INVALID_ENUM;
    return GL_INVALID_ENUM;
  }
  case ID_glCompressedTexImage2DARB:
  case ID_glCompressedTexSubImage2DARB: {
    if (I(0) != GL_TEXTURE_2D)
      return GL_INVALID_ENUM;
    int sub = id == ID_glCompressedTexSubImage2DARB;
    int w = I(sub ? 4 : 3), h = I(sub ? 5 : 4), f = I(sub ? 6 : 2),
        n = I(sub ? 7 : 6);
    if (f < 0x83f0 || f > 0x83f3)
      return GL_INVALID_ENUM;
    if (I(1) < 0 || I(1) >= SW_LEVELS || w < 0 || h < 0 || w > 2048 ||
        h > 2048 ||
        (!sub && (I(5) || (w && (w & (w - 1))) || (h && (h & (h - 1))))))
      return GL_INVALID_VALUE;
    if (n != ((w + 3) / 4) * ((h + 3) / 4) * (f < 0x83f2 ? 8 : 16))
      return GL_INVALID_VALUE;
    return n && !P(sub ? 8 : 7) ? GL_INVALID_VALUE : 0;
  }
  case ID_glCompressedTexImage1DARB:
  case ID_glCompressedTexSubImage1DARB:
  case ID_glCompressedTexImage3DARB:
  case ID_glCompressedTexSubImage3DARB:
    return GL_INVALID_ENUM; /* S3TC supplies 2D formats only. */
  case ID_glDrawPixels:
    if (I(0) < 0 || I(1) < 0 || I(0) > 2048 || I(1) > 2048)
      return GL_INVALID_VALUE;
    if (gl_pixel_bytes(I(2), I(3)) <= 0)
      return GL_INVALID_ENUM;
    return I(0) && I(1) && !P(4) ? GL_INVALID_VALUE : 0;
  case ID_glBitmap:
    return I(0) < 0 || I(1) < 0 || I(0) > 2048 || I(1) > 2048 ? GL_INVALID_VALUE
                                                              : 0;
  case ID_glCopyPixels:
    if (I(4) != GL_COLOR)
      return GL_INVALID_ENUM;
    return I(2) < 0 || I(3) < 0 || I(2) > 2048 || I(3) > 2048 ? GL_INVALID_VALUE
                                                              : 0;
  case ID_glFogf:
  case ID_glFogi:
  case ID_glFogfv:
  case ID_glFogiv: {
    int vector = id == ID_glFogfv || id == ID_glFogiv;
    if (vector && !P(1))
      return GL_INVALID_VALUE;
    if (I(0) == GL_FOG_COLOR)
      return vector ? 0 : GL_INVALID_ENUM;
    double v =
        vector ? id == ID_glFogfv ? *(const float *)P(1) : *(const int *)P(1)
        : id == ID_glFogf ? a[1].f
                          : a[1].i;
    if (I(0) == GL_FOG_MODE)
      return v == GL_LINEAR || v == GL_EXP || v == GL_EXP2 ? 0
                                                           : GL_INVALID_ENUM;
    if (I(0) == GL_FOG_DENSITY)
      return v < 0 ? GL_INVALID_VALUE : 0;
    return I(0) == GL_FOG_START || I(0) == GL_FOG_END ? 0 : GL_INVALID_ENUM;
  }
  case ID_glColorMaterial:
    if (I(0) != GL_FRONT && I(0) != GL_BACK && I(0) != GL_FRONT_AND_BACK)
      return GL_INVALID_ENUM;
    return I(1) == GL_AMBIENT || I(1) == GL_DIFFUSE || I(1) == GL_SPECULAR ||
                   I(1) == GL_EMISSION || I(1) == GL_AMBIENT_AND_DIFFUSE
               ? 0
               : GL_INVALID_ENUM;
  case ID_glLightf:
  case ID_glLighti:
  case ID_glLightfv:
  case ID_glLightiv:
  case ID_glMaterialf:
  case ID_glMateriali:
  case ID_glMaterialfv:
  case ID_glMaterialiv: {
    int light = id == ID_glLightf || id == ID_glLighti || id == ID_glLightfv ||
                id == ID_glLightiv;
    int vector = id == ID_glLightfv || id == ID_glLightiv ||
                 id == ID_glMaterialfv || id == ID_glMaterialiv;
    int integer = id == ID_glLighti || id == ID_glLightiv ||
                  id == ID_glMateriali || id == ID_glMaterialiv;
    if (vector && !P(2))
      return GL_INVALID_VALUE;
    if (light
            ? I(0) < GL_LIGHT0 || I(0) >= GL_LIGHT0 + MAX_LIGHTS
            : I(0) != GL_FRONT && I(0) != GL_BACK && I(0) != GL_FRONT_AND_BACK)
      return GL_INVALID_ENUM;
    double v = vector    ? integer ? *(const int *)P(2) : *(const float *)P(2)
               : integer ? a[2].i
                         : a[2].f;
    if (I(1) == GL_AMBIENT || I(1) == GL_DIFFUSE || I(1) == GL_SPECULAR ||
        (light ? I(1) == GL_POSITION || I(1) == GL_SPOT_DIRECTION
               : I(1) == GL_EMISSION || I(1) == GL_AMBIENT_AND_DIFFUSE))
      return vector ? 0 : GL_INVALID_ENUM;
    if (!light)
      return I(1) != GL_SHININESS ? GL_INVALID_ENUM
             : v < 0 || v > 128   ? GL_INVALID_VALUE
                                  : 0;
    if (I(1) == GL_SPOT_EXPONENT)
      return v < 0 || v > 128 ? GL_INVALID_VALUE : 0;
    if (I(1) == GL_SPOT_CUTOFF)
      return (v < 0 || v > 90) && v != 180 ? GL_INVALID_VALUE : 0;
    if (I(1) == GL_CONSTANT_ATTENUATION || I(1) == GL_LINEAR_ATTENUATION ||
        I(1) == GL_QUADRATIC_ATTENUATION)
      return v < 0 ? GL_INVALID_VALUE : 0;
    return GL_INVALID_ENUM;
  }
  case ID_glLightModelf:
  case ID_glLightModeli:
  case ID_glLightModelfv:
  case ID_glLightModeliv: {
    int vector = id == ID_glLightModelfv || id == ID_glLightModeliv;
    if (vector && !P(1))
      return GL_INVALID_VALUE;
    if (I(0) == GL_LIGHT_MODEL_AMBIENT)
      return vector ? 0 : GL_INVALID_ENUM;
    return I(0) == GL_LIGHT_MODEL_LOCAL_VIEWER ||
                   I(0) == GL_LIGHT_MODEL_TWO_SIDE
               ? 0
               : GL_INVALID_ENUM;
  }
  case ID_glHint:
    if (I(0) != GL_PERSPECTIVE_CORRECTION_HINT &&
        I(0) != GL_POINT_SMOOTH_HINT && I(0) != GL_LINE_SMOOTH_HINT &&
        I(0) != GL_POLYGON_SMOOTH_HINT && I(0) != GL_FOG_HINT &&
        I(0) != GL_TEXTURE_COMPRESSION_HINT_ARB)
      return GL_INVALID_ENUM;
    return I(1) == GL_DONT_CARE || I(1) == GL_FASTEST || I(1) == GL_NICEST
               ? 0
               : GL_INVALID_ENUM;
  case ID_glDrawBuffer:
  case ID_glReadBuffer:
    return I(0) == GL_FRONT || I(0) == GL_BACK || I(0) == GL_FRONT_LEFT || I(0) == GL_BACK_LEFT ||
                   (id == ID_glDrawBuffer && (I(0) == GL_FRONT_AND_BACK || I(0) == GL_NONE))
               ? 0
               : GL_INVALID_ENUM;
  case ID_glPrioritizeTextures:
    return I(0) < 0 || (I(0) && (!P(1) || !P(2))) ? GL_INVALID_VALUE : 0;
  default:
    return GL_INVALID_OPERATION;
  }
}
