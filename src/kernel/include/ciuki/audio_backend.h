/* Private F4 transport interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_AUDIO_BACKEND_H
#define CIUKI_AUDIO_BACKEND_H
#include <ciuki/ac97.h>
#define AUDIO_HW_SLOTS 32u
#define AUDIO_HW_BYTES (AUDIO_HW_SLOTS * AUDIO_QUANTUM * sizeof(struct audio_frame))
enum audio_backend_kind { AUDIO_ICH=1, AUDIO_ES1370, AUDIO_ES1371, AUDIO_SB16, AUDIO_VIA686, AUDIO_VIA8233 };
struct audio_transport {
    uint64_t published, retired, fetched, generation_fetched, frame_base;
    uint64_t fifo_errors, dma_halts, deadline_misses, irqs;
    uint32_t codec_id, rate;
    uint16_t power, master, pcm_volume, remaining, status;
    uint8_t slot, irq, prefetch, lvi;
    bool ready, running, quarantine, idle_tail, tail_drained;
};
struct audio_dma_device;
struct audio_dma_ops {
    int (*prepare)(struct audio_dma_device *);
    int (*start)(struct audio_dma_device *, bool proof);
    int (*halt)(struct audio_dma_device *);
    int (*cursor)(struct audio_dma_device *, unsigned *frame);
    int (*mute)(struct audio_dma_device *, bool);
    bool (*ack)(struct audio_dma_device *);
    void (*inhibit)(struct audio_dma_device *);
};
/* Fixed cyclic transport; IRQ reads/acks only its source, inhibits it on
 * exhaustion. Thread context alone publishes, converts and retires buffers. */
struct audio_dma_device {
    struct audio_transport state;
    struct kmutex register_lock; /* whole DSP/codec transactions, thread context */
    struct ac97_io io;
    struct pci_func pci;
    const struct audio_dma_ops *ops;
    const char *name, *reason;
    enum audio_backend_kind kind;
    uint16_t base, bytes, command, codec_saved[8];
    uint32_t phys, desc_phys, control, serial, saved_link, saved_legacy;
    struct audio_frame *pcm;
    struct ac97_bd *desc;
    int claims[8];
    gen_t generations[8];
    uint64_t last_observation, last_progress, activation_deadline;
    unsigned last_frame, channel, codec_saved_count;
    bool touched, irq_attached, proving, inhibited, codec_changed;
    bool link_changed, legacy_changed;
    uint8_t ak[26], dsp_major, dsp_minor, saved_mixer[6];
};
extern struct audio_dma_device audio_dma;
int sb16_restore(struct audio_dma_device *);
extern const struct audio_dma_ops es137x_ops, sb16_ops, via82xx_ops;
bool es137x_match(const struct pci_func *, enum audio_backend_kind *);
bool via82xx_match(const struct pci_func *, enum audio_backend_kind *);
int es137x_codec_read(struct audio_dma_device *, uint16_t, uint16_t *);
int es137x_codec_write(struct audio_dma_device *, uint16_t, uint16_t);
int via82xx_codec_read(struct audio_dma_device *, uint16_t, uint16_t *);
int via82xx_codec_write(struct audio_dma_device *, uint16_t, uint16_t);
uint32_t audio_hw_read(struct audio_dma_device *, unsigned, unsigned);
void audio_hw_write(struct audio_dma_device *, unsigned, uint32_t, unsigned);
uint64_t audio_hw_deadline(struct audio_dma_device *, uint32_t ms);
int audio_hw_wait(struct audio_dma_device *, unsigned, uint32_t mask, uint32_t value, uint32_t ms);
uint32_t audio_pci_read(const struct ac97_io *, const struct pci_func *, unsigned);
void audio_pci_write(const struct ac97_io *, const struct pci_func *, unsigned, uint32_t, unsigned);
bool audio_pci_routed(const struct ac97_io *, const struct pci_func *);
int audio_dma_progress(struct audio_dma_device *);
int audio_dma_publish(struct audio_dma_device *, const struct audio_frame *);
int audio_dma_start(struct audio_dma_device *);
int audio_dma_stop(struct audio_dma_device *);
int audio_dma_prove(struct audio_dma_device *);
int audio_dma_codec_prepare(struct audio_dma_device *, int (*read)(struct audio_dma_device *,uint16_t,uint16_t *),
                            int (*write)(struct audio_dma_device *,uint16_t,uint16_t));
int audio_dma_codec_mute(struct audio_dma_device *, bool, int (*write)(struct audio_dma_device *,uint16_t,uint16_t));
int audio_backend_init(void);
void audio_backend_snapshot(struct audio_transport *);
void audio_backend_idle(bool);
void audio_backend_rebase(uint64_t);
int audio_backend_progress(void);
int audio_backend_publish(const struct audio_frame *);
int audio_backend_start(void);
int audio_backend_stop(void);
int audio_backend_master(const struct ciuki_mixer_master *);
void audio_backend_shutdown(void);
void audio_backend_deadline(void);
#endif
