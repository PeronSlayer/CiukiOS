/* The process/signal host harness never executes the PID 1 kernel task.
 * Keep its link independent of device/bootstrap services when the lead adds
 * the supervisor_poll hook. Desktop tests link the production implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
void supervisor_poll(void) { }
