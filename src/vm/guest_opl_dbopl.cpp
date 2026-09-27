#include "guest_opl_dbopl.h"

#include <new>
#include "DBOPL.H"

struct cvgp_dbopl {
    DBOPL::Chip *chip;
    unsigned rate;
};

extern "C" cvgp_dbopl *cvgp_dbopl_create(unsigned sample_rate)
{
    cvgp_dbopl *result;
    if (!sample_rate) return 0;
    result = new (std::nothrow) cvgp_dbopl;
    if (!result) return 0;
    result->chip = new (std::nothrow) DBOPL::Chip(true);
    if (!result->chip) { delete result; return 0; }
    result->rate = sample_rate;
    result->chip->Setup(sample_rate);
    return result;
}

extern "C" void cvgp_dbopl_destroy(cvgp_dbopl *opl)
{
    if (!opl) return;
    delete opl->chip;
    delete opl;
}

extern "C" void cvgp_dbopl_reset(void *opaque, unsigned sample_rate)
{
    cvgp_dbopl *opl = static_cast<cvgp_dbopl *>(opaque);
    DBOPL::Chip *replacement;
    if (!opl || !sample_rate) return;
    replacement = new (std::nothrow) DBOPL::Chip(true);
    if (!replacement) return;
    replacement->Setup(sample_rate);
    delete opl->chip;
    opl->chip = replacement;
    opl->rate = sample_rate;
}

extern "C" void cvgp_dbopl_write(void *opaque, uint16_t reg, uint8_t value)
{
    cvgp_dbopl *opl = static_cast<cvgp_dbopl *>(opaque);
    if (opl && opl->chip) opl->chip->WriteReg(reg & 0x1ffu, value);
}

extern "C" int cvgp_dbopl_generate(void *opaque, int16_t *stereo,
                                    unsigned frames)
{
    cvgp_dbopl *opl = static_cast<cvgp_dbopl *>(opaque);
    unsigned i;
    if (!opl || !opl->chip || (!stereo && frames)) return 0;
    opl->chip->Generate(stereo, frames);
    if (!opl->chip->opl3Active) {
        /* DBOPL's OPL2 path writes one mono sample per frame into the first
         * half of the caller's 2*frames workspace.  Expand backwards so the
         * scheduler always receives the promised interleaved stereo shape. */
        for (i = frames; i != 0; --i) {
            int16_t sample = stereo[i - 1];
            stereo[(i - 1) * 2] = sample;
            stereo[(i - 1) * 2 + 1] = sample;
        }
    }
    return 1;
}
