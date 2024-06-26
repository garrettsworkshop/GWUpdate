#include <Windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifndef _CH340G_HAL_H
#define _CH340G_HAL_H

#include "gwu_time.h"
#include "gwu_console.h"

#define CLKCHAR_1 0x00 // 00000000 -> ...10111111111...
#define CLKCHAR_2 0x40 // 01000000 -> ...10101111111...
#define CLKCHAR_3 0x50 // 01010000 -> ...10101011111...
#define CLKCHAR_4 0x54 // 01010100 -> ...10101010111...
#define CLKCHAR_5 0x55 // 01010101 -> ...10101010101...

char portname[16] = { 0 };
HANDLE serialport = NULL;

static void io_tms(int val)
{
	if (!EscapeCommFunction(serialport, val ? CLRRTS : SETRTS)) {
		fprintf(stderr, "Error setting TMS on %s!\n", portname);
		quit(-1);
	}
}

static void io_tdi(int val)
{
	if (!EscapeCommFunction(serialport, val ? CLRDTR : SETDTR)) {
		fprintf(stderr, "Error setting TDI on %s!\n", portname);
		quit(-1);
	}
}

static void io_sendtck(char *buf, int len) {
	int written;
	int success = WriteFile(serialport, buf, len, &written, NULL);
	SetGate();
	if (!success) {
		fprintf(stderr, "Error pulsing TCK on %s!\n", portname);
		quit(-1);
	}
	if (written < len) {
		io_sendtck(&buf[written], len - written);
	}
}

#define TCKBUF_SIZ (32768)
char tckbuf[TCKBUF_SIZ];
static void io_tck(uint16_t count) {
	int fivecount = count / 5;
	int remainder = count % 5;
	switch (remainder) {
	case 1: tckbuf[fivecount] = CLKCHAR_1; break;
	case 2: tckbuf[fivecount] = CLKCHAR_2; break;
	case 3: tckbuf[fivecount] = CLKCHAR_3; break;
	case 4: tckbuf[fivecount] = CLKCHAR_4; break;
	default: break;
	}
	io_sendtck(tckbuf, fivecount + (remainder == 0 ? 0 : 1));
	tckbuf[fivecount] = CLKCHAR_5;
}

static int io_tdo()
{
	DWORD status;
	if (!GetCommModemStatus(serialport, &status)) {
		fprintf(stderr, "Error reading TDO from %s!\n", portname);
		quit(-1);
	}
	return (status & MS_CTS_ON) ? 0 : 1;
}

static int io_dsr()
{
	DWORD status;
	if (!GetCommModemStatus(serialport, &status)) {
		fprintf(stderr, "Error reading DSR from %s!\n", portname);
		quit(-1);
	}
	return (status & MS_DSR_ON) ? 0 : 1;
}

static int io_ri()
{
	DWORD status;
	if (!GetCommModemStatus(serialport, &status)) {
		fprintf(stderr, "Error reading RI from %s!\n", portname);
		quit(-1);
	}
	return (status & MS_RING_ON) ? 0 : 1;
}

static int io_dcd()
{
	DWORD status;
	if (!GetCommModemStatus(serialport, &status)) {
		fprintf(stderr, "Error reading DCD from %s!\n", portname);
		quit(-1);
	}
	return (status & MS_RLSD_ON) ? 0 : 1;
}

static void io_setup(void)
{
	char name[100] = { 0 };
	char root[] = "\\\\.\\\0";
	memcpy(name, root, strlen(root));
	memcpy(name + strlen(root), portname, strlen(portname));

	memset(tckbuf, CLKCHAR_5, TCKBUF_SIZ);
	SetupTicks();

	serialport = CreateFileA(
		name,							// Port name
		GENERIC_READ | GENERIC_WRITE,	// Read & Write
		0,								// No sharing
		NULL,							// No security
		OPEN_EXISTING,					// Open existing port
		0,								// Non-overlapped I/O
		NULL);							// Null for comm devices

	if (serialport == INVALID_HANDLE_VALUE) { goto error; }

	DCB dcb;
	SecureZeroMemory(&dcb, sizeof(DCB));
	dcb.DCBlength = sizeof(DCB);

	if (!GetCommState(serialport, &dcb)) { goto error; }
	dcb.BaudRate = 2000000;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fTXContinueOnXoff = TRUE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fNull = FALSE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fAbortOnError = TRUE;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	if (!SetCommState(serialport, &dcb)) { goto error; }

	io_tms(1);
	io_tdi(1);

	if (!EscapeCommFunction(serialport, CLRBREAK)) { goto error; }
	Sleep(100);
	if (!EscapeCommFunction(serialport, SETBREAK)) { goto error; }
	Sleep(100);
	if (!EscapeCommFunction(serialport, CLRBREAK)) { goto error; }
	Sleep(100);
	if (!EscapeCommFunction(serialport, SETBREAK)) { goto error; }
	Sleep(100);
	if (!EscapeCommFunction(serialport, CLRBREAK)) { goto error; }
	Sleep(100);

	return;

error:
	fprintf(stderr, "Error opening %s!\n", portname);
	if (serialport != INVALID_HANDLE_VALUE) { CloseHandle(serialport); }
	quit(-1);
}

static void io_shutdown(void)
{
	Sleep(100);
	CloseHandle(serialport);
	Sleep(100);
}

#endif
