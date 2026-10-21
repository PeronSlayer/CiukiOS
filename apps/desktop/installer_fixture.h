/* Runtime-only installer observer, also exercised on the host.
 * SPDX-License-Identifier: MIT */
#ifndef DESK_INSTALLER_FIXTURE_H
#define DESK_INSTALLER_FIXTURE_H
struct desktop;
int desk_installer_fixture_active(void);
int desk_installer_fixture_begin(struct desktop *);
void desk_installer_fixture_tick(struct desktop *);
unsigned desk_installer_registry(unsigned stage);
int desk_installer_ready(void);
#endif
