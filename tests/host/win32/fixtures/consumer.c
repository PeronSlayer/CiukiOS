/* SPDX-License-Identifier: MIT */
__declspec(dllimport) int FixtureValue(void);
__declspec(dllimport) int OrdinalValue(void);
volatile unsigned trace;
int entry(void) { return FixtureValue()+OrdinalValue(); }
int __stdcall DllMain(void *module, unsigned reason, void *reserved) {
    (void)module; (void)reserved;
    trace = trace * 10 + reason;
    if (reason == 1) trace += FixtureValue();
    return 1;
}
