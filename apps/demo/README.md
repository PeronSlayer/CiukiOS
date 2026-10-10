# Ciuki demo client

Built alongside the desktop with the static Ciuki SDK. Creates a 240x160 shared
surface, sends HELLO/CREATE_WINDOW, displays a checker pattern and typed ASCII,
counts motion/buttons, answers heartbeat PINGs, and keeps a PING transaction
moving every main-loop turn. It receives no display or input grant.

The launcher explicitly maps its channel endpoint to fd 3, or supplies
`--channel-fd=N`. Supported `--fault=` values are `none`, `bad-pointer`,
`closed-peer`, `forged-fd`, `grant-fd`, and `handler-fault`. Faults occur only
after CONFIGURE proves the desktop accepted the shared surface, and after a
PING is sent without consuming its reply. Detailed expected outcomes and the
grant-test boundary are in [docs/desktop.md](../../docs/desktop.md).
