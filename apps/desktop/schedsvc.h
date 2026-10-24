/* SPDX-License-Identifier: MIT
 * G30 scheduler service (SY-7). A worker thread owns the schedule file
 * (SS_PATH) and the pure service step of apps/systools/service.c; the frame
 * thread owns launching, through the desktop's existing launcher
 * (desk_task_launch), so a scheduled program starts exactly as one chosen
 * from the Programs menu. Failures reach the user as the shell's error
 * notice; the recorded last run is the time the desktop started the task. */
#ifndef CIUKI_DESKTOP_SCHEDSVC_H
#define CIUKI_DESKTOP_SCHEDSVC_H
#include <stddef.h>
#include <stdint.h>
struct desktop;
struct desk_schedsvc;
#define DESK_SCHED_POLL_MS 30000u
#define DESK_SCHED_QUEUE 8u
/* Starts the worker (0 or an errno); NULL-safe and idempotent. */
int desk_schedsvc_start(struct desktop *);
/* Frame thread: launches what the worker queued; the worker polls every DESK_SCHED_POLL_MS. */
void desk_schedsvc_tick(struct desktop *);
void desk_schedsvc_stop(struct desktop *);
/* Test seams: replace the file, clock and launcher of a service created by desk_schedsvc_create. */
struct desk_schedsvc_io {
    void *ctx;
    int (*load)(void *, char *text, size_t cap, size_t *length);
    int (*save)(void *, const char *text, size_t length);
    int64_t (*now_minutes)(void *);
    int (*launch)(void *, const char *program, const char *argument);   /* frame thread; >0 pid or -errno */
    void (*failed)(void *, const char *program, int error);             /* frame thread, after a launch error */
};
struct desk_schedsvc *desk_schedsvc_create(const struct desk_schedsvc_io *);
/* One synchronous worker step (the thread calls this every poll); returns tasks queued, or -errno. */
int desk_schedsvc_step(struct desk_schedsvc *);
/* One synchronous frame-thread step: launches every queued task through io->launch; returns how many. */
unsigned desk_schedsvc_drain(struct desk_schedsvc *);
void desk_schedsvc_destroy(struct desk_schedsvc *);
#endif
