/* Test-only extension of the confined i386 H1 adapter. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef INSTALLER_H1_SERVICES_H
#define INSTALLER_H1_SERVICES_H
#include "linux.h"
int h1_installer_init(const char *);
int h1_installer_kernel(void *,uint32_t,int32_t *);
uint32_t h1_installer_server(const void *,uint32_t,void *,uint32_t,uint32_t *);
void h1_installer_parameters(const char *);
void h1_installer_providers(const struct w32_builtin *,uint32_t);
/* f7 game mode (h1_game.c): desktop, surfaces, display, message queue. */
void h1_game_init(int32_t (*)(void *,uint32_t));
int h1_game_enabled(void);
int h1_game_kernel(void *,uint32_t,int32_t *);
int h1_game_gate(uint32_t,uint32_t,uint32_t,uint32_t,int32_t *);
void h1_game_report(void);
void h1_game_module(const char *,void *); /* after a DLL's successful DLL_PROCESS_ATTACH: GL call profile hooks */
void h1_game_exe(void *); /* fx-hlrun: the EXE before its entry point: "apicount=1" import call counts */
int h1_game_syscall(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int32_t *); /* fx-hlrun: PE int 0x80 (SIGSYS) */
#endif
void h1_game_script(const char *command_line);   /* +set ciuki_keys "text": console typing on the lease frames */
void h1_game_options(const char *options);      /* game-exec harness option defaults=IDLE_MS,MAX (fx-is5boot) */
