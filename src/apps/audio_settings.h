/* Shared AC'97 master preference for the desktop and Sound panel.
 * VOLUME.CFG packs level 0..31 in bits 0..4 and mute in bit 7.
 */
#ifndef CIUKIOS_AUDIO_SETTINGS_H
#define CIUKIOS_AUDIO_SETTINGS_H

#define AUDIO_SETTINGS_PATH "\\SYSTEM\\VOLUME.CFG"

#ifndef AUDIO_SETTINGS_NO_LOAD
static int audio_settings_load(int *level, int *mute)
{
    char data[2];
    int parsed_level, parsed_mute, closed;
    u8 value;
    int f = dos_open(AUDIO_SETTINGS_PATH, 0);
    int n;
    if (f < 0) return 0;
    n = dos_read(f, data, sizeof data);
    closed = dos_close(f);
    if (closed < 0 || n != 1) return 0;
    value = (u8)data[0];
    if (value & 0x60) return 0;
    parsed_level = value & 31;
    parsed_mute = (value & 0x80) != 0;
    *level = parsed_level;
    *mute = parsed_mute;
    return 1;
}
#endif

#ifndef AUDIO_SETTINGS_NO_SAVE
static int audio_settings_save(int level, int mute)
{
    char data;
    int f, n, closed;
    if (level < 0) level = 0;
    if (level > 31) level = 31;
    data = (char)(level | (mute ? 0x80 : 0));
    f = dos_create(AUDIO_SETTINGS_PATH);
    if (f < 0) return 0;
    n = dos_write(f, &data, 1);
    closed = dos_close(f);
    return n == 1 && closed >= 0;
}
#endif

#endif
