/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_SECURITY_H
#define CIUKI_SDK_SECURITY_H
#include <ciuki/abi.h>
/* Call 101. Records/constants are copied from the canonical ABI at SDK build.
 * Return 0, or -1 with errno; buffers remain unchanged on failure. */
int ciuki_security(struct ciuki_security_args *args);
int ciuki_security_self(struct ciuki_credential *credential);
int ciuki_security_process(uint32_t pid, struct ciuki_credential *credential);
int ciuki_security_peer(int fd, struct ciuki_credential *credential);
/* Read-only state query; a session id is never a transferable credential. */
int ciuki_security_session(uint32_t session, uint32_t *state);
#endif
