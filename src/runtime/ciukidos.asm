; Canonical full-profile CIUKIDOS kernel image.
;
; The complete normal DOS runtime is assembled from the former Stage1 core
; under the kernel profile.  The full-profile Stage1 is a separate bounded
; FAT16 loader and contains no INT 20h/21h ownership or DOS process state.

%define CIUKIDOS_KERNEL_BUILD 1
%include "src/boot/floppy_stage1.asm"
