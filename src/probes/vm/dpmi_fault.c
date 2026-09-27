/* Fault-unwind probe for the built-in official HDPMI 3.24 adapter. */
#include <conio.h>
#include <stdio.h>

static void invalid_opcode(void);
#pragma aux invalid_opcode = "cli" "db 0fh,0bh" modify exact []

static void mark(const char *text)
{
    const char *p = text;
    unsigned wait;
    while (*p) {
        wait = 65535;
        while (wait-- && !(inp(0x3fd) & 0x20)) { }
        outp(0x3f8, *p++);
    }
    outp(0x3f8, '\r'); outp(0x3f8, '\n');
    puts(text);
}

int main(void)
{
    mark("[DPMIFAULT] UD2 ENTER");
    invalid_opcode();
    mark("[DPMIFAULT] FAIL returned from UD2");
    return 1;
}
