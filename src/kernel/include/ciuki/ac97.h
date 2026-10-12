/* Intel ICH transport, mockable register boundary. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AC97_H
#define CIUKI_AC97_H
#include <ciuki/audio.h>
#include <ciuki/registry.h>
#define AC97_SLOTS 32u
#define AC97_DCH 1u
#define AC97_CELV 2u
#define AC97_LVBCI 4u
#define AC97_BCIS 8u
#define AC97_FIFOE 16u
struct ac97_bd { uint32_t address, length; };
struct ac97_io {
    void *cookie;
    uint32_t (*read)(void *, uint16_t, unsigned width);
    void (*write)(void *, uint16_t, uint32_t, unsigned width);
    uint64_t (*now)(void *);
    void (*sleep)(void *, uint32_t ms);
};
struct ac97_cursor { uint8_t civ, piv; uint16_t status, remaining; };
struct ac97_device {
    struct ac97_io io;
    struct pci_func pci;
    uint16_t nam, nabm, nam_bytes, nabm_bytes, command, codec_caps, pcm_unity;
    uint32_t bdl_phys, payload_pages[8], generation, codec_id;
    struct ac97_bd *bdl;
    struct ac97_cursor irq_cursor;
    struct audio_frame *pcm[AC97_SLOTS];
    int claims[12];
    gen_t claim_generation[12];
    uint64_t retired, published, frame_base, fetched, generation_fetched, last_progress;
    uint64_t activation_deadline;
    uint64_t fifo_errors, dma_halts, deadline_misses, irqs;
    uint32_t global_status;
    uint16_t power, master, pcm_volume, rate;
    struct ac97_cursor cursor;
    bool ready, running, stopping, quarantine, irq_attached, touched;
    bool observed_halt, codec_changed, software_master, beep_present, cirrus_spdif;
    bool idle_tail; /* no started source: silence cushion, halts not counted */
    bool tail_drained; /* progress proved DCH at the fully fetched last period */
    uint16_t saved_master, saved_pcm, saved_headphone, saved_ext, saved_rate, saved_power, saved_beep, saved_spdif;
};
extern struct ac97_device ac97;
int ac97_init(void);
int ac97_codec_read(struct ac97_device *, uint16_t reg, uint16_t *value, uint64_t deadline);
int ac97_codec_write(struct ac97_device *, uint16_t reg, uint16_t value, uint64_t deadline);
int ac97_codec_prepare(struct ac97_device *);
int ac97_codec_restore(struct ac97_device *);
int ac97_controller_reset(struct ac97_device *);
int ac97_set_master(struct ac97_device *, const struct ciuki_mixer_master *);
int ac97_set_mute(struct ac97_device *, bool mute);
int ac97_observe(struct ac97_device *, struct ac97_cursor *);
int ac97_progress(struct ac97_device *);
int ac97_publish(struct ac97_device *, const struct audio_frame *);
int ac97_start(struct ac97_device *);
int ac97_pause(struct ac97_device *);
int ac97_stop(struct ac97_device *);
void ac97_shutdown(void);
#endif
