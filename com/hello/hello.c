#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

void com_main(void *boot_info_ptr, void *handoff_ptr, ciuki_services_t *svc) {
    (void)boot_info_ptr;
    (void)handoff_ptr;

    svc->cls();
    svc->print("================================\n");
    svc->print("  Hello from INIT.COM!\n");
    svc->print("  CiukiOS Phase 0 - COM loader\n");
    svc->print("================================\n");
    svc->print("\nCOM executed successfully.\n");
    svc->print("Returning to shell...\n");
}
