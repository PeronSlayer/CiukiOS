/* Freestanding stand-in for DBOPL's <string.h>; memset is in session_opl.cpp. */
#pragma once
#include <stddef.h>
extern "C" void *memset(void *destination, int value, size_t count);
