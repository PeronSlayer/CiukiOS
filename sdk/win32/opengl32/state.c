/* Bounded server/client state stacks. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
struct gl_snapshot {
  struct gl_snapshot *next;
  unsigned mask;
  struct gl_context state;
  float point;
};
void gl_drop_state(struct gl_context *c) {
  while (c->attributes) {
    struct gl_snapshot *p = c->attributes;
    c->attributes = p->next;
    if (p->mask & GL_TEXTURE_BIT)
      gl_texture_bindings(&p->state, -1);
    gl_dealloc(p);
  }
  while (c->client_attributes) {
    struct gl_snapshot *p = c->client_attributes;
    c->client_attributes = p->next;
    gl_dealloc(p);
  }
}
int gl_state(struct gl_context *c, unsigned id, const union gl_arg *a,
             union gl_arg *result) {
  if (id == ID_glPushAttrib || id == ID_glPushClientAttrib) {
    int client = id == ID_glPushClientAttrib;
    unsigned mask = (unsigned)a[0].i,
             *depth = client ? &c->client_depth : &c->attribute_depth;
    struct gl_snapshot **head = client ? &c->client_attributes : &c->attributes;
    if ((!client && (mask & ~GL_ALL_ATTRIB_BITS)) ||
        (client && (mask & ~3u) && mask != 0xffffffffu)) {
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    if (*depth == 16) {
      gl_error(c, GL_STACK_OVERFLOW);
      return 1;
    }
    struct gl_snapshot *s = gl_alloc(sizeof(*s));
    if (!s) {
      gl_error(c, GL_OUT_OF_MEMORY);
      return 1;
    }
    s->mask = mask;
    s->state = *c;
    if (!client && (mask & GL_TEXTURE_BIT))
      gl_texture_bindings(&s->state, 1);
    s->point = c->zb->pointsize;
    s->next = *head;
    *head = s;
    (*depth)++;
    return 1;
  }
  if (id == ID_glPopClientAttrib) {
    struct gl_snapshot *p = c->client_attributes;
    if (!p) {
      gl_error(c, GL_STACK_UNDERFLOW);
      return 1;
    }
    if (p->mask & 1) {
      w32_copy(c->pack, p->state.pack, sizeof(c->pack));
      w32_copy(c->unpack, p->state.unpack, sizeof(c->unpack));
    }
    if (p->mask & 2) {
      w32_copy(c->array, p->state.array, sizeof(c->array));
      c->client = p->state.client;
      c->locked_first = p->state.locked_first;
      c->locked_count = p->state.locked_count;
    }
    c->client_attributes = p->next;
    c->client_depth--;
    gl_dealloc(p);
    return 1;
  }
  if (id == ID_glPopAttrib) {
    struct gl_snapshot *p = c->attributes;
    if (!p) {
      gl_error(c, GL_STACK_UNDERFLOW);
      return 1;
    }
    struct gl_context *s = &p->state;
    unsigned mask = p->mask;
#define RESTORE(field) c->field = s->field
#define COPY(field) w32_copy(&c->field, &s->field, sizeof(c->field))
    if (mask & GL_CURRENT_BIT) {
      COPY(tiny.current_color);
      COPY(tiny.current_normal);
      COPY(tiny.current_tex_coord);
      COPY(tiny.current_tex_coord1);
      COPY(texcoord);
      COPY(raster);
      COPY(raster_color);
      RESTORE(raster_valid);
      RESTORE(tiny.current_edge_flag);
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
      RESTORE(sw.depth_test);
      RESTORE(sw.depth_write);
      RESTORE(sw.depth_func);
      RESTORE(clear_depth);
    }
    if (mask & GL_COLOR_BUFFER_BIT) {
      RESTORE(sw.alpha_test);
      RESTORE(sw.alpha_func);
      RESTORE(sw.alpha_ref);
      RESTORE(alpha_ref);
      RESTORE(sw.blend);
      RESTORE(sw.sfactor);
      RESTORE(sw.dfactor);
      RESTORE(sw.color_mask);
      COPY(clear);
    }
    if (mask & GL_VIEWPORT_BIT) {
      COPY(viewport);
      COPY(range);
      glViewport(c->viewport[0], c->viewport[1],
                 c->viewport[2] ? c->viewport[2] : 1,
                 c->viewport[3] ? c->viewport[3] : 1);
    }
    if (mask & GL_SCISSOR_BIT) {
      RESTORE(sw.scissor);
      COPY(scissor);
      gl_window_state(c);
    }
    if (mask & GL_FOG_BIT) {
      RESTORE(sw.fog);
      RESTORE(sw.fog_color);
      RESTORE(fog_mode);
      RESTORE(fog_density);
      RESTORE(fog_start);
      RESTORE(fog_end);
    }
    if (mask & GL_POLYGON_BIT) {
      RESTORE(tiny.polygon_mode_front);
      RESTORE(tiny.polygon_mode_back);
      RESTORE(tiny.current_front_face);
      RESTORE(tiny.current_cull_face);
      RESTORE(tiny.cull_face_enabled);
      RESTORE(tiny.offset_states);
      RESTORE(offset_factor);
      RESTORE(offset_units);
    }
    if (mask & GL_LIGHTING_BIT) {
      COPY(tiny.lights);
      COPY(tiny.materials);
      COPY(tiny.ambient_light_model);
      RESTORE(tiny.first_light);
      RESTORE(tiny.local_light_model);
      RESTORE(tiny.lighting_enabled);
      RESTORE(tiny.light_model_two_side);
      RESTORE(tiny.current_shade_model);
      RESTORE(tiny.color_material_enabled);
      RESTORE(tiny.current_color_material_mode);
      RESTORE(tiny.current_color_material_type);
    }
    if (mask & GL_TEXTURE_BIT) {
      gl_texture_bindings(c, -1);
      COPY(sw.unit);
      COPY(binding);
      COPY(texture_enabled);
      COPY(texgen);
      RESTORE(active);
      c->tiny.matrix_stack[2] = c->texture_base[c->active];
      c->tiny.matrix_stack_ptr[2] = c->texture_ptr[c->active];
      /* Transfer the snapshot's binding references to the current state. */
    }
    if (mask & GL_TRANSFORM_BIT) {
      RESTORE(tiny.matrix_mode);
      RESTORE(tiny.normalize_enabled);
      COPY(clip_plane);
      COPY(clip_enabled);
    }
    if (mask & GL_LIST_BIT)
      RESTORE(list_base);
    if (mask & GL_HINT_BIT)
      RESTORE(compression_hint);
    if (mask & GL_PIXEL_MODE_BIT)
      COPY(zoom);
    if (mask & GL_POINT_BIT)
      c->zb->pointsize = p->point;
    if (mask & GL_ENABLE_BIT) {
      RESTORE(sw.depth_test);
      RESTORE(sw.alpha_test);
      RESTORE(sw.blend);
      RESTORE(sw.scissor);
      RESTORE(sw.fog);
      COPY(texture_enabled);
      COPY(clip_enabled);
      for (int u = 0; u < 2; u++)
        for (int k = 0; k < 4; k++)
          RESTORE(texgen[u][k].enabled);
      RESTORE(tiny.lighting_enabled);
      RESTORE(tiny.normalize_enabled);
      RESTORE(tiny.color_material_enabled);
      RESTORE(tiny.cull_face_enabled);
      RESTORE(tiny.offset_states);
      for (int k = 0; k < MAX_LIGHTS; k++) {
        RESTORE(tiny.lights[k].enabled);
        RESTORE(tiny.lights[k].next);
        RESTORE(tiny.lights[k].prev);
      }
      RESTORE(tiny.first_light);
    }
    gl_sync_textures(c);
    c->tiny.matrix_model_projection_updated = 1;
    c->attributes = p->next;
    c->attribute_depth--;
    gl_dealloc(p);
    return 1;
#undef RESTORE
#undef COPY
  }
  if (id == ID_glGetLightfv || id == ID_glGetLightiv ||
      id == ID_glGetMaterialfv || id == ID_glGetMaterialiv) {
    int light = id == ID_glGetLightfv || id == ID_glGetLightiv,
        integer = id == ID_glGetLightiv || id == ID_glGetMaterialiv;
    unsigned object = (unsigned)a[0].i, pname = (unsigned)a[1].i;
    float value[4] = {0};
    int n = 4, color = 1;
    if (!a[2].p) {
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    if (light) {
      if (object < GL_LIGHT0 || object >= GL_LIGHT0 + MAX_LIGHTS) {
        gl_error(c, GL_INVALID_ENUM);
        return 1;
      }
      GLLight *l = &c->tiny.lights[object - GL_LIGHT0];
      switch (pname) {
      case GL_AMBIENT:
        w32_copy(value, l->ambient.v, 16);
        break;
      case GL_DIFFUSE:
        w32_copy(value, l->diffuse.v, 16);
        break;
      case GL_SPECULAR:
        w32_copy(value, l->specular.v, 16);
        break;
      case GL_POSITION:
        w32_copy(value, l->position.v, 16);
        color = 0;
        break;
      case GL_SPOT_DIRECTION:
        w32_copy(value, l->spot_direction.v, 12);
        n = 3;
        color = 0;
        break;
      case GL_SPOT_EXPONENT:
        value[0] = l->spot_exponent;
        n = 1;
        color = 0;
        break;
      case GL_SPOT_CUTOFF:
        value[0] = l->spot_cutoff;
        n = 1;
        color = 0;
        break;
      case GL_CONSTANT_ATTENUATION:
      case GL_LINEAR_ATTENUATION:
      case GL_QUADRATIC_ATTENUATION:
        value[0] = l->attenuation[pname - GL_CONSTANT_ATTENUATION];
        n = 1;
        color = 0;
        break;
      default:
        gl_error(c, GL_INVALID_ENUM);
        return 1;
      }
    } else {
      if (object != GL_FRONT && object != GL_BACK) {
        gl_error(c, GL_INVALID_ENUM);
        return 1;
      }
      GLMaterial *m = &c->tiny.materials[object == GL_BACK];
      switch (pname) {
      case GL_AMBIENT:
        w32_copy(value, m->ambient.v, 16);
        break;
      case GL_DIFFUSE:
        w32_copy(value, m->diffuse.v, 16);
        break;
      case GL_SPECULAR:
        w32_copy(value, m->specular.v, 16);
        break;
      case GL_EMISSION:
        w32_copy(value, m->emission.v, 16);
        break;
      case GL_SHININESS:
        value[0] = m->shininess;
        n = 1;
        color = 0;
        break;
      default:
        gl_error(c, GL_INVALID_ENUM);
        return 1;
      }
    }
    for (int k = 0; k < n; k++)
      if (integer)
        ((int *)a[2].p)[k] =
            (int)(color ? (double)value[k] * 2147483647.0 : value[k]);
      else
        ((float *)a[2].p)[k] = value[k];
    return 1;
  }
  if (id == ID_glPointSize) {
    if (a[0].f <= 0 || a[0].f > 64)
      gl_error(c, GL_INVALID_VALUE);
    else
      c->zb->pointsize = (float)a[0].f;
    return 1;
  }
  if (id == ID_glRenderMode) {
    if (a[0].i != GL_RENDER)
      gl_error(c, GL_INVALID_ENUM);
    result->i = 0;
    return 1;
  }
  return 0;
}
