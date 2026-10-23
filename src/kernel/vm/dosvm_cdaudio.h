/* fx-10 DOS VM CD audio: MSCDEX drive per window VM and the streaming task
 * (dosvm_cdaudio.c). SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_CDAUDIO_H
#define CIUKI_DOSVM_CDAUDIO_H
#include <ciuki/dosvm.h>
/* Client context, after the VM's name is set and before boot: binds the
 * MSCDEX drive and scans the virtual disc (bounded wait). Never fatal. */
int dosvm_cdaudio_open(struct dosvm *);
/* Destroy path, after the VM worker is reaped: stops the stream. */
void dosvm_cdaudio_close(struct dosvm *);
/* One pass of the streaming task; true while a stream needs feeding. */
bool dosvm_cdaudio_step(void);
#endif
