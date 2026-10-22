/* Per-game network preference; no kernel or memory setting is overwritten.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_NETWORK_SETTINGS_H
#define CIUKI_DOSBOX_NETWORK_SETTINGS_H
#include "settings.h"
#include "../../src/kernel/include/ciuki/dosvm_network_abi.h"
static inline const char *dosbox_network_label(unsigned mode)
{ return mode == CIUKI_DOSVM_NETWORK_IPX ? "IPX (LAN)" : "off (default)"; }
static inline int dosbox_network_path(unsigned game, char *out, size_t size)
{
    if (!game || game > DESK_GAME_COUNT) { errno = EINVAL; return -1; }
    int n = snprintf(out, size, DOSBOX_SETTINGS_DIR "/dos-game-%u-network.cfg", game);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
static inline unsigned dosbox_network_load(unsigned game)
{
    char path[144], text[32];
    if (dosbox_network_path(game, path, sizeof(path))) return CIUKI_DOSVM_NETWORK_OFF;
    FILE *file = fopen(path, "rb"); if (!file) return CIUKI_DOSVM_NETWORK_OFF;
    size_t n = fread(text, 1, sizeof(text), file);
    int error = ferror(file); fclose(file);
    static const char ipx[] = "network=ipx\n";
    return !error && n == sizeof(ipx) - 1 && !memcmp(text, ipx, n) ? CIUKI_DOSVM_NETWORK_IPX : CIUKI_DOSVM_NETWORK_OFF;
}
#endif
