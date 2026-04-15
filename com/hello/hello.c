#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    (void)ctx;

    svc->cls();
    svc->print("================================\n");
    svc->print("  Hello from INIT.COM!\n");
    svc->print("  CiukiOS M1 - DOS-like .COM\n");
    svc->print("================================\n");
    svc->print("\nCOM executed successfully.\n");
    svc->print("PSP segment: 0x");
    svc->print_hex64((unsigned long long)ctx->psp_segment);
    svc->print("\n");
    if (ctx->command_tail_len > 0) {
        svc->print("Tail: ");
        svc->print(ctx->command_tail);
        svc->print("\n");
    }
    svc->print("Returning to shell...\n");
    svc->int21_4c(ctx, 0x00);
}
