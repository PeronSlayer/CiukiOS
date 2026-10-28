/* Fake credential admission for isolated hardware fixtures; queries use the
 * actual security.c implementation. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/process.h>
void security_fixture_identity(struct process *, uint32_t uid, uint32_t session, bool admin, uint32_t role);
