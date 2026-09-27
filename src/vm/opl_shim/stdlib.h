/* Freestanding stand-in for DBOPL's <stdlib.h> in CVSESSION ring 0. */
#pragma once
#include <stddef.h>
static inline long labs(long value) { return value < 0 ? -value : value; }
