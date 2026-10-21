/* Test-only extension of the confined i386 H1 adapter. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef INSTALLER_H1_SERVICES_H
#define INSTALLER_H1_SERVICES_H
#include "linux.h"
int h1_installer_init(const char *);
int h1_installer_kernel(void *,uint32_t,int32_t *);
uint32_t h1_installer_server(const void *,uint32_t,void *,uint32_t,uint32_t *);
void h1_installer_parameters(const char *);
void h1_installer_providers(const struct w32_builtin *,uint32_t);
#endif
