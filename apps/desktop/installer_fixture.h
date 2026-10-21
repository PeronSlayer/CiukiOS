/* Runtime-only installer observer, also exercised on the host.
 * SPDX-License-Identifier: MIT */
#ifndef DESK_INSTALLER_FIXTURE_H
#define DESK_INSTALLER_FIXTURE_H
unsigned desk_installer_registry(unsigned stage);
int desk_installer_ready(void);
#endif
