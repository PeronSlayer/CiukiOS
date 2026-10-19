"""Native/PE shared RPC layout checked against the actual f5-04 endpoint."""
from pathlib import Path
import resource
import signal
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[3]

class W32ServerWireTests(unittest.TestCase):
    def test_microsoft_i386_stack_allocation_helper(self):
        out=ROOT/'build/host/proc/w32srv'
        out.mkdir(parents=True,exist_ok=True)
        source=out/'stack-test.S'
        source.write_text('''.text
.globl _start
_start:
 movl $0,%esi
again:
 movl %esp,%edi
 movl $0x12345678,%ecx
 movl sizes(,%esi,4),%eax
 call __chkstk
 cmpl $0x12345678,%ecx
 jne failure
 movl %esp,%edx
 addl sizes(,%esi,4),%edx
 cmpl %edi,%edx
 jne failure
 movl %edi,%esp
 incl %esi
 cmpl $6,%esi
 jb again
 ud2
failure:
 int3
.section .rodata
sizes: .long 0,4,4095,4096,4097,8199
.section .note.GNU-stack,"",@progbits
''')
        objects=[]
        for i,p in enumerate((source,ROOT/'sdk/win32/services/stack.S')):
            obj=out/f'stack-{i}.o'
            subprocess.run(['clang','--target=i386-unknown-linux-gnu','-c',str(p),'-o',str(obj)],check=True)
            objects.append(str(obj))
        binary=out/'stack-test'
        subprocess.run(['ld.lld','-m','elf_i386','-static','--entry=_start','-o',str(binary),*objects],check=True)
        # The self-authored host instruction test makes no syscall. UD2 is its
        # success sentinel; INT3 means an assertion failed. No guest runs.
        try:
            result=subprocess.run([str(binary)],capture_output=True,timeout=5,
                                  preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        except OSError as error:
            self.skipTest('host cannot execute ELF32: '+str(error))
        self.assertEqual(result.returncode,-signal.SIGILL,result.stderr)

    def test_i386_service_prefix_and_channel_frame(self):
        out=ROOT/'build/host/proc/w32srv'
        out.mkdir(parents=True,exist_ok=True)
        source=out/'wire.c'
        source.write_text('''#include <ciuki/w32_objects.h>
#include <w32/server.h>
#define ASSERT(x) _Static_assert(x, #x)
ASSERT(sizeof(struct w32r_service)==sizeof(struct ciuki_w32_service_args));
ASSERT(__builtin_offsetof(struct w32r_service,version)==__builtin_offsetof(struct ciuki_w32_service_args,header.version));
ASSERT(__builtin_offsetof(struct w32r_service,size)==__builtin_offsetof(struct ciuki_w32_service_args,header.size));
ASSERT(__builtin_offsetof(struct w32r_service,operation)==__builtin_offsetof(struct ciuki_w32_service_args,header.operation));
ASSERT(__builtin_offsetof(struct w32r_service,action)==__builtin_offsetof(struct ciuki_w32_service_args,action));
ASSERT(__builtin_offsetof(struct w32r_service,error)==__builtin_offsetof(struct ciuki_w32_service_args,error));
ASSERT(sizeof(struct w32r_packet)==CIUKI_MESSAGE_PAYLOAD_MAX);
ASSERT(sizeof(struct w32r_request)==40);
ASSERT(sizeof(struct w32r_reply)==40);
ASSERT(sizeof(struct w32r_window)==24);
''')
        for target in ('i686-unknown-elf','i686-pc-windows-msvc','i686-w64-mingw32'):
            subprocess.run(['clang','--target='+target,'-march=pentiumpro','-ffreestanding','-Wall','-Wextra','-Werror',
                            '-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'sdk/win32/include'),'-c',str(source),
                            '-o',str(out/(target+'.o'))],check=True)

if __name__=='__main__':unittest.main()
