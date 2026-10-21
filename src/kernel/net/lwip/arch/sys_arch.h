/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_LWIP_SYS_ARCH_H
#define CIUKI_LWIP_SYS_ARCH_H
struct cu_sem;
struct cu_mutex;
struct cu_mbox;
typedef struct cu_sem *sys_sem_t;
typedef struct cu_mutex *sys_mutex_t;
typedef struct cu_mbox *sys_mbox_t;
typedef uintptr_t sys_thread_t;
#define SYS_SEM_NULL NULL
#define SYS_MBOX_NULL NULL
#define sys_sem_valid(s) (*(s) != NULL)
#define sys_sem_set_invalid(s) (*(s) = NULL)
#define sys_mutex_valid(s) (*(s) != NULL)
#define sys_mutex_set_invalid(s) (*(s) = NULL)
#define sys_mbox_valid(s) (*(s) != NULL)
#define sys_mbox_set_invalid(s) (*(s) = NULL)
/* Cancellation is deliberately excluded from internal lwIP completions. */
uint64_t cu_now(void);
void cu_sleep(unsigned);
void cu_yield(void);
uintptr_t cu_identity(void);
unsigned cu_arch_bytes(void);
unsigned cu_mbox_bytes(void);
unsigned cu_mbox_count(sys_mbox_t);
unsigned cu_change_snapshot(void);
void cu_changed(void);
void cu_wait_change(unsigned, uint64_t);
#ifdef LWIP_HOST
void cu_host_join(sys_thread_t);
void cu_host_shutdown(void);
void cu_host_clock_offset(uint64_t);
#endif
#endif
