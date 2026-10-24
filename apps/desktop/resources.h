/* SPDX-License-Identifier: MIT
 * G30 Resource Meter tray icon feed (SY-3): while a resmeter client has a
 * window the desktop samples its client table and the free physical pages
 * every five seconds and updates the tray icon (shell.c). */
#ifndef CIUKI_DESKTOP_RESOURCES_H
#define CIUKI_DESKTOP_RESOURCES_H
#include <stdint.h>
struct desktop;
#define DESK_RESOURCES_PERIOD_NS 5000000000ull
/* Replaceable page source (host tests); returns 0 or -errno. Default: kernel call 70 (system record only). */
extern int (*desk_resources_pages)(uint32_t *free_pages, uint32_t *usable_pages);
/* Frame thread, every loop: shows, updates or hides the icon. */
void desk_resources_poll(struct desktop *);
#endif
