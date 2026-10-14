/* SPDX-License-Identifier: MIT */
volatile unsigned trace;
int FixtureValue(void) { return 7; }
int OrdinalValue(void) { return 11; }
int __stdcall DllMain(void *module, unsigned reason, void *reserved) {
    (void)module; (void)reserved;
    trace = trace * 10 + reason;
#ifdef FAIL_ATTACH
    if (reason == 1) return 0;
#endif
    return 1;
}
