/* F2 signal services; wire constants and records belong exclusively to abi.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SIGNAL_H
#define CIUKI_SIGNAL_H
#include <ciuki/process.h>

#define PROC_SIGNAL_SET (CIUKI_SIGBIT(SIGINT) | CIUKI_SIGBIT(SIGILL) | \
    CIUKI_SIGBIT(SIGABRT) | CIUKI_SIGBIT(SIGBUS) | CIUKI_SIGBIT(SIGFPE) | \
    CIUKI_SIGBIT(SIGKILL) | CIUKI_SIGBIT(SIGUSR1) | CIUKI_SIGBIT(SIGSEGV) | \
    CIUKI_SIGBIT(SIGUSR2) | CIUKI_SIGBIT(SIGPIPE) | CIUKI_SIGBIT(SIGTERM) | \
    CIUKI_SIGBIT(SIGCHLD))

struct sig_active {
    uint32_t tid, address, tls;
    struct ciuki_signal_frame original;
};

bool proc_signal_valid(uint32_t signo);
bool proc_signal_eligible(const struct proc_thread *t, uint32_t signo);
uint32_t proc_signal_next(struct proc_thread *t, bool *process_pending);
bool proc_signal_caught(struct proc_thread *t);
void proc_signal_refresh(struct process *p);
void proc_signal_post(struct process *p, struct proc_thread *t, uint32_t signo, uint32_t sender);
void proc_signal_child(struct process *parent, uint32_t child_pid);
void proc_signal_pipe(struct proc_thread *t);
int proc_signal_kill(struct process *caller, int32_t pid, uint32_t signo);
int proc_signal_thread_kill(struct process *caller, uint32_t tid, uint32_t signo);
void proc_signal_fault_info(const struct trap_frame *tf, uint32_t cr2, struct ciuki_siginfo *info);
void proc_signal_fault(struct trap_frame *tf, uint32_t cr2);
void proc_signal_return_to_user(struct trap_frame *tf);
__attribute__((noreturn)) void proc_signal_die(struct proc_thread *t, uint32_t signo);

/* Backends latch interruption once observed. I-class issued reads retain
 * pins/buffers and drain under their original deadline before returning.
 * D-class users set committed at the FIRST mutation/command issuance. */
enum signal_wait_class { SIGNAL_WAIT_I, SIGNAL_WAIT_D, SIGNAL_WAIT_DEFER };
enum signal_wait_decision { SIGNAL_WAIT_CONTINUE, SIGNAL_WAIT_EINTR, SIGNAL_WAIT_DRAIN };
enum signal_wait_decision proc_signal_decide(bool caught, enum signal_wait_class kind,
                                            bool committed, bool issued, uint32_t progress);
enum signal_wait_decision proc_signal_wait(struct proc_thread *t, enum signal_wait_class kind,
                                          bool committed, bool issued, uint32_t progress);
int32_t proc_signal_read_result(uint32_t progress, bool interrupted, bool drain_ok, int32_t result);

bool sigframe_executable(const struct uaddr *u, uint32_t address, bool image_only);
int sigframe_build(struct proc_thread *t, const struct trap_frame *tf,
                   const struct ciuki_sigaction *action, const struct ciuki_siginfo *info,
                   uint64_t token, struct ciuki_signal_frame *frame, uint32_t *address);
int sigframe_validate(const struct proc_thread *t, const struct sig_active *active,
                      uint32_t address, struct ciuki_signal_frame *frame, struct trap_frame *tf);
bool sigframe_fp_validate(uint8_t *image);
void sigframe_fp_sanitize(uint8_t *image);
void proc_signal_sigreturn(struct trap_frame *tf, uint32_t address);
int proc_sigaction(uint32_t signo, uint32_t new_action, uint32_t old_action);
int proc_sigprocmask(uint32_t how, uint32_t set, uint32_t old_set);
bool proc_signal_syscall(struct trap_frame *tf);

/* Integer conversion helpers are also exercised by T0. Image sizes derive
 * from ciuki_ucontext; the private FXSR save area is architectural. */
void fpu_signal_export(struct task *t, uint8_t *image);
void fpu_signal_reset(struct task *t);
void fpu_signal_import(struct task *t, const uint8_t *image);
void fpu_signal_to_fnsave(uint8_t *out, const uint8_t *in, bool fxsr);
void fpu_signal_from_fnsave(uint8_t *out, const uint8_t *in, bool fxsr);
#endif
