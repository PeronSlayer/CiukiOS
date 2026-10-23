/* Serial/USB/PS2 pointer merge through the production queue.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "usb/input_queue.c"
static bool firmware_policy;
bool drivers_input_firmware(void) { return firmware_policy; }
bool drivers_input_native(void) { return !firmware_policy; }
void klog(const char *format, ...) { (void)format; }
uint64_t g_tsc_per_ms;
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
static struct input_event event(void)
{
    struct input_event e; assert(native.queue.stats.pending);
    struct input_queue *q=&native.queue; e=q->ring[q->head];
    q->head=(q->head+1)%INPUT_CAPACITY; q->stats.pending--; return e;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    firmware_policy=true;
    input_serial_event(INPUT_BTN,INPUT_LEFT,1,11,10);
    struct input_event e=event(); assert(e.type==INPUT_BTN && e.value==1 && e.generation==11);
    firmware_policy=false;
    input_usb_event(INPUT_BTN,INPUT_LEFT,1,12,11); assert(!native.queue.stats.pending);
    input_serial_event(INPUT_BTN,INPUT_LEFT,0,11,12); assert(!native.queue.stats.pending && native.queue.stats.buttons==1);
    input_usb_event(INPUT_BTN,INPUT_LEFT,0,12,13); e=event(); assert(e.type==INPUT_BTN && !e.value);
    mouse_byte(&native,9,14); mouse_byte(&native,0,14); mouse_byte(&native,0,14);
    e=event(); assert(e.type==INPUT_BTN && e.value==1);
    input_serial_event(INPUT_BTN,INPUT_LEFT,1,13,15); assert(!native.queue.stats.pending);
    input_serial_event(INPUT_BTN,INPUT_LEFT,0,13,16); assert(!native.queue.stats.pending && native.queue.stats.buttons==1);
    mouse_byte(&native,8,17); mouse_byte(&native,0,17); mouse_byte(&native,0,17);
    e=event(); assert(e.type==INPUT_BTN && !e.value);
    input_serial_event(INPUT_REL,INPUT_X,-255,14,18); e=event(); assert(e.value==-255 && e.code==INPUT_X);
    input_serial_event(INPUT_REL,INPUT_WHEEL,1,14,19); e=event(); assert(e.value==1 && e.code==INPUT_WHEEL);
    assert(!native.queue.stats.pending && !native.queue.stats.buttons);
    puts("Serial pointer merge: firmware policy, USB/PS2 held buttons, release and wheel PASS");
    return 0;
}
