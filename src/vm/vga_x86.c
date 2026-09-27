#include "vga_x86.h"

/* Original CiukiOS implementation. Instruction semantics follow the Intel 64
 * and IA-32 Architectures Software Developer's Manual, Volume 2 (instruction
 * reference) and Volume 1 section 3.4.3 (EFLAGS). Host tests compare it with
 * Unicorn/QEMU execution of the same bytes (scripts/test_vga_x86.py).
 * No 64-bit integer types: OpenWatcom freestanding objects must not import
 * runtime helpers. */

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_DF 0x0400u
#define F_OF 0x0800u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

typedef struct dec {
    cvx_cpu *cpu;
    const cvx_bus *bus;
    cvx_result *res;
    uint32_t ip;
    unsigned len, osize, asize, rep, fault;
    int seg;
    unsigned mod, reg, rm, mem, mseg;
    uint32_t moff;
} dec;

static uint32_t size_mask(unsigned size)
{
    return size == 1 ? 0xffUL : size == 2 ? 0xffffUL : 0xffffffffUL;
}

static uint32_t size_sign(unsigned size)
{
    return 1UL << (size * 8 - 1);
}

static int fetch8(dec *d, uint32_t *value)
{
    uint8_t b;
    uint32_t offset = d->cpu->code32 ? d->ip : (d->ip & 0xffffUL);
    if (d->len >= 15) { d->fault = CVX_TOO_LONG; return 0; }
    if (d->bus->fetch(d->bus->context, d->cpu->seg_base[CVX_CS] + offset, &b)) {
        d->fault = CVX_BUS_FAULT;
        return 0;
    }
    d->res->bytes[d->len++] = b;
    ++d->ip;
    *value = b;
    return 1;
}

static int fetch(dec *d, unsigned size, uint32_t *value)
{
    uint32_t b, v = 0;
    unsigned i;
    for (i = 0; i < size; ++i) {
        if (!fetch8(d, &b)) return 0;
        v |= b << (8 * i);
    }
    *value = v;
    return 1;
}

static uint32_t sext(uint32_t value, unsigned size)
{
    if (size == 1) return (value & 0x80) ? value | 0xffffff00UL : value & 0xff;
    if (size == 2) return (value & 0x8000) ? value | 0xffff0000UL : value & 0xffff;
    return value;
}

static uint32_t get_reg(const cvx_cpu *c, unsigned r, unsigned size)
{
    if (size == 1) return r < 4 ? c->gpr[r] & 0xff : (c->gpr[r - 4] >> 8) & 0xff;
    if (size == 2) return c->gpr[r] & 0xffff;
    return c->gpr[r];
}

static void set_reg(cvx_cpu *c, unsigned r, unsigned size, uint32_t v)
{
    if (size == 1) {
        if (r < 4) c->gpr[r] = (c->gpr[r] & 0xffffff00UL) | (v & 0xff);
        else c->gpr[r - 4] = (c->gpr[r - 4] & 0xffff00ffUL) | ((v & 0xff) << 8);
    } else if (size == 2) c->gpr[r] = (c->gpr[r] & 0xffff0000UL) | (v & 0xffff);
    else c->gpr[r] = v;
}

static uint32_t linear(const dec *d, unsigned seg, uint32_t offset)
{
    if (d->asize == 2) offset &= 0xffff;
    return d->cpu->seg_base[seg] + offset;
}

static int mem_read(dec *d, unsigned seg, uint32_t offset, unsigned size, uint32_t *value)
{
    uint32_t v = 0;
    uint8_t b;
    unsigned i;
    for (i = 0; i < size; ++i) {
        if (d->bus->read(d->bus->context, linear(d, seg, offset + i), &b)) {
            d->fault = CVX_BUS_FAULT;
            return 0;
        }
        v |= (uint32_t)b << (8 * i);
    }
    *value = v;
    return 1;
}

static int mem_write(dec *d, unsigned seg, uint32_t offset, unsigned size, uint32_t value)
{
    unsigned i;
    for (i = 0; i < size; ++i)
        if (d->bus->write(d->bus->context, linear(d, seg, offset + i),
                          (uint8_t)(value >> (8 * i)))) {
            d->fault = CVX_BUS_FAULT;
            return 0;
        }
    return 1;
}

static int decode_modrm(dec *d)
{
    uint32_t b, disp, sib, base, index;
    const cvx_cpu *c = d->cpu;
    unsigned seg = CVX_DS;
    uint32_t ea = 0;
    if (!fetch8(d, &b)) return 0;
    d->mod = (unsigned)(b >> 6);
    d->reg = (unsigned)((b >> 3) & 7);
    d->rm = (unsigned)(b & 7);
    d->mem = d->mod != 3;
    if (!d->mem) return 1;
    if (d->asize == 2) {
        /* Keep this as comparisons rather than a C switch.  The same
         * freestanding object is linked into HDPMI's relocation-free PX
         * payload, where absolute compiler jump tables are not admissible. */
        if (d->rm == 0) ea = c->gpr[CVX_EBX] + c->gpr[CVX_ESI];
        else if (d->rm == 1) ea = c->gpr[CVX_EBX] + c->gpr[CVX_EDI];
        else if (d->rm == 2) { ea = c->gpr[CVX_EBP] + c->gpr[CVX_ESI]; seg = CVX_SS; }
        else if (d->rm == 3) { ea = c->gpr[CVX_EBP] + c->gpr[CVX_EDI]; seg = CVX_SS; }
        else if (d->rm == 4) ea = c->gpr[CVX_ESI];
        else if (d->rm == 5) ea = c->gpr[CVX_EDI];
        else if (d->rm == 6) {
            if (d->mod == 0) {
                if (!fetch(d, 2, &disp)) return 0;
                ea = disp;
            } else {
                ea = c->gpr[CVX_EBP];
                seg = CVX_SS;
            }
        } else ea = c->gpr[CVX_EBX];
        if (d->mod == 1) {
            if (!fetch(d, 1, &disp)) return 0;
            ea += sext(disp, 1);
        } else if (d->mod == 2) {
            if (!fetch(d, 2, &disp)) return 0;
            ea += disp;
        }
        ea &= 0xffff;
    } else {
        if (d->rm == 4) {
            if (!fetch8(d, &sib)) return 0;
            base = sib & 7;
            index = (sib >> 3) & 7;
            if (base == 5 && d->mod == 0) {
                if (!fetch(d, 4, &disp)) return 0;
                ea = disp;
            } else {
                ea = c->gpr[base];
                if (base == CVX_ESP || base == CVX_EBP) seg = CVX_SS;
            }
            if (index != 4) ea += c->gpr[index] << (sib >> 6);
        } else if (d->rm == 5 && d->mod == 0) {
            if (!fetch(d, 4, &disp)) return 0;
            ea = disp;
        } else {
            ea = c->gpr[d->rm];
            if (d->rm == CVX_EBP) seg = CVX_SS;
        }
        if (d->mod == 1) {
            if (!fetch(d, 1, &disp)) return 0;
            ea += sext(disp, 1);
        } else if (d->mod == 2) {
            if (!fetch(d, 4, &disp)) return 0;
            ea += disp;
        }
    }
    d->mseg = d->seg >= 0 ? (unsigned)d->seg : seg;
    d->moff = ea;
    return 1;
}

static int rm_read(dec *d, unsigned size, uint32_t *value)
{
    if (!d->mem) {
        *value = get_reg(d->cpu, d->rm, size);
        return 1;
    }
    return mem_read(d, d->mseg, d->moff, size, value);
}

static int rm_write(dec *d, unsigned size, uint32_t value)
{
    if (!d->mem) {
        set_reg(d->cpu, d->rm, size, value);
        return 1;
    }
    return mem_write(d, d->mseg, d->moff, size, value);
}

static unsigned even_parity(uint32_t v)
{
    v &= 0xff;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

/* Replace SF/ZF/PF (and the bits in other) from result. */
static void result_flags(cvx_cpu *c, uint32_t r, unsigned size, uint32_t other, uint32_t keep)
{
    uint32_t f = c->eflags & ~(F_SF | F_ZF | F_PF | keep);
    r &= size_mask(size);
    if (!r) f |= F_ZF;
    if (r & size_sign(size)) f |= F_SF;
    if (even_parity(r)) f |= F_PF;
    c->eflags = f | other;
}

static uint32_t alu(cvx_cpu *c, unsigned op, uint32_t a, uint32_t b, unsigned size)
{
    uint32_t m = size_mask(size), s = size_sign(size), r = 0, cf = 0, of = 0, af = 0, t;
    uint32_t cin = c->eflags & F_CF;
    a &= m;
    b &= m;
    if (op == 0 || op == 2) {                         /* ADD, ADC */
        if (op == 0) cin = 0;
        if (size == 4) {
            t = a + b;
            r = t + cin;
            cf = (t < a) || (r < t);
        } else {
            t = a + b + cin;
            r = t & m;
            cf = t > m;
        }
        of = (a ^ r) & (b ^ r) & s;
        af = (a ^ b ^ r) & 0x10;
    } else if (op == 3 || op == 5 || op == 7) {       /* SBB, SUB, CMP */
        if (op != 3) cin = 0;
        r = (a - b - cin) & m;
        cf = (a < b) || (cin && a == b);
        of = (a ^ b) & (a ^ r) & s;
        af = (a ^ b ^ r) & 0x10;
    } else if (op == 1) r = a | b;
    else if (op == 4) r = a & b;
    else r = a ^ b;
    result_flags(c, r, size, (cf ? F_CF : 0) | (of ? F_OF : 0) | (af ? F_AF : 0),
                 F_CF | F_OF | F_AF);
    return r & m;
}

static int condition(const cvx_cpu *c, unsigned cc)
{
    uint32_t f = c->eflags;
    int sf_ne_of = !!(f & F_SF) != !!(f & F_OF), r;
    if ((cc >> 1) == 0) r = !!(f & F_OF);
    else if ((cc >> 1) == 1) r = !!(f & F_CF);
    else if ((cc >> 1) == 2) r = !!(f & F_ZF);
    else if ((cc >> 1) == 3) r = (f & (F_CF | F_ZF)) != 0;
    else if ((cc >> 1) == 4) r = !!(f & F_SF);
    else if ((cc >> 1) == 5) r = !!(f & F_PF);
    else if ((cc >> 1) == 6) r = sf_ne_of;
    else r = (f & F_ZF) || sf_ne_of;
    return (cc & 1) ? !r : r;
}

static void mul32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
    uint32_t al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    uint32_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint32_t mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);
    *lo = (ll & 0xffff) | (mid << 16);
    *hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}

static void neg64(uint32_t *lo, uint32_t *hi)
{
    *lo = ~*lo + 1;
    *hi = ~*hi + (*lo == 0);
}

/* Signed 32x32 -> 64. */
static void imul32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
    int negative = ((a ^ b) & 0x80000000UL) != 0;
    if (a & 0x80000000UL) a = 0 - a;
    if (b & 0x80000000UL) b = 0 - b;
    mul32(a, b, lo, hi);
    if (negative) neg64(lo, hi);
}

/* Unsigned (hi:lo) / d with hi < d. */
static uint32_t div64(uint32_t hi, uint32_t lo, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, top;
    int i;
    for (i = 0; i < 32; ++i) {
        top = hi >> 31;
        hi = (hi << 1) | (lo >> 31);
        lo <<= 1;
        q <<= 1;
        if (top || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    *rem = hi;
    return q;
}

/* Two/three-operand IMUL and one-operand F6/F7 /5 share the overflow rule:
 * CF=OF=1 when the full signed product differs from its truncated value. */
static uint32_t imul_trunc(cvx_cpu *c, uint32_t a, uint32_t b, unsigned size, uint32_t *high)
{
    uint32_t lo, hi, m = size_mask(size), overflow;
    a = sext(a & m, size);
    b = sext(b & m, size);
    imul32(a, b, &lo, &hi);
    if (size == 4) {
        overflow = hi != ((lo & 0x80000000UL) ? 0xffffffffUL : 0);
        *high = hi;
    } else {
        overflow = lo != sext(lo & m, size);
        *high = (lo >> (size * 8)) & m;
    }
    result_flags(c, lo, size, overflow ? (F_CF | F_OF) : 0, F_CF | F_OF | F_AF);
    return lo & m;
}

static uint32_t shift(cvx_cpu *c, unsigned op, uint32_t a, unsigned count, unsigned size)
{
    unsigned bits = size * 8, n, i;
    uint32_t m = size_mask(size), s = size_sign(size), r = a & m, cf, of, carry;
    count &= 31;
    if (!count) return r;
    a &= m;
    if (op == 0) {                                           /* ROL */
        n = count % bits;
        if (n) r = ((a << n) | (a >> (bits - n))) & m;
        cf = r & 1;
        of = ((r & s) != 0) ^ cf;
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    } else if (op == 1) {                                    /* ROR */
        n = count % bits;
        if (n) r = ((a >> n) | (a << (bits - n))) & m;
        cf = (r & s) != 0;
        of = cf ^ ((r & (s >> 1)) != 0);
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    } else if (op == 2 || op == 3) {                         /* RCL, RCR */
        n = count % (bits + 1);
        carry = (c->eflags & F_CF) != 0;
        if (op == 2) {
            for (i = 0; i < n; ++i) {
                cf = (r & s) != 0;
                r = ((r << 1) | carry) & m;
                carry = cf;
            }
            of = ((r & s) != 0) ^ carry;
        } else {
            of = ((r & s) != 0) ^ carry;
            for (i = 0; i < n; ++i) {
                cf = r & 1;
                r = (r >> 1) | (carry ? s : 0);
                carry = cf;
            }
        }
        if (!n) return r;
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (carry ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    } else if (op == 4 || op == 6) {                         /* SHL/SAL */
        if (count <= bits) {
            cf = (a >> (bits - count)) & 1;
            r = count == bits ? 0 : (a << count) & m;
        } else {
            cf = 0;
            r = 0;
        }
        of = ((r & s) != 0) ^ cf;
    } else if (op == 5) {                                    /* SHR */
        cf = count <= bits ? (a >> (count - 1)) & 1 : 0;
        r = count >= bits ? 0 : a >> count;
        of = (a & s) != 0;
    } else {                                                 /* SAR */
        if (count >= bits) {
            cf = (a & s) != 0;
            r = cf ? m : 0;
        } else {
            cf = (a >> (count - 1)) & 1;
            r = a >> count;
            if (a & s) r |= (m << (bits - count)) & m;
        }
        of = 0;
    }
    result_flags(c, r, size, (cf ? F_CF : 0) | (of ? F_OF : 0), F_CF | F_OF | F_AF);
    return r;
}

static uint32_t index_reg(const dec *d, unsigned r)
{
    uint32_t v = d->cpu->gpr[r];
    return d->asize == 2 ? v & 0xffff : v;
}

static void advance_index(dec *d, unsigned r, unsigned size)
{
    uint32_t v = d->cpu->gpr[r];
    uint32_t delta = (d->cpu->eflags & F_DF) ? 0 - (uint32_t)size : size;
    if (d->asize == 2) d->cpu->gpr[r] = (v & 0xffff0000UL) | ((v + delta) & 0xffff);
    else d->cpu->gpr[r] = v + delta;
}

static int string_op(dec *d, unsigned opcode, uint32_t budget)
{
    cvx_cpu *c = d->cpu;
    unsigned size = (opcode & 1) ? d->osize : 1, kind = opcode & 0xfe;
    unsigned source = d->seg >= 0 ? (unsigned)d->seg : CVX_DS;
    uint32_t count = 0, a, b, m = size_mask(size);
    if (d->rep) {
        count = index_reg(d, CVX_ECX);
        if (!count) return CVX_DONE;
    }
    for (;;) {
        if (kind == 0xa4) {
            if (!mem_read(d, source, index_reg(d, CVX_ESI), size, &a) ||
                !mem_write(d, CVX_ES, index_reg(d, CVX_EDI), size, a)) return d->fault;
            advance_index(d, CVX_ESI, size);
            advance_index(d, CVX_EDI, size);
        } else if (kind == 0xa6) {
            if (!mem_read(d, source, index_reg(d, CVX_ESI), size, &a) ||
                !mem_read(d, CVX_ES, index_reg(d, CVX_EDI), size, &b)) return d->fault;
            alu(c, 7, a, b, size);
            advance_index(d, CVX_ESI, size);
            advance_index(d, CVX_EDI, size);
        } else if (kind == 0xaa) {
            if (!mem_write(d, CVX_ES, index_reg(d, CVX_EDI), size, c->gpr[CVX_EAX] & m))
                return d->fault;
            advance_index(d, CVX_EDI, size);
        } else if (kind == 0xac) {
            if (!mem_read(d, source, index_reg(d, CVX_ESI), size, &a)) return d->fault;
            set_reg(c, CVX_EAX, size, a);
            advance_index(d, CVX_ESI, size);
        } else {                                             /* 0xae SCAS */
            if (!mem_read(d, CVX_ES, index_reg(d, CVX_EDI), size, &b)) return d->fault;
            alu(c, 7, c->gpr[CVX_EAX], b, size);
            advance_index(d, CVX_EDI, size);
        }
        ++d->res->elements;
        if (!d->rep) return CVX_DONE;
        --count;
        if (d->asize == 2) c->gpr[CVX_ECX] = (c->gpr[CVX_ECX] & 0xffff0000UL) | (count & 0xffff);
        else c->gpr[CVX_ECX] = count;
        if (!count) return CVX_DONE;
        if (kind == 0xa6 || kind == 0xae) {
            if (d->rep == 0xf3 && !(c->eflags & F_ZF)) return CVX_DONE;
            if (d->rep == 0xf2 && (c->eflags & F_ZF)) return CVX_DONE;
        }
        if (d->res->elements >= budget) return CVX_PARTIAL;
    }
}

/* F6/F7 /4-/7. */
static int muldiv(dec *d, unsigned size, uint32_t src)
{
    cvx_cpu *c = d->cpu;
    unsigned op = d->reg;
    uint32_t m = size_mask(size), lo, hi, q, r, a, dividend_lo, dividend_hi;
    int negative_q, negative_r;
    if (op == 4 || op == 5) {
        if (op == 4) {
            a = get_reg(c, CVX_EAX, size);
            if (size == 4) mul32(a, src, &lo, &hi);
            else {
                lo = (a & m) * (src & m);
                hi = (lo >> (size * 8)) & m;
            }
            result_flags(c, lo, size, hi ? (F_CF | F_OF) : 0, F_CF | F_OF | F_AF);
            lo &= m;
        } else lo = imul_trunc(c, get_reg(c, CVX_EAX, size), src, size, &hi);
        if (size == 1) set_reg(c, CVX_EAX, 2, (hi << 8) | lo);
        else {
            set_reg(c, CVX_EAX, size, lo);
            set_reg(c, CVX_EDX, size, hi);
        }
        return CVX_DONE;
    }
    src &= m;
    if (!src) return CVX_DIVIDE_ERROR;
    if (size == 1) {
        dividend_hi = 0;
        dividend_lo = c->gpr[CVX_EAX] & 0xffff;
    } else if (size == 2) {
        dividend_hi = 0;
        dividend_lo = ((c->gpr[CVX_EDX] & 0xffff) << 16) | (c->gpr[CVX_EAX] & 0xffff);
    } else {
        dividend_hi = c->gpr[CVX_EDX];
        dividend_lo = c->gpr[CVX_EAX];
    }
    if (op == 6) {
        if (size == 4) {
            if (dividend_hi >= src) return CVX_DIVIDE_ERROR;
            q = div64(dividend_hi, dividend_lo, src, &r);
        } else {
            q = dividend_lo / src;
            r = dividend_lo % src;
            if (q > m) return CVX_DIVIDE_ERROR;
        }
    } else {
        if (size != 4) {
            dividend_lo = sext(dividend_lo, size * 2);
            dividend_hi = (dividend_lo & 0x80000000UL) ? 0xffffffffUL : 0;
        }
        src = sext(src, size);
        negative_r = (dividend_hi & 0x80000000UL) != 0;
        negative_q = negative_r != ((src & 0x80000000UL) != 0);
        if (negative_r) neg64(&dividend_lo, &dividend_hi);
        if (src & 0x80000000UL) src = 0 - src;
        if (dividend_hi >= src) return CVX_DIVIDE_ERROR;
        q = div64(dividend_hi, dividend_lo, src, &r);
        if (negative_q ? q > size_sign(size) : q >= size_sign(size)) return CVX_DIVIDE_ERROR;
        if (negative_q) q = 0 - q;
        if (negative_r) r = 0 - r;
    }
    if (size == 1) set_reg(c, CVX_EAX, 2, ((r & 0xff) << 8) | (q & 0xff));
    else {
        set_reg(c, CVX_EAX, size, q);
        set_reg(c, CVX_EDX, size, r);
    }
    return CVX_DONE;
}

int cvx_execute(cvx_cpu *cpu, const cvx_bus *bus, uint32_t budget, cvx_result *result)
{
    dec d;
    cvx_cpu saved = *cpu;
    uint32_t op, b, imm, a, value, high;
    unsigned size, i;
    int status = CVX_UNSUPPORTED;
    for (i = 0; i < sizeof(result->bytes); ++i) result->bytes[i] = 0;
    result->length = 0;
    result->elements = 0;
    if (!budget) budget = 1;
    d.cpu = cpu;
    d.bus = bus;
    d.res = result;
    d.ip = cpu->eip;
    d.len = 0;
    d.fault = 0;
    d.seg = -1;
    d.rep = 0;
    d.osize = d.asize = cpu->code32 ? 4 : 2;
    d.mem = 0;
    for (;;) {
        if (!fetch8(&d, &op)) return d.fault;
        switch (op) {
        case 0x26: d.seg = CVX_ES; continue;
        case 0x2e: d.seg = CVX_CS; continue;
        case 0x36: d.seg = CVX_SS; continue;
        case 0x3e: d.seg = CVX_DS; continue;
        case 0x64: d.seg = CVX_FS; continue;
        case 0x65: d.seg = CVX_GS; continue;
        case 0x66: d.osize = cpu->code32 ? 2 : 4; continue;
        case 0x67: d.asize = cpu->code32 ? 2 : 4; continue;
        case 0xf0: continue;
        case 0xf2: case 0xf3: d.rep = (unsigned)op; continue;
        default: break;
        }
        break;
    }
    /* Decode and execute. Every UNSUPPORTED exit precedes the first memory
     * or register side effect, so the caller may reflect a fault safely. */
    if (op < 0x40 && (op & 7) < 4) {                            /* ALU forms */
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (!rm_read(&d, size, &a)) goto fault;
        b = get_reg(cpu, d.reg, size);
        if (op & 2) {
            value = alu(cpu, (unsigned)(op >> 3), b, a, size);
            if ((op >> 3) != 7) set_reg(cpu, d.reg, size, value);
        } else {
            value = alu(cpu, (unsigned)(op >> 3), a, b, size);
            if ((op >> 3) != 7 && !rm_write(&d, size, value)) goto fault;
        }
        status = CVX_DONE;
    } else if (op >= 0x80 && op <= 0x83) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (!fetch(&d, op == 0x81 ? size : 1, &imm)) return d.fault;
        if (op == 0x83) imm = sext(imm, 1);
        if (!rm_read(&d, size, &a)) goto fault;
        value = alu(cpu, d.reg, a, imm, size);
        if (d.reg != 7 && !rm_write(&d, size, value)) goto fault;
        status = CVX_DONE;
    } else if (op == 0x84 || op == 0x85) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (!rm_read(&d, size, &a)) goto fault;
        alu(cpu, 4, a, get_reg(cpu, d.reg, size), size);
        status = CVX_DONE;
    } else if (op == 0x86 || op == 0x87) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (!rm_read(&d, size, &a)) goto fault;
        if (!rm_write(&d, size, get_reg(cpu, d.reg, size))) goto fault;
        set_reg(cpu, d.reg, size, a);
        status = CVX_DONE;
    } else if (op >= 0x88 && op <= 0x8b) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (op & 2) {
            if (!rm_read(&d, size, &a)) goto fault;
            set_reg(cpu, d.reg, size, a);
        } else if (!rm_write(&d, size, get_reg(cpu, d.reg, size))) goto fault;
        status = CVX_DONE;
    } else if (op == 0x8c) {
        if (!decode_modrm(&d)) return d.fault;
        if (d.reg > CVX_GS) return CVX_UNSUPPORTED;
        /* Memory stores are always 16 bits; register forms zero-extend. */
        if (d.mem) {
            if (!mem_write(&d, d.mseg, d.moff, 2, cpu->sreg[d.reg])) goto fault;
        } else set_reg(cpu, d.rm, d.osize, cpu->sreg[d.reg]);
        status = CVX_DONE;
    } else if (op >= 0xa0 && op <= 0xa3) {
        size = (op & 1) ? d.osize : 1;
        if (!fetch(&d, d.asize, &imm)) return d.fault;
        a = d.seg >= 0 ? (unsigned)d.seg : CVX_DS;
        if (op & 2) {
            if (!mem_write(&d, (unsigned)a, imm, size, get_reg(cpu, CVX_EAX, size))) goto fault;
        } else {
            if (!mem_read(&d, (unsigned)a, imm, size, &value)) goto fault;
            set_reg(cpu, CVX_EAX, size, value);
        }
        status = CVX_DONE;
    } else if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {
        status = string_op(&d, (unsigned)op, budget);
        if (status == CVX_BUS_FAULT) goto fault;
    } else if (op == 0xc6 || op == 0xc7) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (d.reg) return CVX_UNSUPPORTED;
        if (!fetch(&d, size, &imm)) return d.fault;
        if (!rm_write(&d, size, imm)) goto fault;
        status = CVX_DONE;
    } else if (op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (op <= 0xc1) {
            if (!fetch(&d, 1, &imm)) return d.fault;
        } else imm = op <= 0xd1 ? 1 : cpu->gpr[CVX_ECX] & 0xff;
        if (!rm_read(&d, size, &a)) goto fault;
        value = shift(cpu, d.reg, a, (unsigned)imm, size);
        if ((imm & 31) && !rm_write(&d, size, value)) goto fault;
        status = CVX_DONE;
    } else if (op == 0xd7) {
        a = d.seg >= 0 ? (unsigned)d.seg : CVX_DS;
        if (!mem_read(&d, (unsigned)a, index_reg(&d, CVX_EBX) + (cpu->gpr[CVX_EAX] & 0xff), 1, &value))
            goto fault;
        set_reg(cpu, CVX_EAX, 1, value);
        status = CVX_DONE;
    } else if (op == 0xf6 || op == 0xf7) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (d.reg < 2 && !fetch(&d, size, &imm)) return d.fault;
        if (!rm_read(&d, size, &a)) goto fault;
        switch (d.reg) {
        case 0: case 1: alu(cpu, 4, a, imm, size); status = CVX_DONE; break;
        case 2: status = rm_write(&d, size, ~a) ? CVX_DONE : CVX_BUS_FAULT; break;
        case 3:
            value = alu(cpu, 5, 0, a, size);
            if (a & size_mask(size)) cpu->eflags |= F_CF; else cpu->eflags &= ~F_CF;
            status = rm_write(&d, size, value) ? CVX_DONE : CVX_BUS_FAULT;
            break;
        default: status = muldiv(&d, size, a); break;
        }
        if (status == CVX_BUS_FAULT) goto fault;
    } else if (op == 0xfe || op == 0xff) {
        size = (op & 1) ? d.osize : 1;
        if (!decode_modrm(&d)) return d.fault;
        if (d.reg > 1) return CVX_UNSUPPORTED;
        if (!rm_read(&d, size, &a)) goto fault;
        b = cpu->eflags & F_CF;
        value = alu(cpu, d.reg ? 5 : 0, a, 1, size);
        cpu->eflags = (cpu->eflags & ~F_CF) | b;
        if (!rm_write(&d, size, value)) goto fault;
        status = CVX_DONE;
    } else if (op == 0x69 || op == 0x6b) {
        size = d.osize;
        if (!decode_modrm(&d)) return d.fault;
        if (!fetch(&d, op == 0x69 ? size : 1, &imm)) return d.fault;
        if (!rm_read(&d, size, &a)) goto fault;
        set_reg(cpu, d.reg, size,
                imul_trunc(cpu, a, op == 0x6b ? sext(imm, 1) : imm, size, &high));
        status = CVX_DONE;
    } else if (op == 0x0f) {
        if (!fetch8(&d, &op)) return d.fault;
        if (op == 0xb6 || op == 0xb7 || op == 0xbe || op == 0xbf) {
            size = (op & 1) ? 2 : 1;
            if (!decode_modrm(&d)) return d.fault;
            if (!rm_read(&d, size, &a)) goto fault;
            set_reg(cpu, d.reg, d.osize, op >= 0xbe ? sext(a, size) : a);
            status = CVX_DONE;
        } else if (op >= 0x90 && op <= 0x9f) {
            if (!decode_modrm(&d)) return d.fault;
            if (!rm_write(&d, 1, (uint32_t)condition(cpu, (unsigned)(op & 15)))) goto fault;
            status = CVX_DONE;
        } else if (op == 0xaf) {
            size = d.osize;
            if (!decode_modrm(&d)) return d.fault;
            if (!rm_read(&d, size, &a)) goto fault;
            set_reg(cpu, d.reg, size, imul_trunc(cpu, get_reg(cpu, d.reg, size), a, size, &high));
            status = CVX_DONE;
        } else return CVX_UNSUPPORTED;
    } else return CVX_UNSUPPORTED;
    if (status == CVX_DIVIDE_ERROR || status == CVX_UNSUPPORTED) {
        *cpu = saved;
        return status;
    }
    result->length = d.len;
    if (!result->elements) result->elements = 1;
    if (status == CVX_DONE) cpu->eip = cpu->code32 ? d.ip : (cpu->eip & 0xffff0000UL) | (d.ip & 0xffff);
    return status;
fault:
    result->length = d.len;
    return CVX_BUS_FAULT;
}
