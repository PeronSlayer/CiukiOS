/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/audio.h>
#include <errno.h>
#include <string.h>
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size)
{
    uint32_t value = CU_CALL(DEVCTL, fd, op, CU_PTR(record), size, 0, 0);
    int error = ciuki_error(value);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)value;
}
int ciuki_midi_write(int fd,const uint8_t *bytes,uint32_t size)
{
    if (!size) return 0;
    if (!bytes) { __getreent()->_errno=EFAULT; return -1; }
    struct ciuki_midi_data data={.version=1,.size=sizeof(data)};
    data.bytes=size>CIUKI_MIDI_BYTES ? CIUKI_MIDI_BYTES : size;
    memcpy(data.data,bytes,data.bytes);
    if (ciuki_devctl(fd,AUDIO_MIDI_WRITE,&data,sizeof(data))) return -1;
    return (int)data.accepted;
}
