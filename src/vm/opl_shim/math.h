/* Freestanding stand-in for DBOPL's <math.h>: x87 versions in session_opl.cpp. */
#pragma once
extern "C" double pow(double x, double y);
extern "C" double sin(double x);
extern "C" double log10(double x);
