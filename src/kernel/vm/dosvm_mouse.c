/* INT33 state and checked V86 far-call continuation. No host callback pointer.
 * Research: f3-acceptance.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

static void mouse_low(uint32_t *r, uint16_t n) { *r = (*r & 0xFFFF0000u) | n; }
static int32_t mouse_clamp(int64_t n, int32_t lo, int32_t hi) { return n < lo ? lo : n > hi ? hi : (int32_t)n; }
void dosvm_mouse_init(struct dosvm *v)
{
    memset(&v->mouse, 0, sizeof(v->mouse));
    v->mouse.max_x = 639; v->mouse.max_y = 199;
    v->mouse.x = 320; v->mouse.y = 96; v->mouse.visible = -1;
    v->mouse.ratio_x = 8; v->mouse.ratio_y = 16;
    v->mouse.screen_mask = 0xFFFF; v->mouse.cursor_mask = 0x7700;
}
void dosvm_mouse_event(struct dosvm *v, int32_t dx, int32_t dy, unsigned buttons)
{
    struct dosvm_mouse *m = &v->mouse;
    unsigned changed = m->buttons ^ (buttons & 7), event = (dx || dy) ? 1 : 0;
    m->motion_x = mouse_clamp((int64_t)m->motion_x + dx, -32768, 32767);
    m->motion_y = mouse_clamp((int64_t)m->motion_y + dy, -32768, 32767);
    int64_t x = (int64_t)dx * 8 + m->fraction_x, y = (int64_t)dy * 8 + m->fraction_y;
    m->fraction_x = x % m->ratio_x; m->fraction_y = y % m->ratio_y;
    m->x = mouse_clamp((int64_t)m->x + x / m->ratio_x, m->min_x, m->max_x);
    m->y = mouse_clamp((int64_t)m->y + y / m->ratio_y, m->min_y, m->max_y);
    for (unsigned i = 0; i < 3; i++) if (changed & (1u << i)) {
        bool down = buttons & (1u << i);
        event |= 1u << (2 * i + (down ? 1 : 2));
        uint16_t *count = down ? m->pressed : m->released;
        if (count[i] < 0x7FFF) count[i]++;
        (down ? m->press_x : m->release_x)[i] = m->x & ~7u;
        (down ? m->press_y : m->release_y)[i] = m->y & ~7u;
    }
    m->buttons = buttons & 7;
    m->pending_mask |= event & m->mask;
}
int dosvm_mouse_bios(struct dosvm *v, struct v86_frame *f)
{
    struct dosvm_mouse *m = &v->mouse;
    unsigned fn = (uint16_t)f->tf.eax;
    switch (fn) {
    case 0: case 0x21:
        /* A reset inside a callback must still return through its continuation. */
        if (m->active) return -V86_EPERM;
        dosvm_mouse_init(v); mouse_low(&f->tf.eax, 0xFFFF); mouse_low(&f->tf.ebx, 3); return 0;
    case 1: if (m->visible < 0) m->visible++; return 0;
    case 2: if (m->visible > -32768) m->visible--; return 0;
    case 3:
        mouse_low(&f->tf.ebx, m->buttons); mouse_low(&f->tf.ecx, m->x & ~7u); mouse_low(&f->tf.edx, m->y & ~7u); return 0;
    case 4:
        m->x = mouse_clamp((uint16_t)f->tf.ecx & ~7u, m->min_x, m->max_x);
        m->y = mouse_clamp((uint16_t)f->tf.edx & ~7u, m->min_y, m->max_y); return 0;
    case 5: case 6: {
        unsigned button = (uint16_t)f->tf.ebx;
        mouse_low(&f->tf.eax, m->buttons);
        if (button >= 3) { mouse_low(&f->tf.ebx, 0); return 0; }
        uint16_t *count = fn == 5 ? m->pressed : m->released;
        mouse_low(&f->tf.ebx, count[button]); count[button] = 0;
        mouse_low(&f->tf.ecx, (fn == 5 ? m->press_x : m->release_x)[button]);
        mouse_low(&f->tf.edx, (fn == 5 ? m->press_y : m->release_y)[button]); return 0;
    }
    case 7: case 8: {
        uint16_t lo = f->tf.ecx, hi = f->tf.edx;
        lo &= ~7u; hi &= ~7u;
        if (lo > hi) return -V86_EPERM;
        if (fn == 7) { m->min_x = lo; m->max_x = hi; m->x = mouse_clamp(m->x, lo, hi); }
        else { m->min_y = lo; m->max_y = hi; m->y = mouse_clamp(m->y, lo, hi); }
        return 0;
    }
    case 0xA:
        if ((uint16_t)f->tf.ebx > 1) return -V86_EPERM;
        m->text_type = f->tf.ebx; m->screen_mask = f->tf.ecx; m->cursor_mask = f->tf.edx; return 0;
    case 0xB:
        mouse_low(&f->tf.ecx, m->motion_x); mouse_low(&f->tf.edx, m->motion_y);
        m->motion_x = m->motion_y = 0; return 0;
    case 0xC: case 0x14: {
        uint16_t mask = f->tf.ecx, cs = f->es, ip = f->tf.edx;
        if ((mask & ~0x7Fu) || (mask && (f->es > 0xFFFF || !dosvm_range(v, ((uint32_t)cs << 4) + ip, 1, false)))) return -V86_EPERM;
        if (fn == 0x14) { mouse_low(&f->tf.ecx, m->mask); mouse_low(&f->tf.edx, m->callback_ip); f->es = m->callback_cs; }
        m->mask = mask; m->callback_cs = cs; m->callback_ip = ip; m->pending_mask = 0; return 0;
    }
    case 0xF:
        if (!(uint16_t)f->tf.ecx || !(uint16_t)f->tf.edx) return -V86_EPERM;
        m->ratio_x = f->tf.ecx; m->ratio_y = f->tf.edx; m->fraction_x = m->fraction_y = 0; return 0;
    default:
        /* No version/capability claim for the later Microsoft extensions. */
        f->tf.eflags |= V86_CF; return 0;
    }
}
int dosvm_mouse_dispatch(struct dosvm *v, struct v86_frame *f)
{
    struct dosvm_mouse *m = &v->mouse;
    if (m->active || !m->pending_mask || !m->mask || !v->monitor.vif || v->monitor.shadow) return 0;
    if (f->tf.user_ss > 0xFFFF || (uint16_t)f->tf.user_esp < 4 ||
        !dosvm_range(v, ((uint32_t)m->callback_cs << 4) + m->callback_ip, 1, false)) return -EFAULT;
    uint16_t sp = (uint16_t)f->tf.user_esp - 4;
    uint16_t ret[2] = {0x1000,0xF000};
    if (dosvm_write(v, (f->tf.user_ss << 4) + sp, ret, 4)) return -EFAULT;
    m->saved = *f; m->saved_vif = v->monitor.vif; m->active = true;
    mouse_low(&f->tf.user_esp, sp); f->tf.cs = m->callback_cs; f->tf.eip = m->callback_ip;
    mouse_low(&f->tf.eax, m->pending_mask); mouse_low(&f->tf.ebx, m->buttons);
    mouse_low(&f->tf.ecx, m->x & ~7u); mouse_low(&f->tf.edx, m->y & ~7u);
    mouse_low(&f->tf.esi, m->motion_x); mouse_low(&f->tf.edi, m->motion_y);
    m->pending_mask = 0;
    v->monitor.vif = false; v->monitor.state = V86_RUNNING;
    f->tf.eflags &= ~(V86_TF | 0x400u); /* clean callback DF */
    return 1;
}
int dosvm_mouse_return(struct dosvm *v, struct v86_frame *f)
{
    struct dosvm_mouse *m = &v->mouse;
    if (!m->active || f->tf.cs != 0xF000 || f->tf.eip != 0x1002 ||
        f->tf.user_ss != m->saved.tf.user_ss || f->tf.user_esp != m->saved.tf.user_esp) return -V86_EPERM;
    *f = m->saved; m->active = false; v->monitor.vif = m->saved_vif;
    /* Wake from HLT: resume at the instruction following the HLT. */
    v->monitor.state = V86_RUNNING;
    return 0;
}
void dosvm_mouse_text(struct dosvm *v)
{
    struct dosvm_mouse *m = &v->mouse;
    if (m->visible < 0 || m->text_type || m->x >= 640 || m->y >= 200) return;
    unsigned cell = (m->y / 8) * 80 + m->x / 8;
    v->text[cell] = (v->text[cell] & m->screen_mask) ^ m->cursor_mask;
}
