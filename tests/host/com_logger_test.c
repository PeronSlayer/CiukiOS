/* Both production drivers share one IRQ dispatcher and independent UARTs.
 * SPDX-License-Identifier: GPL-2.0-only */
#define COM_WITH_LOGGER
#define main com_fixture_main
#include "com_test.c"
#undef main
#include "../../src/kernel/core/serial.c"
int main(void)
{
    for (unsigned irq=3;irq<=4;irq++) {
        reset(); serial_init(0,3);
        unsigned base=irq==3 ? 0x2f8 : 0x3f8;
        make_model(0,base,CIUKI_COM_16550A); make_model(1,base-0x10,CIUKI_COM_16650);
        serial_init((uint16_t)base,3); assert(serial_present());
        g_boot.uart_base=base;
        assert(registry_claim(RES_PORT,base,base+8,"uart",false)>=0);
        assert(registry_claim(RES_IRQ,irq,irq+1,"uart",false)>=0);
        serial_writer_init();
        unsigned before=reads+writes;
        assert(admit(0,1,irq,CIUKI_COM_BDA)==0 && reads+writes==before);
        uint32_t g; assert(com_open(0,&g)==-EBUSY && reads+writes==before);
        assert(admit(1,2,irq,CIUKI_COM_BDA)==1 && !com_open(1,&g));
        uint8_t payload[127]; for (unsigned i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)i;
        assert(com_transfer(1,g,payload,sizeof(payload),true)==sizeof(payload));
        serial_write("BOOT\n",5); serial_tick();
        unsigned passes=0;
        while (uart_used || ports[1].status.tx_queued) {
            assert(++passes<100);
            if (passes==2) serial_write("READY\n",6);
            serial_tick();
            receive(&models[1],0x6d); interrupt(irq);
        }
        struct serial_stats log; serial_snapshot(&log);
        assert(log.accepted==2 && log.sent==2 && !log.dropped);
        assert(models[0].tx_used==13 && !memcmp(models[0].tx,"BOOT\r\nREADY\r\n",13));
        assert(models[1].tx_used==sizeof(payload) && !memcmp(models[1].tx,payload,sizeof(payload)));
        assert(models[0].regs[3]==3 && models[0].divisor[0]==3 && !models[0].divisor[1] && models[0].regs[4]==0x0b);
        com_close(1,g); drain_work();
    }
    reset();
    puts("PASS COM production logger arbitration: IRQ3/4, exact boot log and binary application TX/RX, reserved-open refusal, zero log drops/configuration changes");
    return 0;
}
