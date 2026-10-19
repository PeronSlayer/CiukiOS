/* SPDX-License-Identifier: LGPL-2.1-or-later */
.text
.globl __aulldiv
__aulldiv:
    jmp _k32_udiv@16
.globl __aullrem
__aullrem:
    jmp _k32_urem@16
