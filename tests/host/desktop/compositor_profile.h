/* SPDX-License-Identifier: MIT
 * Host-only, nested exclusive painter timing. Inject with -include. */
#ifndef CIUKI_COMPOSITOR_PROFILE_H
#define CIUKI_COMPOSITOR_PROFILE_H
void compositor_profile_enter(const char *name);
void compositor_profile_leave(void);
#define DESK_PROFILE(name, ...) do { compositor_profile_enter(name); __VA_ARGS__; compositor_profile_leave(); } while (0)
#endif
