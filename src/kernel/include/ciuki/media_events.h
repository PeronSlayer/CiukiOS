/* Internal media-observer publication, independent of the drive namespace.
 * Standalone driver/storage host builds may omit the native observer.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MEDIA_EVENTS_H
#define CIUKI_MEDIA_EVENTS_H
extern void media_state_changed(void) __attribute__((weak));
static inline void media_notify_changed(void)
{
    if (media_state_changed) media_state_changed();
}
#endif
