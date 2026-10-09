#ifndef FS_TEST_SCAN_H
#define FS_TEST_SCAN_H
struct scan_result { unsigned crosslinks, corrupt, lost, divergent, dirty, files; };
struct scan_result independent_scan(int fd, unsigned copy);
#endif
