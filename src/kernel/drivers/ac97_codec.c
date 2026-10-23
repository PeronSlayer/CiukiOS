/* Codec identities and bounded stereo routing adapted from Linux v6.12:
 * sound/pci/ac97/ac97_codec.c and sound/pci/ac97/ac97_patch.c
 * Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 * Universal interface for Audio Codec '97
 * Upstream SPDX-License-Identifier: GPL-2.0-or-later
 * Ciuki modifications: private masks, copied snapshots, bounded transport
 * callbacks and restore, no ALSA framework. See docs/licenses/linux-ac97-NOTICES.txt.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/ac97_codec.h>
#define CODEC(id, mask, name, master, pcm, hp, q) {id,mask,name,master,pcm,hp,q,0,0,true}
#define STANDARD(id, name, q) CODEC(id,UINT32_MAX,name,0x9f1f,0x9f1f,0x9f1f,q)
static const struct ac97_codec_profile codec_profiles[] = {
    STANDARD(0x41445340,"AD1881",0), STANDARD(0x41445348,"AD1881A",0),
    {0x41445360,UINT32_MAX,"AD1885",0x9f1f,0x9f1f,0xbf3f,AC97_Q_AD1885,0,0x0404,true},
    {0x41445361,UINT32_MAX,"AD1886",0x9f1f,0x9f1f,0xbf3f,AC97_Q_AD1886,0,0x0404,true},
    STANDARD(0x41445363,"AD1886A",0),
    CODEC(0x41445370,UINT32_MAX,"AD1980",0x9f9f,0x9f9f,0x9f9f,AC97_Q_AD_FRONT),
    CODEC(0x41445372,UINT32_MAX,"AD1981A",0x9f9f,0x9f9f,0x9f9f,AC97_Q_AD_SPLIT),
    CODEC(0x41445374,UINT32_MAX,"AD1981B",0x9f9f,0x9f9f,0x9f9f,AC97_Q_AD_SPLIT),
    CODEC(0x41445375,UINT32_MAX,"AD1985",0x9f9f,0x9f9f,0x9f9f,AC97_Q_AD_FRONT),
    CODEC(0x414c4721,UINT32_MAX,"ALC650D",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC650),
    CODEC(0x414c4722,UINT32_MAX,"ALC650E",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC650),
    CODEC(0x414c4723,UINT32_MAX,"ALC650F",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC650),
    CODEC(0x414c4720,0xfffffff0,"ALC650",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC650),
    CODEC(0x414c4760,0xfffffff0,"ALC655",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC655),
    CODEC(0x414c4781,UINT32_MAX,"ALC658D",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC658),
    CODEC(0x414c4780,0xfffffff0,"ALC658",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC658),
    CODEC(0x414c4790,0xfffffff0,"ALC850",0x9f1f,0x9f1f,0x9f1f,AC97_Q_ALC850),
    CODEC(0x434d4941,UINT32_MAX,"CMI9738",0x9f1f,0,0x9f1f,AC97_Q_CM9738),
    CODEC(0x434d4961,UINT32_MAX,"CMI9739",0,0,0,AC97_Q_CM9739),
    CODEC(0x43525910,0xfffffff8,"CS4297A",0x9f1f,0x9f1f,0x9f1f,0),
    CODEC(0x43525930,0xfffffff8,"CS4299",0x9f1f,0x9f1f,0x9f1f,AC97_Q_CIRRUS),
    STANDARD(0x49434551,"VT1616",AC97_Q_VENDOR_JACK),
    STANDARD(0x49434552,"VT1616i",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847600,"STAC9700/83/84",0),
    STANDARD(0x83847604,"STAC9701/3/4/5",0),
    STANDARD(0x83847605,"STAC9704",0),
    STANDARD(0x83847608,"STAC9708/11",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847609,"STAC9721/23",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847644,"STAC9744",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847650,"STAC9750/51",0),
    STANDARD(0x83847652,"STAC9752/53",0),
    STANDARD(0x83847656,"STAC9756/57",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847658,"STAC9758/59",AC97_Q_VENDOR_JACK),
    STANDARD(0x83847666,"STAC9766/67",0),
    STANDARD(0x574d4c00,"WM9701/9701A",0),
    STANDARD(0x574d4c03,"WM9703/7/8/9717",0),
    STANDARD(0x574d4c04,"WM9704M/Q",AC97_Q_VENDOR_JACK),
    STANDARD(0x574d4c05,"WM9705/9710",0),
    STANDARD(0x574d4c09,"WM9709",0),
    STANDARD(0x574d4c12,"WM9711/12/15",AC97_Q_SPECIAL_MIXER),
    STANDARD(0x574d4c13,"WM9713/14",AC97_Q_SPECIAL_MIXER)
};
static const struct ac97_codec_profile generic_codec = STANDARD(0,"Generic AC'97",0);
#undef STANDARD
#undef CODEC
const struct ac97_codec_profile *ac97_codec_lookup(uint32_t id)
{
    for (unsigned i=0;i<sizeof(codec_profiles)/sizeof(codec_profiles[0]);i++)
        if ((id & codec_profiles[i].id_mask)==codec_profiles[i].id) return &codec_profiles[i];
    return &generic_codec;
}
void ac97_codec_policy_init(struct ac97_codec_policy *s, const struct pci_func *p, uint32_t id, uint16_t caps, uint16_t ext)
{
    memset(s,0,sizeof(*s)); s->profile=ac97_codec_lookup(id); s->caps=caps; s->ext_id=ext;
    /* Linux intel8x0.c ac97 quirks: X31 only, never all IBM machines/T23. */
    s->eapd=p->vendor==0x8086 && p->sub_vendor==0x1014 && p->sub_device==0x0534 ? 0x8000 : s->profile->eapd_enable;
}
static int codec_patch(struct ac97_codec_policy *s, const struct ac97_codec_ops *io, uint16_t reg, uint16_t mask, uint16_t value)
{
    if (s->saved_count==AC97_CODEC_PATCHES) return -EIO;
    uint16_t old; int e=io->read(io->cookie,reg,&old); if (e) return e;
    unsigned n=s->saved_count++;
    s->saved[n].reg=reg; s->saved[n].value=old; s->saved[n].mask=mask;
    /* Save before the first attempted write: even a failed posted write may land. */
    return io->update(io->cookie,reg,(old & ~mask) | (value & mask),mask);
}
int ac97_codec_unpatch(struct ac97_codec_policy *s, const struct ac97_codec_ops *io)
{
    while (s->saved_count) {
        unsigned n=s->saved_count-1;
        int e=io->update(io->cookie,s->saved[n].reg,s->saved[n].value,s->saved[n].mask);
        if (e) return e;
        s->saved_count--;
    }
    return 0;
}
int ac97_codec_quirks(struct ac97_codec_policy *s, const struct pci_func *p, const struct ac97_codec_ops *io)
{
    unsigned q=s->profile->quirks;
    if (q & AC97_Q_SPECIAL_MIXER) return -EOPNOTSUPP;
    int e=0;
    if (q & AC97_Q_AD1885) e=codec_patch(s,io,0x76,0x0404,0x0404);
    if (q & AC97_Q_AD1886) e=codec_patch(s,io,0x72,0x0010,0x0010);
    if (q & (AC97_Q_AD_FRONT|AC97_Q_AD_SPLIT)) {
        uint16_t mask=0x2000, value=0x2000; /* independent stereo mutes */
        if (q & AC97_Q_AD_FRONT) {
            mask|=0x4420; value|=0x4000;
            if (!(p->sub_vendor==0x1043 && p->sub_device==0x1193)) value|=0x0420;
        }
        e=codec_patch(s,io,0x76,mask,value);
    }
    if (!e && (q & (AC97_Q_ALC655|AC97_Q_ALC658|AC97_Q_ALC850)))
        e=codec_patch(s,io,0x24,0x000f,0); /* vendor page; restore last */
    if (!e && (q & (AC97_Q_ALC650|AC97_Q_ALC655|AC97_Q_ALC658|AC97_Q_ALC850)))
        e=codec_patch(s,io,0x6a,0x7e00,0); /* stereo; no jack retasking/duplicate front */
    if (!e && (q & AC97_Q_ALC655) && p->sub_vendor==0x1462 &&
        (p->sub_device==0x0131 || p->sub_device==0x0161 || p->sub_device==0x0351 ||
         p->sub_device==0x0471 || p->sub_device==0x0061))
        e=codec_patch(s,io,0x7a,0x0002,0); /* pin47 EAPD, not SPDIF input */
    if (!e && (q & AC97_Q_ALC850)) {
        e=codec_patch(s,io,0x7a,0x30f2,0x20d2); /* upstream front amp/mix/Vref */
        if (!e) e=codec_patch(s,io,0x76,0x9b9f,0x8a90); /* front output jack */
    }
    if (!e && (q & AC97_Q_CM9738)) e=codec_patch(s,io,0x18,0x8000,0x8000);
    if (!e && (q & AC97_Q_CM9739)) {
        e=codec_patch(s,io,0x02,0x8000,0x8000); /* registers react but do not control volume */
        if (!e) e=codec_patch(s,io,0x18,0x8000,0x8000);
        if (!e) e=codec_patch(s,io,0x64,0x7629,0x6008); /* EAPD; bit4 revision preserved */
        if (!e) e=codec_patch(s,io,0x70,UINT16_MAX,0x0100);
        if (!e) e=codec_patch(s,io,0x72,UINT16_MAX,0x0020);
    }
    return e;
}
