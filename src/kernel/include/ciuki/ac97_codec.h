/* Private AC-link policy; public device/audio ABI is unchanged.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AC97_CODEC_H
#define CIUKI_AC97_CODEC_H
#include <ciuki/registry.h>
enum ac97_codec_quirk {
    AC97_Q_AD1885=1, AC97_Q_AD1886=2, AC97_Q_AD_FRONT=4,
    AC97_Q_AD_SPLIT=8, AC97_Q_ALC650=16, AC97_Q_ALC655=32,
    AC97_Q_ALC658=64, AC97_Q_ALC850=128, AC97_Q_CM9738=256,
    AC97_Q_CM9739=512, AC97_Q_CIRRUS=1024, AC97_Q_SPECIAL_MIXER=2048,
    AC97_Q_VENDOR_JACK=4096
};
struct ac97_codec_profile {
    uint32_t id, id_mask;
    const char *name;
    uint16_t master_mask, pcm_mask, headphone_mask, quirks;
    uint16_t eapd_enable; /* standard EAPD=0; exact board overrides below policy_init */
    uint16_t headphone_unity; /* AD1885/1886: code 4 is 0 dB, code 0 is +6 dB */
    /* VRA is optional even for a named codec: probe 28h and verify 2Ah. */
    bool probe_vra;
};
#define AC97_CODEC_PATCHES 8u
struct ac97_codec_policy {
    const struct ac97_codec_profile *profile;
    uint16_t caps, ext_id, eapd;
    bool variable_rate;
    unsigned saved_count;
    struct { uint16_t reg, value, mask; } saved[AC97_CODEC_PATCHES];
};
struct ac97_codec_ops {
    void *cookie;
    int (*read)(void *, uint16_t, uint16_t *);
    int (*update)(void *, uint16_t, uint16_t, uint16_t);
};
const struct ac97_codec_profile *ac97_codec_lookup(uint32_t id);
void ac97_codec_policy_init(struct ac97_codec_policy *, const struct pci_func *, uint32_t id, uint16_t caps, uint16_t ext);
int ac97_codec_quirks(struct ac97_codec_policy *, const struct pci_func *, const struct ac97_codec_ops *);
int ac97_codec_unpatch(struct ac97_codec_policy *, const struct ac97_codec_ops *);
#endif
