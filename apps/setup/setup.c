/* CiukiOS Setup placeholder (directive f9-01 item 7, cd-setup-plan.md 4).
 * Spawned by the supervisor in Setup mode from the boot CD's C: until
 * f9-03 replaces it with the libdeskui wizard. Console only, read only:
 * it prints the boot information, the volumes, the ATA disks with their
 * write counters (the Device inventory call also makes the kernel emit its
 * "[setup] writes disk=<n> count=<c>" records) and the media record, then
 * stays idle. No file is created or written.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/devices.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "setup_info.h"

static struct ciuki_devices_info info;
static char release_text[4096];

/* Regular files of the boot volume (C: is "/"; /mnt and /dev are the
 * namespace's other volumes and devices, not part of the image). */
static unsigned long count_files(char *path, size_t used, size_t size, unsigned depth)
{
    unsigned long files = 0;
    DIR *dir = opendir(used ? path : "/");
    if (!dir || depth > 16) { if (dir) closedir(dir); return 0; }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!used && (!strcmp(name, "mnt") || !strcmp(name, "dev"))) continue;
        size_t length = strlen(name);
        if (used + 1 + length + 1 > size) continue;
        path[used] = '/'; memcpy(path + used + 1, name, length + 1);
        struct stat st;
        if (!stat(path, &st)) {
            if (S_ISDIR(st.st_mode)) files += count_files(path, used + 1 + length, size, depth + 1);
            else if (S_ISREG(st.st_mode)) files++;
        }
        path[used] = 0;
    }
    closedir(dir);
    return files;
}

static void volumes(void)
{
    printf("Volumes:\n  C:  Setup CD system image (read-only)\n");
    DIR *dir = opendir("/mnt");
    if (!dir) return;
    char seen[27] = { 0 };
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (entry->d_name[0] >= 'd' && entry->d_name[0] <= 'z' && !entry->d_name[1]) seen[entry->d_name[0] - 'a'] = 1;
    closedir(dir);
    for (int letter = 3; letter < 26; letter++)
        if (seen[letter])
            printf("  %c:  %s\n", 'A' + letter, letter == 3 ? "Setup CD files (ISO 9660, read-only)" : "read-only volume");
}

static void disks(void)
{
    printf("Disks:\n");
    unsigned shown = 0;
    for (unsigned i = 0; i < info.count && i < CIUKI_DEVICES_MAX; i++) {
        const struct ciuki_device_record *d = &info.devices[i];
        if (d->class_id != CIUKI_DEVICE_DISK || strcmp(d->driver, "ata-pio")) continue;
        printf("  ATA disk %u: %s - %s\n", d->slot * 2 + d->function, d->name, d->reason);
        shown++;
    }
    if (!shown) printf("  no ATA disk found\n");
}

int main(void)
{
    printf("CiukiOS Setup\nClassic computing, one woof at a time.\n\n");
    info.version = CIUKI_DEVICES_VERSION; info.size = sizeof(info);
    if (ciuki_devices(&info)) printf("Device inventory unavailable\n");
    else {
        printf("Boot options: %s\nBoot flags: %08lx\nProcessor: %s %s\n", info.boot_options,
               (unsigned long)info.boot_flags, info.cpu_vendor, info.cpu_brand);
    }
    volumes();
    disks();
    struct setup_release release;
    int fd = open("/system/RELEASE.JSON", O_RDONLY);
    ssize_t bytes = fd < 0 ? -1 : read(fd, release_text, sizeof(release_text) - 1);
    if (fd >= 0) close(fd);
    char line[256];
    if (bytes <= 0 || setup_release_parse(release_text, (size_t)bytes, &release)) {
        snprintf(line, sizeof(line), "[setup] media bad reason=release mode=placeholder\n");
    } else {
        char path[512] = { 0 };
        unsigned long counted = count_files(path, 0, sizeof(path), 0);
        printf("Release: CiukiOS %s\n", release.version);
        if (setup_media_record(line, sizeof(line), &release, counted) < 0)
            snprintf(line, sizeof(line), "[setup] media bad reason=record mode=placeholder\n");
    }
    /* One write: the console layer turns it into one serial record. */
    fflush(stdout);
    write(1, line, strlen(line));
    printf("\nThis build has no installer yet. Nothing was written to any disk.\n"
           "Remove the Setup CD and switch the computer off.\n");
    fflush(stdout);
    for (;;) sleep(3600);
}
