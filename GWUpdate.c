/*
 *  GWUpdate
 *  based on...
 *  Lib(X)SVF  -  A library for implementing SVF and XSVF JTAG players
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "libxsvf.h"
#include "comsearch.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "CH340G-HAL.h"
#include "CH340G-time.h"
#include "CH340G-quit.h"

uint32_t expected_devices;
uint32_t expected_idcode;
uint32_t found_devices;
uint32_t found_idcode;
uint32_t expected_bits;
enum libxsvf_mode cur_mode;
char enable_vt;

struct udata_s {
	FILE* f;
	int clockcount;
	int bitcount_tdi;
	int bitcount_tdo;
	int sendcount;
};
static struct udata_s u;

LONGLONG start = 0;
void printinfo() {
	LONGLONG end = GetTicksNow() - start;
	double elapsed = (double)end / ticks_per_ms / 1000.0f;
	fprintf(stderr, "\n");
	fprintf(stderr, "Total number of clock cycles: %d\n", u.clockcount);
	fprintf(stderr, "Number of significant TDI bits: %d\n", u.bitcount_tdi);
	fprintf(stderr, "Number of significant TDO bits: %d\n", u.bitcount_tdo);
	fprintf(stderr, "Number of TCK pulsetrains: %d\n", u.sendcount);
	fprintf(stderr, "Time elapsed: %lf sec.\n", elapsed);
	fprintf(stderr, "Speed: %lf bits / sec.\n", (double)u.clockcount / elapsed);
	fprintf(stderr, "\n");
}
void printshortinfo() {
	if (cur_mode != LIBXSVF_MODE_SCAN) {
		LONGLONG end = GetTicksNow() - start;
		double elapsed = (double)end / ticks_per_ms / 1000.0f;
		if (enable_vt) { fprintf(stderr, "\033[1A\033[K"); }
		float percent = 100.0f * (float)u.clockcount / expected_bits;
		if (percent > 100.0f) { percent = 100.0f; }
		fprintf(stderr, "Update in progress... %-4.1f%%      Bits: %d      Time: %.1f sec.      Speed: %.1f b/sec.\n",
			percent, u.clockcount, elapsed, (double)u.clockcount / elapsed);
	}
}

unsigned char tck_queue = 0;

static void flush_tck() {
	io_tck(tck_queue);
	tck_queue = 0;
}

static int h_setup(struct libxsvf_host* h)
{
	static char printed = 0;
	struct udata_s* u = h->user_data;
	if (!printed) {
		fprintf(stderr, "Opening JTAG connection...\n\n");
		fflush(stderr);
		printed = 1;
	}
	tck_queue = 0;
	io_setup();
	return 0;
}

static int h_shutdown(struct libxsvf_host* h)
{
	struct udata_s* u = h->user_data;
	if (cur_mode != LIBXSVF_MODE_SCAN) {
		fprintf(stderr, "Closing JTAG connection...\n\n");
		fflush(stderr);
	}
	flush_tck();
	io_shutdown();
	return 0;
}

static void h_udelay(struct libxsvf_host* h, long usecs, int tms, long num_tck)
{
	struct udata_s* u = h->user_data;
	if (num_tck > 0) {
		io_tms(tms);
		while (num_tck > 255) {
			io_tck(255);
			num_tck -= 255;
		}
		io_tck((unsigned char)num_tck);
	}
	if (usecs > 0) { Sleep((usecs + 999) / 1000); }
}

static int h_getbyte(struct libxsvf_host* h)
{
	struct udata_s* u = h->user_data;
	char c = fgetc(u->f);
	return c;
}

static int h_set_frequency(struct libxsvf_host* h, int v) { return 0; }

static void h_report_tapstate(struct libxsvf_host* h)
{
	struct udata_s* u = h->user_data;
	const char* message = libxsvf_state2str(h->tap_state);
	char newmessage[40];
	memset(newmessage, ' ', sizeof(newmessage) - 1);
	newmessage[sizeof(newmessage) - 1] = 0;
	memcpy(newmessage, message, strlen(message));
	newmessage[strlen(message)] = ']';
	//fprintf(stderr, "[%s  ", newmessage);
	printshortinfo();
}

static void h_report_device(struct libxsvf_host* h, unsigned long idcode)
{
	printf("Found device on JTAG chain.      IDCODE=0x%08lx, REV=0x%01lx, PART=0x%04lx, MFR=0x%03lx\n",
		idcode, (idcode >> 28) & 0xf, (idcode >> 12) & 0xffff, (idcode >> 1) & 0x7ff);

	found_devices++;
	found_idcode = idcode;
	printshortinfo();
}

static void h_report_status(struct libxsvf_host* h, const char* message)
{
	struct udata_s* u = h->user_data;
	char newmessage[33];
	memset(newmessage, ' ', sizeof(newmessage) - 1);
	newmessage[sizeof(newmessage) - 1] = 0;
	if (strlen(message) < 33) {
		memcpy(newmessage, message, strlen(message));
		//fprintf(stderr, "[STATUS] %s ", newmessage);
	}
	else {
		//fprintf(stderr, "[STATUS] %s ", message);
	}
	printshortinfo();
}

static void h_report_error(struct libxsvf_host* h, const char* file, int line, const char* message)
{
	fprintf(stderr, "[%s:%d] %s\n\n", file, line, message);
}

static int realloc_maxsize[LIBXSVF_MEM_NUM];

static void* h_realloc(struct libxsvf_host* h, void* ptr, int size, enum libxsvf_mem which)
{
	struct udata_s* u = h->user_data;
	if (size > realloc_maxsize[which]) { realloc_maxsize[which] = size; }
	return realloc(ptr, size);
}

static int fast_enable = 0;
static int tms_old = -1;
static int tdi_old = -1;
static int h_pulse_tck(struct libxsvf_host* h, int tms, int tdi, int tdo, int rmask, int sync)
{
	struct udata_s* u = h->user_data;

	u->clockcount++;
	if (tdi >= 0) { u->bitcount_tdi++; }

	if (!fast_enable) {
		u->sendcount++;
		io_tms(tms);
		io_tdi(tdi);
		Sleep(1);
		if (!EscapeCommFunction(serialport, SETBREAK)) { goto error; }
		Sleep(1);
		if (!EscapeCommFunction(serialport, CLRBREAK)) { goto error; }
		Sleep(1);
		int line_tdo = io_tdo();
		return tdo < 0 || line_tdo == tdo ? line_tdo : -1;
	error:
		fprintf(stderr, "Error enabling COM port break condition!\n");
		quit(-1);
	}
	else if (!sync && tdo < 0 && tms == tms_old && (tdi == tdi_old || tdi < 0) && tck_queue < 255) {
		tck_queue++;
		return 1;
	}
	else {
		if (tck_queue > 0) {
			Gate();
			SetGate();
			flush_tck();
			SetGate();
			Gate();
			u->sendcount++;
			if (tms != tms_old || (tdi >= 0 && tdi != tdi_old)) { Gate(); }
		}

		if (tms != tms_old) {
			if (tdi < 0 || tdi == tdi_old) { SetGate(); }
			io_tms(tms);
			tms_old = tms;
		}
		if (tdi >= 0) {
			if (tdi != tdi_old) {
				SetGate();
				io_tdi(tdi);
				tdi_old = tdi;
			}
		}

		if (!sync && tdo < 0) {
			tck_queue++;
			return 1;
		}
		else {
			if (tdo >= 0) { u->bitcount_tdo++; }
			Gate();
			SetGate();
			io_tck(1);
			u->sendcount++;
			Gate();
			int line_tdo = io_tdo();
			return tdo < 0 || line_tdo == tdo ? line_tdo : -1;
		}
	}
}

static struct libxsvf_host h = {
	.udelay = h_udelay,
	.setup = h_setup,
	.shutdown = h_shutdown,
	.getbyte = h_getbyte,
	.pulse_tck = h_pulse_tck,
	.pulse_sck = NULL,
	.set_trst = NULL,
	.set_frequency = h_set_frequency,
	.report_tapstate = h_report_tapstate,
	.report_device = h_report_device,
	.report_status = h_report_status,
	.report_error = h_report_error,
	.realloc = h_realloc,
	.user_data = &u
};

static void copyleft()
{
	fprintf(stderr,
		"   ____                          _    _    _        __        __            _          _                   \n"
		"  / ___|  __ _  _ __  _ __  ___ | |_ | |_ ( )___    \\ \\      / /___   _ __ | | __ ___ | |__    ___   _ __   \n"
		" | |  _  / _` || '__|| '__|/ _ \\| __|| __||// __|    \\ \\ /\\ / // _ \\ | '__|| |/ // __|| '_ \\  / _ \\ | '_ \\  \n"
		" | |_| || (_| || |   | |  |  __/| |_ | |_   \\__ \\     \\ V  V /| (_) || |   |   < \\__ \\| | | || (_) || |_) | \n"
		"  \\____| \\__,_||_|   |_|   \\___| \\__| \\__|  |___/      \\_/\\_/  \\___/ |_|   |_|\\_\\|___/|_| |_| \\___/ |  __/  \n"
		"                    _   _           _       _         ____            _                             |_|    \n"
		"                   | | | |_ __   __| | __ _| |_ ___  / ___| _   _ ___| |_ ___ _ __ ___  \n"
		"                   | | | | '_ \\ / _` |/ _` | __/ _ \\ \\___ \\| | | / __| __/ _ \\ '_ ` _ \\ \n"
		"                   | |_| | |_) | (_| | (_| | ||  __/  ___) | |_| \\__ \\ ||  __/ | | | | | \n"
		"                    \\___/|  __/ \\__,_|\\__,_|\\__\\___| |____/ \\__, |___/\\__\\___|_| |_| |_|\n"
		"                         |_|                                |___/                       \n");
	fprintf(stderr, "Copyright (C) 2023 Garrett's Workshop\n");
	fprintf(stderr, "Based on xsvftool-gpio, part of Lib(X)SVF (http://www.clifford.at/libxsvf/).\n");
	fprintf(stderr, "Copyright (C) 2009  RIEGL Research ForschungsGmbH\n");
	fprintf(stderr, "Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>\n");
	fprintf(stderr, "Lib(X)SVF is free software licensed under the ISC license.\n");
	fprintf(stderr, "GWUpdate is free software licensed under the ISC license.\n\n");
	fprintf(stderr, "Loading...");
#ifndef _DEBUG
	for (int i = 0; i < 10; i++) {
		fputc('.', stderr);
		Sleep(1000);
	}
#endif
	if (enable_vt) {
		fputc('\n', stderr);
		for (int i = 0; i < 9; i++) { fprintf(stderr, "\033[0K\033[1A"); }
	}
	fputc('\n', stderr);
	fputc('\n', stderr);
}

#define STRBUF_SIZE (64 * 1024)
char strbuf[STRBUF_SIZE];

typedef enum boardid_digit_e {
	BOARDID_DIGIT_0 = 0,
	BOARDID_DIGIT_1 = 1,
	BOARDID_DIGIT_DTR = 2,
	BOARDID_DIGIT_RTS = 3,
	BOARDID_DIGIT_DONTCARE = -1,
} boardid_digit_t;

int check_boardid_digit(boardid_digit_t(*get)(), boardid_digit_t expected) {
	io_tdi(0); io_tms(0);
	char func_type = get() & 1;
	io_tdi(0); io_tms(1);
	func_type = (func_type << 1) | (get() & 1);
	io_tdi(1); io_tms(0);
	func_type = (func_type << 1) | (get() & 1);
	io_tdi(1); io_tms(1);
	func_type = (func_type << 1) | (get() & 1);

	switch (expected) {
	case BOARDID_DIGIT_0: return func_type == 0;
	case BOARDID_DIGIT_1: return func_type == 1;
	case BOARDID_DIGIT_DTR:
		io_tdi(1);
		if (get() != 1) { return -1; }
		io_tdi(0);
		if (get() != 0) { return -1; }
		io_tdi(1);
	case BOARDID_DIGIT_RTS:
		io_tms(1);
		if (get() != 1) { return -1; }
		io_tms(0);
		if (get() != 0) { return -1; }
		io_tms(1);
	}
	return 0;
}

int main(int argc, char** argv)
{
	enum libxsvf_mode mode;
	int portnum;

	// Enable ASCII terminal control codes
	HANDLE hConsole = GetStdHandle(STD_ERROR_HANDLE);
	DWORD consoleMode;
	GetConsoleMode(hConsole, &consoleMode);
	consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	if (SetConsoleMode(hConsole, consoleMode)) { enable_vt = 1; }
	else { enable_vt = 0; }

	// Display copyright message
	copyleft();

	// Check for correct number of arguments
	if (argc != 1) {
		fprintf(stderr, "Error! Bad arguments.\n");
		return quit(-1);
	}

	// Open data file
#ifndef _DEBUG
	u.f = fopen(argv[0], "rb");
#else
	u.f = fopen("Packager/GWUpdate_out.exe", "rb");
#endif

	if (!u.f) {
		fprintf(stderr,
#ifndef _DEBUG
			"Error! Failed to open GWUpdate executable as data file."
#else
			"Error! Failed to open update data file."
#endif
			"\n");
		return quit(-1);
	}

	// Find XSVF data
	for (int i = 1; ; i++) { // Search at each 128k offset for SVF/XSVF flag
		if (i > 255) { // Looked too many times fail
			fprintf(stderr, "Error! (X)SVF flag not found.\n");
			return quit(-1);
		}
		if (fseek(u.f, i * 128 * 1024, SEEK_SET)) { // Seek past end of file fail
			fprintf(stderr, "Error! Seeked past end of file looking for (X)SVF flag.\n");
			return quit(-1);
		}

		// Check for flag
		char c = fgetc(u.f);
		if (c == 'X') { mode = LIBXSVF_MODE_XSVF; } // First 'X' for XSVF
		else if (c == ' ') { mode = LIBXSVF_MODE_SVF; } // First ' ' for SVF
		else { continue; } // If neither try next 128k
		// Then 'S', 'V', 'F', else try next 128k
		if (fgetc(u.f) != 'S') { continue; }
		if (fgetc(u.f) != 'V') { continue; }
		if (fgetc(u.f) != 'F') { continue; }
		break;
	}

	// Get boardid digits
	boardid_digit_t boardid_dsr;
	boardid_digit_t boardid_ri;
	boardid_digit_t boardid_dcd;
	if (!fread(&boardid_dsr, sizeof(boardid_digit_t), 1, u.f)) {
		fprintf(stderr, "Error! Could not read boardid digit DSR from update file.\n");
		return quit(-1);
	}
	if (!fread(&boardid_ri, sizeof(boardid_digit_t), 1, u.f)) {
		fprintf(stderr, "Error! Could not read boardid digit RI from update file.\n");
		return quit(-1);
	}
	if (!fread(&boardid_dcd, sizeof(boardid_digit_t), 1, u.f)) {
		fprintf(stderr, "Error! Could not read boardid digit DCD from update file.\n");
		return quit(-1);
	}

	// Get expected bit count from update file
	if (!fread(&expected_bits, sizeof(uint32_t), 1, u.f)) {
		fprintf(stderr, "Error! Could not read expected bit count from update file.\n");
		return quit(-1);
	}

	// Ensure update file indicates only one device on JTAG chain
	if (!fread(&expected_devices, sizeof(uint32_t), 1, u.f)) {
		fprintf(stderr, "Error! Could not read JTAG device count from update file.\n");
		return quit(-1);
	}
	// Only supporting one device on chain...
	if (expected_devices > 1) {
		fprintf(stderr, "Error! Update file has multiple devices on JTAG chain but GWUpdate only supports one device.\n");
		return quit(-1);
	}
	else if (expected_devices == 0) {
		fprintf(stderr, "Error! Update file has no devices on JTAG chain.\n");
		return quit(-1);
	}
	found_devices = 0; // Reset found devices


	// Read single expected IDCODE from update file
	if (fread(&expected_idcode, sizeof(uint32_t), 1, u.f) != 1) { // Couldn't read idcode
		fprintf(stderr, "Error! Couldn't read JTAG idcode from file.\n");
		return quit(-1);
	}

	// Print first instructions text from update file
	while (1) {
		int c = fgetc(u.f);
		if (c == EOF || c == 0) { break; }
		else { fputc(c, stderr); }
	}

	getchar();

	comsearch(portname);

	// Print second instructions text from update file
	while (1) {
		int c = fgetc(u.f);
		if (c == EOF || c == 0) { break; }
		else { fputc(c, stderr); }
	}

	getchar();

	// Find COM port
	if ((portnum = compick(portname)) <= 0) {
		fprintf(stderr, "Error! Could not find USB device.\n");
		return quit(-1);
	}

	// Check for expected board ID
	if (check_boardid_digit(io_dsr, boardid_dsr) ||
		check_boardid_digit(io_ri, boardid_ri) ||
		check_boardid_digit(io_dcd, boardid_dcd)) {
		fprintf(stderr, "Error! Wrong board ID.\n");
		return quit(-1);
	}

	// Start elapsed time timer
	SetupTicks();
	start = GetTicksNow();

	// Scan JTAG chain
	cur_mode = LIBXSVF_MODE_SCAN;
	if (libxsvf_play(&h, LIBXSVF_MODE_SCAN) < 0) {
		fprintf(stderr, "Error! Failed to scan JTAG chain.\n");
		return quit(-1);
	}

	// Check for expected IDCODE
#ifndef _DEBUG
	if (expected_idcode != 0 && expected_idcode != found_idcode) {
		fprintf(stderr, "Error! Incorrect device found on JTAG chain.\n");
		return quit(-1);
	}
#endif

	fast_enable = 1;

	// Play update (X)SVF
	fputc('\n', stderr);
	cur_mode = mode;
	if (libxsvf_play(&h, mode) < 0) {
		fprintf(stderr, "Error! Failed to play (X)SVF.\n");
		printinfo();
		fprintf(stderr, "----------------------\n");
		fprintf(stderr, "| Programming FAILED |\n");
		fprintf(stderr, "----------------------\n");
		return quit(-1);
	}
	else {
		printinfo();
		fprintf(stderr, "--------------------------\n");
		fprintf(stderr, "| Programming SUCCESSFUL |\n");
		fprintf(stderr, "--------------------------\n");
	}

	LONGLONG end = GetTicksNow() - start;
	double elapsed = (double)end / ticks_per_ms / 1000.0f;

	// Close file
	fclose(u.f);

	return quit(0);
}
