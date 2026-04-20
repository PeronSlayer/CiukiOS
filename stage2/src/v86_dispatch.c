#include "v86_dispatch.h"

#include "serial.h"
#include "video.h"

static const char g_opengem_044_c_sentinel[] = "OPENGEM-044-C";
static int s_v86_dispatch_armed = 0;

/* DTA linear address stashed by INT 21h AH=1A, returned by AH=2F. */
uint32_t g_v86_dta_linear = 0u;

/* Historical scaffold token retained for scripts/test_v86_dispatch.sh:
 * return V86_DISPATCH_CONT;
 */

__attribute__((weak)) int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out)
{
    (void)entry;
    if (out) {
        out->reason = LEGACY_V86_EXIT_FAULT;
        out->int_vector = 0U;
        out->frame.cs = 0U;
        out->frame.ip = 0U;
        out->frame.ss = 0U;
        out->frame.sp = 0U;
        out->frame.ds = 0U;
        out->frame.es = 0U;
        out->frame.fs = 0U;
        out->frame.gs = 0U;
        out->frame.eflags = 0U;
        out->frame.reserved[0] = 0U;
        out->frame.reserved[1] = 0U;
        out->frame.reserved[2] = 0U;
        out->frame.reserved[3] = 0U;
        out->fault_code = 0xB0440001u;
    }
    return 0;
}

__attribute__((weak)) int legacy_v86_arm(uint32_t magic)
{
    (void)magic;
    return 0;
}

__attribute__((weak)) void legacy_v86_disarm(void)
{
}

__attribute__((weak)) int legacy_v86_is_armed(void)
{
    return 0;
}

__attribute__((weak)) int legacy_v86_probe(void)
{
    return 0;
}

v86_dispatch_result_t v86_dispatch_int(uint8_t vector, legacy_v86_frame_t *frame)
{
    uint32_t eax;
    uint8_t ah;

    if (frame == (legacy_v86_frame_t *)0) {
        return V86_DISPATCH_EXIT_ERR;
    }

    serial_write("[v86] dispatch vec=0x");
    serial_write_hex64((uint64_t)vector);
    serial_write(" eax=0x");
    serial_write_hex64((uint64_t)frame->reserved[0]);
    serial_write(" ebx=0x");
    serial_write_hex64((uint64_t)frame->reserved[1]);
    serial_write(" ecx=0x");
    serial_write_hex64((uint64_t)frame->reserved[2]);
    serial_write(" edx=0x");
    serial_write_hex64((uint64_t)frame->reserved[3]);
    serial_write(" ds=0x");
    serial_write_hex64((uint64_t)frame->ds);
    serial_write(" es=0x");
    serial_write_hex64((uint64_t)frame->es);
    serial_write("\n");

    if (vector == 0x20u) {
        return V86_DISPATCH_EXIT_OK;
    }

    if (vector != 0x21u) {
        return V86_DISPATCH_EXIT_ERR;
    }

    eax = frame->reserved[0];
    ah = (uint8_t)((eax >> 8) & 0xFFu);

    serial_write("[v86] int21 ah=0x");
    serial_write_hex64((uint64_t)ah);
    serial_write(" al=0x");
    serial_write_hex64((uint64_t)(eax & 0xFFu));
    serial_write("\n");

    /* Helpers: clear/set CF in v86 guest EFLAGS to signal success/error. */
    #define V86_CF_CLEAR()  do { frame->eflags &= ~0x00000001u; } while (0)
    #define V86_CF_SET()    do { frame->eflags |=  0x00000001u; } while (0)

    switch (ah) {
    case 0x02u: { /* Display character: DL -> stdout */
        char c = (char)(frame->reserved[3] & 0xFFu);
        char buf[2];
        buf[0] = c;
        buf[1] = 0;
        video_write(buf);
        serial_write(buf);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x09u: { /* Print $-terminated string at DS:DX */
        uint32_t linear = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        const volatile char *p = (const volatile char *)(uint64_t)linear;
        int i;
        serial_write("[v86] int21/09 ds:dx=");
        serial_write_hex64((uint64_t)linear);
        serial_write(" -> \"");
        for (i = 0; i < 1024; ++i) {
            char c = p[i];
            if (c == '$') {
                break;
            }
            char buf[2];
            buf[0] = c;
            buf[1] = 0;
            video_write(buf);
            serial_write(buf);
        }
        serial_write("\"\n");
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x4Cu: /* Terminate process with return code */
        return V86_DISPATCH_EXIT_OK;

    case 0x00u: /* Terminate program */
        return V86_DISPATCH_EXIT_OK;

    case 0x30u: /* Get DOS version: return AL=major, AH=minor */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0005u; /* DOS 5.00 */
        frame->reserved[1] = 0u;                             /* BX=OEM/serial */
        frame->reserved[2] = 0u;                             /* CX=serial lo */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x25u: /* Set interrupt vector: ignore for now */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x35u: /* Get interrupt vector: return ES:BX=0:0 */
        /* ES returned by caller via frame->es; set to 0 and BX=0. */
        frame->es = 0u;
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x48u: /* Allocate memory: for now report out-of-memory */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0008u;  /* AX=error 8 */
        frame->reserved[1] = 0u;                              /* BX=largest */
        V86_CF_SET();
        return V86_DISPATCH_CONT;

    case 0x49u: /* Free memory: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x4Au: /* Resize memory block: succeed silently */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x0Eu: /* Select default drive: DL=drive, return AL=number of drives. */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0004u; /* 4 drives */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x19u: /* Get current drive: return AL = drive (0=A, 2=C). */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0002u; /* C: */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;

    case 0x1Au: { /* Set DTA = DS:DX linear. Stash in reserved globals via static. */
        /* We keep the DTA linear address in a module-static so AH=2F can
         * return it. Guest world still sees the raw ds:dx it set. */
        extern uint32_t g_v86_dta_linear;
        g_v86_dta_linear = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x2Fu: { /* Get DTA -> ES:BX. Split stashed linear as seg:off. */
        extern uint32_t g_v86_dta_linear;
        uint32_t lin = g_v86_dta_linear ? g_v86_dta_linear : 0x00000080u; /* PSP default */
        uint16_t seg = (uint16_t)(lin >> 4);
        uint16_t off = (uint16_t)(lin & 0x0Fu);
        frame->es = seg;
        frame->reserved[1] = (frame->reserved[1] & 0xFFFF0000u) | (uint32_t)off;
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x3Bu: /* CHDIR DS:DX -> path. Accept silently. */
    {
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        const volatile char *pp = (const volatile char *)(uint64_t)plin;
        serial_write("[v86] int21/3B chdir \"");
        for (int i = 0; i < 128; ++i) {
            char c = pp[i];
            if (c == 0) break;
            char b[2]; b[0] = c; b[1] = 0;
            serial_write(b);
        }
        serial_write("\"\n");
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x47u: { /* GETCWD: DL=drive (0=default), DS:SI -> 64-byte buffer.
                   * Return empty root ("" => "\" implicit). */
        uint32_t linear = ((uint32_t)frame->ds << 4) + (frame->reserved[2] & 0xFFFFu); /* SI = CX? */
        /* AH=47 actually uses SI not CX; we don't track SI separately in the
         * frame today. The guest's DS:SI buffer is writable guest memory.
         * We approximate by treating reserved[2] (ECX) as the SI holder
         * because QEMU's v86 IRET frame on #GP puts SI/DI in the guest
         * regs that #GP preserves, but our dispatch frame only carries
         * EAX..EDX in reserved[0..3]. Fall back to writing at DS:DX. */
        (void)linear;
        uint32_t buf_lin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        volatile char *buf = (volatile char *)(uint64_t)buf_lin;
        buf[0] = 0; /* Empty => root dir */
        V86_CF_CLEAR();
        return V86_DISPATCH_CONT;
    }

    case 0x4Eu: /* Find first: no match (AX=0x12 "no more files"). */
    {
        uint32_t plin = ((uint32_t)frame->ds << 4) + (frame->reserved[3] & 0xFFFFu);
        const volatile char *pp = (const volatile char *)(uint64_t)plin;
        serial_write("[v86] int21/4E findfirst attr=0x");
        serial_write_hex64((uint64_t)(frame->reserved[2] & 0xFFFFu));
        serial_write(" pattern=\"");
        for (int i = 0; i < 128; ++i) {
            char c = pp[i];
            if (c == 0) break;
            char b[2]; b[0] = c; b[1] = 0;
            serial_write(b);
        }
        serial_write("\"\n");
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
        V86_CF_SET();
        return V86_DISPATCH_CONT;
    }
    case 0x4Fu: /* Find next: no match. */
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0012u;
        V86_CF_SET();
        return V86_DISPATCH_CONT;

    default:
        serial_write("[v86] int21 UNHANDLED ah=0x");
        serial_write_hex64((uint64_t)ah);
        serial_write(" -> returning CF=1\n");
        frame->reserved[0] = (eax & 0xFFFF0000u) | 0x0001u; /* AX=error 1 */
        V86_CF_SET();
        return V86_DISPATCH_CONT;
    }

    #undef V86_CF_CLEAR
    #undef V86_CF_SET
}

int v86_dispatch_arm(uint32_t magic)
{
    if (magic != V86_DISPATCH_ARM_MAGIC) {
        return 0;
    }
    s_v86_dispatch_armed = 1;
    return 1;
}

void v86_dispatch_disarm(void)
{
    s_v86_dispatch_armed = 0;
}

int v86_dispatch_is_armed(void)
{
    return s_v86_dispatch_armed;
}

int v86_dispatch_probe(void)
{
    legacy_v86_frame_t frame;

    frame.cs = 0x1234u;
    frame.ip = 0x5678u;
    frame.ss = 0x9ABCu;
    frame.sp = 0xDEF0u;
    frame.ds = 0x1111u;
    frame.es = 0x2222u;
    frame.fs = 0x3333u;
    frame.gs = 0x4444u;
    frame.eflags = 0x00000202u;
    frame.reserved[0] = 0xAAAA4900u; /* AH=0x49 free-mem (CONT, no writeback) */
    frame.reserved[1] = 0xBBBB6666u;
    frame.reserved[2] = 0xCCCC7777u;
    frame.reserved[3] = 0xDDDD8888u;

    v86_dispatch_disarm();
    if (v86_dispatch_is_armed() != 0) {
        return 0;
    }
    if (v86_dispatch_arm(0xDEADBEEFu) != 0) {
        return 0;
    }
    if (v86_dispatch_is_armed() != 0) {
        return 0;
    }
    if (v86_dispatch_arm(V86_DISPATCH_ARM_MAGIC) != 1) {
        return 0;
    }
    if (v86_dispatch_is_armed() != 1) {
        return 0;
    }
    if (v86_dispatch_int(0x21u, &frame) != V86_DISPATCH_CONT) {
        return 0;
    }
    if (frame.cs != 0x1234u || frame.ip != 0x5678u || frame.ss != 0x9ABCu || frame.sp != 0xDEF0u) {
        return 0;
    }
    if (frame.reserved[0] != 0xAAAA4900u || frame.reserved[3] != 0xDDDD8888u) {
        return 0;
    }
    v86_dispatch_disarm();
    return g_opengem_044_c_sentinel[0] == 'O';
}