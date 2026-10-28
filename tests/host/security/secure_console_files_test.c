/* H09 (files): the real private session console on the production fd table,
 * POSIX namespace and device nodes (FAT32 fixture, fake block device). The
 * console that SESSION_LAUNCH children and LOGON_UI receive is one
 * /dev/console description at 0..3: writes reach the session log, reads are
 * EOF, the access mode cannot be changed and nobody can open it O_RDWR.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "../proc/files_test.c"

static void session_console_case(struct process *p, const char *who)
{
    int32_t console = -1;
    CHECK(!files_session_console(p, &console) && console == 3);
    struct proc_object *o = p->fds[0].object; CHECK(o);
    for (unsigned i = 0; i < 4; i++) CHECK(p->fds[i].object == o && !p->fds[i].flags);
    CHECK(!p->fds[4].object);
    struct file_description *d = file_fd(p, 3);
    CHECK(d && d->node->kind == PX_CONSOLE && (d->flags & O_ACCMODE) == O_RDWR && d->references == 4);
    CHECK(files_session_console(p, &console) == -EBUSY && p->fds[0].object == o);
    /* Through the ordinary syscalls of a process that holds the description. */
    int k = file_fd_slot(client, 0); CHECK(k >= 0);
    o->retain(o); client->fds[k] = (struct proc_fd){ o, 0 };
    uint8_t sentinel[12]; memset(sentinel, 0x5a, sizeof(sentinel));
    CHECK(!ua_write(client->memory, USER2, sentinel, sizeof(sentinel)));
    CHECK(call(CIUKI_SYS_READ, (uint32_t)k, USER2, 12, 0, 0) == 0);
    uint8_t after[12]; CHECK(!ua_read(client->memory, after, USER2, 12) && !memcmp(after, sentinel, 12));
    unsigned before = console_count;
    CHECK(!ua_write(client->memory, USER2, "session log\n", 12));
    CHECK(call(CIUKI_SYS_WRITE, (uint32_t)k, USER2, 12, 0, 0) == 12);
    CHECK(console_count == before + 12 && !memcmp(console_bytes + before, "session log\n", 12));
    CHECK(call(CIUKI_SYS_FCNTL, (uint32_t)k, F_SETFL, O_WRONLY, 0, 0) == 0);
    CHECK((call(CIUKI_SYS_FCNTL, (uint32_t)k, F_GETFL, 0, 0, 0) & O_ACCMODE) == O_RDWR);
    CHECK(call(CIUKI_SYS_LSEEK64, (uint32_t)k, 0, 0, SEEK_SET, USER) == -ESPIPE);
    CHECK(!file_close(client, k));
    printf("H09 session console (%s): fds 0..3 one description, EOF reads, log writes, fixed access mode PASS\n", who);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); setvbuf(stdout, 0, _IONBF, 0);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    CHECK(!fake_open(&disk[0], argv[1])); disk[0].undo_enabled = true;
    CHECK(!cache_init(&cache, 0)); vfs_init(&vfs);
    CHECK(!fat_mount(&volumes[0], &cache, &disk[0].dev, 0, disk[0].dev.capacity, true));
    CHECK(!vfs_attach(&vfs, 2, &volumes[0]));
    CHECK(!files_attach(&vfs, &space)); CHECK(!files_bootstrap(&vfs));
    client_thread = make_process(proc_supervisor(), &client); g_current = client_thread->task; g_current->state = T_RUNNING;
    /* Nobody can open the console for reading, before or after. */
    CHECK(file_open(client, "/dev/console", O_RDWR, 0) == -EACCES);
    CHECK(file_open(client, "/dev/console", O_RDONLY, 0) == -EACCES);
    /* LOGON_UI-shaped SYSTEM child (PID 1 fixed manifest). */
    struct process *system_child; CHECK(!proc_prepare(proc_supervisor(), &system_child));
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) CHECK(!system_child->fds[i].object);
    session_console_case(system_child, "SYSTEM role child");
    /* SESSION_LAUNCH-shaped user child: credential replaced, table emptied,
     * cwd installed by px_security_cwd before the backend runs. */
    struct process *user_child; CHECK(!proc_prepare(proc_supervisor(), &user_child));
    CHECK(!security_bootstrap_desktop(user_child) && security_credential(user_child)->uid == 1000);
    CHECK(!user_child->cwd);
    int32_t none; CHECK(files_session_console(user_child, &none) == -ENOENT); /* no cwd: nothing installed */
    for (unsigned i = 0; i < 4; i++) CHECK(!user_child->fds[i].object);
    user_child->cwd = space->root; px_retain(user_child->cwd);
    user_child->cwd_retain = px_retain; user_child->cwd_release = px_release;
    session_console_case(user_child, "uid 1000 session child");
    /* An occupied standard slot refuses before any description is created. */
    struct process *busy; CHECK(!proc_prepare(proc_supervisor(), &busy));
    int fd = file_open(busy, "/dev/null", O_RDONLY, 0); CHECK(fd == 0);
    CHECK(file_dup(busy, 0, 2, true, 0) == 2 && !file_close(busy, 0));
    struct file_ledger ledger; files_snapshot(space, &ledger); unsigned descriptions = ledger.descriptions;
    CHECK(files_session_console(busy, &none) == -EBUSY);
    files_snapshot(space, &ledger); CHECK(ledger.descriptions == descriptions && !busy->fds[0].object);
    /* A published process is never re-equipped. */
    CHECK(files_session_console(client, &none) == -EPERM);
    proc_discard(busy); proc_discard(user_child); proc_discard(system_child);
    all_closed(); stop_collect(client);
    for (int i = 0; i < 3; i++) if (file_fd_object(proc_supervisor(), i)) CHECK(!file_close(proc_supervisor(), i));
    px_release(proc_supervisor()->cwd); proc_supervisor()->cwd = 0;
    files_snapshot(space, &ledger); CHECK(!ledger.descriptions && !ledger.pins);
    CHECK(!vfs_detach(&vfs, 2)); vfs_destroy(&vfs); cache_destroy(&cache);
    fake_reset(&disk[0]); fake_close(&disk[0]);
    kfree(supervisor_task); supervisor_task = 0;
    CHECK(!pages_used); free(ram);
    printf("H09 session console: PASS checks=%u\n", checks);
    return 0;
}
