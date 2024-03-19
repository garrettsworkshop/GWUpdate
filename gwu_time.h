#include <Windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifndef _GWU_TIME_H
#define _GWU_TIME_H

LONGLONG ticks_per_gate;
LONGLONG ticks_per_ms;
static void SetupTicks() {
	LARGE_INTEGER ticks_per_sec;
	QueryPerformanceFrequency(&ticks_per_sec);
	ticks_per_ms = (ticks_per_sec.QuadPart + 999) / 1000;
	ticks_per_gate = ticks_per_sec.QuadPart / 1500;
}

static LONGLONG GetTicksNow() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

LONGLONG last;
char last_set = 0;
static void SetGate() {
	last = GetTicksNow();
}
static void Gate() {
	if (!last_set) { SetGate(); }
	LONGLONG end = last + ticks_per_gate;
	do { last = GetTicksNow(); } while (last < end);
}

static void Wait() {
	LONGLONG end = GetTicksNow() + ticks_per_gate;
	while (GetTicksNow() < end);
}

#endif