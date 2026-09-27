/* Ring-0 DBOPL adapter for CVSESSION (GPL-2.0-or-later with DBOPL).
 *
 * Built by clang for i686 COFF, freestanding: no heap, no exceptions, no
 * C/C++ runtime. The single chip lives in static storage and is rebuilt in
 * place. Table setup uses the x87; the caller saves and restores the guest's
 * FPU state around cvop_create and cvop_generate (session_devices.c).
 */
#include "DBOPL.H"

static unsigned char chip_storage[sizeof(DBOPL::Chip)] __attribute__((aligned(16)));
static DBOPL::Chip *chip;

inline void *operator new(unsigned int, void *where) { return where; }

extern "C" void *cvop_create(unsigned rate)
{
    chip = new (chip_storage) DBOPL::Chip(true);
    chip->Setup(rate);
    return chip;
}

extern "C" void cvop_write(void *opaque, unsigned reg, unsigned value)
{
    static_cast<DBOPL::Chip *>(opaque)->WriteReg(reg & 0x1ffu, (unsigned char)value);
}

extern "C" void cvop_generate(void *opaque, short *stereo, unsigned frames)
{
    DBOPL::Chip *c = static_cast<DBOPL::Chip *>(opaque);
    unsigned i;
    c->Generate(stereo, frames);
    if (!c->opl3Active) {
        /* OPL2 writes one mono sample per frame into the first half. */
        for (i = frames; i != 0; --i) {
            short sample = stereo[i - 1];
            stereo[(i - 1) * 2] = sample;
            stereo[(i - 1) * 2 + 1] = sample;
        }
    }
}

extern "C" void *memset(void *destination, int value, unsigned int count)
{
    unsigned char *p = static_cast<unsigned char *>(destination);
    while (count--) *p++ = (unsigned char)value;
    return destination;
}

extern "C" double sin(double x)
{
    double result;
    __asm__("fsin" : "=t"(result) : "0"(x));
    return result;
}

/* y * log2(x), then 2^(integer + fraction) through F2XM1/FSCALE; x > 0. */
extern "C" double pow(double x, double y)
{
    double result;
    if (x == 0.0) return 0.0;
    /* Only the destination-ST(0) FSUB form: AT&T reverses FSUB/FSUBR when
     * ST(i) is the destination, which silently computed 2^-fraction. */
    __asm__("fyl2x\n\t"
            "fld %%st(0)\n\t"
            "frndint\n\t"
            "fxch\n\t"
            "fsub %%st(1), %%st\n\t"
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

extern "C" double log10(double x)
{
    double result;
    __asm__("fldlg2\n\t"
            "fxch\n\t"
            "fyl2x"
            : "=t"(result) : "0"(x));
    return result;
}
