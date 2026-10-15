/* Count production painter paths in the acceleration regression harness. */
#ifndef DESK_TEST_PAINT_PROBE_H
#define DESK_TEST_PAINT_PROBE_H
void desk_test_profile(const char *);
#define DESK_PROFILE(name, ...) do { desk_test_profile(name); __VA_ARGS__; } while (0)
#endif
