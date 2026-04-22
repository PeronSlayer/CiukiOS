# Handoffs for Claude Code

From now on, for every major change set, create a handoff file in this directory.

## Naming Rule
`YYYY-MM-DD-<topic>.md`

Example:
`2026-04-15-stage2-bootstrap.md`

## When to create a handoff
1. Architectural refactor or redesign.
2. New subsystem (loader, memory manager, INT 21h layer, filesystem).
3. Multi-file changes affecting ABI/boot/runtime.
4. Any change set that requires non-trivial context to continue.

## Minimum flow
1. Write the handoff before ending the session.
2. Include status, decisions, risks, and immediate next steps.
3. Link touched files and executed tests.

## Template
Use: `docs/handoffs/HANDOFF_TEMPLATE.md`
