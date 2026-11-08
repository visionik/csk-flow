/*
 * csk-flow: agent-friendly flash+observe for CSK6 radios.
 *
 * Links libcskburn_serial (LISTENAI/cskburn, Apache-2.0) for the burn
 * protocol, then holds the port and streams the console itself so the boot
 * window is never missed.
 *
 * The C62's USB-serial cable does NOT wire DTR/RTS to BOOT/RESET (tested:
 * all reset strategies fail), so burn mode is entered by hand (hold BOOT,
 * power on) and the chip is power-cycled by hand after a flash. csk-flow
 * waits for the device, flashes, then opens the console and is already
 * listening when you power-cycle -- no race, no separate console tool.
 *
 * usage:
 *   csk-flow [-v] [--json] [--force] -s <port> [-b <flash-baud>] [-C venus]
 *            [--console-baud N] [--until REGEX] [--timeout MS]
 *            <addr> <file> [<addr> <file> ...]
 *
 * --force    always write; otherwise a partition whose on-device MD5 already
 *            matches the file is skipped (~35 s -> ~1 s on an unchanged image).
 * --json     emit one JSON result object on stdout; console lines are counted,
 *            not streamed. Human prompts always go to stderr.
 */

#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cskburn_serial.h"
#include "fsio.h"
#include "log.h"
#include "md5.h"
#include "serial.h"

#define DEFAULT_FLASH_BAUD 115200
#define DEFAULT_CONSOLE_BAUD 115200
#define DEFAULT_TIMEOUT_MS 180000
#define RESET_DELAY_MS 500
#define PROBE_TIMEOUT_MS 60000 /* generous: human holds BOOT + powers on */
#define MAX_PARTS 8

struct part {
	uint32_t addr;
	const char *file;
};

struct part_result {
	uint32_t addr;
	long bytes;
	const char *action; /* "written", "skipped", "failed" */
	uint32_t ms;
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_progress(int32_t wrote, uint32_t total)
{
	if (total)
		fprintf(stderr, "\r  %u / %u bytes (%u%%)", (unsigned)wrote, total,
				(unsigned)((uint64_t)wrote * 100 / total));
}

static void hex16(char *dst, const uint8_t *b, int n)
{
	static const char *h = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		dst[i * 2] = h[b[i] >> 4];
		dst[i * 2 + 1] = h[b[i] & 0xf];
	}
	dst[n * 2] = '\0';
}

/* Write one partition, or skip it when the device already holds this data. */
static int flash_part(cskburn_serial_device_t *dev, struct part *p, int force,
		struct part_result *res)
{
	res->addr = p->addr;
	res->bytes = 0;
	res->ms = 0;

	uint8_t want[16], have[16];
	long fsize = 0;

	if (!force && md5_file(p->file, want, &fsize) == 0 && fsize > 0 &&
			cskburn_serial_verify(dev, TARGET_FLASH, p->addr, (uint32_t)fsize,
					have) == 0 &&
			memcmp(want, have, 16) == 0) {
		res->bytes = fsize;
		res->action = "skipped";
		fprintf(stderr, "skip  %s -> 0x%08X (%ld bytes, MD5 matches)\n", p->file,
				p->addr, fsize);
		return 0;
	}

	reader_t *rd = filereader_open(p->file);
	if (rd == NULL) {
		fprintf(stderr, "cannot open %s\n", p->file);
		res->action = "failed";
		return -1;
	}
	fprintf(stderr, "write %s -> 0x%08X (%u bytes)\n", p->file, p->addr,
			rd->size);

	uint64_t t0 = now_ms();
	int ret = cskburn_serial_write(dev, TARGET_FLASH, p->addr, rd, 0,
			on_progress);
	res->bytes = rd->size;
	rd->close(&rd);
	fprintf(stderr, "\n");

	res->ms = (uint32_t)(now_ms() - t0);
	if (ret != 0) {
		fprintf(stderr, "write failed: %d\n", ret);
		res->action = "failed";
		return ret;
	}
	res->action = "written";
	return 0;
}

static int do_flash(const char *port, cskburn_serial_chip_t chip, uint32_t baud,
		struct part *parts, int nparts, int force, uint8_t chip_id[8],
		int *have_id, struct part_result *results)
{
	cskburn_serial_device_t *dev = NULL;
	int ret;

	*have_id = 0;

	ret = cskburn_serial_open(&dev, port, chip, 5000);
	if (ret != 0) {
		fprintf(stderr, "open(%s) failed: %d\n", port, ret);
		return ret;
	}

	fprintf(stderr,
			">>> Put the radio in BURN MODE now (hold BOOT, power on). "
			"Waiting up to %d s...\n",
			PROBE_TIMEOUT_MS / 1000);
	ret = cskburn_serial_connect(dev, RESET_DELAY_MS, PROBE_TIMEOUT_MS,
			CSKBURN_RESET_DTR_BOOT);
	if (ret != 0) {
		fprintf(stderr, "connect/probe failed: %d (did it enter burn mode?)\n",
				ret);
		goto out;
	}

	ret = cskburn_serial_enter(dev, baud, NULL, 0); /* NULL = embedded burner */
	if (ret != 0) {
		fprintf(stderr, "enter burner failed: %d\n", ret);
		goto out;
	}

	if (cskburn_serial_read_chip_id(dev, chip_id) == 0) {
		char s[17];
		hex16(s, chip_id, 8);
		fprintf(stderr, "chip-id: %s\n", s);
		*have_id = 1;
	}

	for (int i = 0; i < nparts; i++) {
		ret = flash_part(dev, &parts[i], force, &results[i]);
		if (ret != 0)
			goto out;
	}
	fprintf(stderr, "flash OK\n");

out:
	cskburn_serial_close(&dev); /* release the port for the console phase */
	return ret;
}

static int do_console(const char *port, uint32_t baud, const char *until,
		uint64_t timeout_ms, int json, char *matched_out, size_t matched_cap,
		int *lines_out)
{
	serial_dev_t *s = NULL;
	regex_t re;
	bool have_re = false;

	*lines_out = 0;
	if (matched_cap)
		matched_out[0] = '\0';

	if (until && regcomp(&re, until, REG_EXTENDED | REG_NOSUB) == 0)
		have_re = true;
	else if (until)
		fprintf(stderr, "bad --until regex: %s\n", until);

	if (serial_open(port, &s) != 0) {
		fprintf(stderr, "console open(%s) failed\n", port);
		if (have_re)
			regfree(&re);
		return -1;
	}
	serial_set_speed(s, baud);

	fprintf(stderr, ">>> Listening at %u baud. POWER-CYCLE the radio now.%s\n",
			baud, until ? " (stops on --until match)" : "");

	uint64_t start = now_ms();
	char line[2048];
	size_t ll = 0;
	uint8_t buf[1024];
	int rc = 1; /* 1 = timed out without match */

	while (now_ms() - start < timeout_ms) {
		ssize_t r = serial_read(s, buf, sizeof(buf), 100);
		if (r <= 0)
			continue;
		for (ssize_t i = 0; i < r; i++) {
			char c = (char)buf[i];
			if (c == '\n' || ll == sizeof(line) - 1) {
				line[ll] = '\0';
				(*lines_out)++;
				if (!json)
					printf("[%7.3f] %s\n", (now_ms() - start) / 1000.0, line);
				else
					fflush(stdout);
				if (have_re && regexec(&re, line, 0, NULL, 0) == 0) {
					if (matched_cap) {
						strncpy(matched_out, line, matched_cap - 1);
						matched_out[matched_cap - 1] = '\0';
					}
					fprintf(stderr, ">>> matched --until\n");
					rc = 0;
					goto done;
				}
				ll = 0;
			} else if (c != '\r') {
				line[ll++] = c;
			}
		}
	}
	fprintf(stderr, ">>> console timeout after %llu ms\n",
			(unsigned long long)timeout_ms);
done:
	if (!json)
		fflush(stdout);
	if (have_re)
		regfree(&re);
	serial_close(&s);
	return rc;
}

static void json_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 2 < cap; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = c;
		} else if (c >= 0x20) {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

static void print_json(int flash_ok, int have_id, const uint8_t *chip_id,
		struct part *parts, struct part_result *results, int nparts,
		const char *until, int console_rc, const char *matched, int lines)
{
	char idstr[17] = "";
	if (have_id)
		hex16(idstr, chip_id, 8);

	printf("{\n");
	if (have_id)
		printf("  \"chip_id\": \"%s\",\n", idstr);
	else
		printf("  \"chip_id\": null,\n");
	printf("  \"flash\": \"%s\",\n", flash_ok ? "ok" : "failed");
	printf("  \"partitions\": [\n");
	for (int i = 0; i < nparts; i++) {
		printf("    {\"addr\": \"0x%08X\", \"bytes\": %ld, \"action\": \"%s\", "
			   "\"ms\": %u}%s\n",
				parts[i].addr, results[i].bytes,
				results[i].action ? results[i].action : "not-reached",
				results[i].ms, i + 1 < nparts ? "," : "");
	}
	printf("  ],\n");

	printf("  \"console\": {\"lines\": %d, \"matched\": ", lines);
	if (until && console_rc == 0 && matched) {
		char mesc[256];
		json_escape(matched, mesc, sizeof(mesc));
		printf("\"%s\"", mesc);
	} else {
		printf("null");
	}
	printf("},\n");
	const char *result = !flash_ok ? "flash-failed"
			: !until               ? "ok"
			: console_rc == 0      ? "matched"
								   : "console-timeout";
	printf("  \"result\": \"%s\"\n", result);
	printf("}\n");
}

static cskburn_serial_chip_t parse_chip(const char *s)
{
	if (strcmp(s, "venus") == 0 || strcmp(s, "6") == 0)
		return CHIP_VENUS;
	if (strcmp(s, "castor") == 0)
		return CHIP_CASTOR;
	if (strcmp(s, "arcs") == 0)
		return CHIP_ARCS;
	if (strcmp(s, "venusa") == 0 || strcmp(s, "7") == 0)
		return CHIP_VENUSA;
	return CHIP_VENUS;
}

int main(int argc, char **argv)
{
	const char *port = NULL, *until = NULL;
	cskburn_serial_chip_t chip = CHIP_VENUS;
	uint32_t flash_baud = DEFAULT_FLASH_BAUD;
	uint32_t console_baud = DEFAULT_CONSOLE_BAUD;
	uint64_t timeout_ms = DEFAULT_TIMEOUT_MS;
	int verbose = 0, json = 0, force = 0, observe = 1;
	struct part parts[MAX_PARTS];
	int nparts = 0;

	int i = 1;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			verbose++;
		else if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (strcmp(argv[i], "--force") == 0)
			force = 1;
		else if (strcmp(argv[i], "--no-console") == 0)
			observe = 0;
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			port = argv[++i];
		else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
			flash_baud = (uint32_t)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
			chip = parse_chip(argv[++i]);
		else if (strcmp(argv[i], "--console-baud") == 0 && i + 1 < argc)
			console_baud = (uint32_t)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc)
			until = argv[++i];
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			timeout_ms = strtoull(argv[++i], NULL, 10);
		else
			break; /* rest are addr/file pairs */
	}

	for (; i + 1 < argc && nparts < MAX_PARTS; i += 2) {
		parts[nparts].addr = (uint32_t)strtoul(argv[i], NULL, 0);
		parts[nparts].file = argv[i + 1];
		nparts++;
	}

	if (!port || nparts == 0) {
		fprintf(stderr,
				"usage: csk-flow [-v] [--json] [--force] [--no-console] "
				"-s <port> [-b baud] [-C venus] [--console-baud N] "
				"[--until REGEX] [--timeout MS] "
				"<addr> <file> [<addr> <file> ...]\n");
		return 2;
	}

	/* Default to INFO; the library leaves the level at TRACE (0) otherwise,
	 * which floods the probe. -v -> DEBUG, -vv -> TRACE. */
	set_log_level(verbose >= 2 ? LOGLEVEL_TRACE
			: verbose == 1    ? LOGLEVEL_DEBUG
							  : LOGLEVEL_INFO);

	uint8_t chip_id[8];
	int have_id = 0;
	struct part_result results[MAX_PARTS];
	for (int k = 0; k < nparts; k++)
		results[k].action = NULL;

	int flash_ret = do_flash(port, chip, flash_baud, parts, nparts, force,
			chip_id, &have_id, results);

	char matched[256] = "";
	int console_rc = 1, lines = 0;
	if (flash_ret == 0 && observe)
		console_rc = do_console(port, console_baud, until, timeout_ms, json,
				matched, sizeof(matched), &lines);
	if (!observe)
		until = NULL; /* nothing to match on when not observing */

	if (json)
		print_json(flash_ret == 0, have_id, chip_id, parts, results, nparts,
				until, console_rc, matched, lines);

	if (flash_ret != 0)
		return 1;
	return console_rc == 0 || !until ? 0 : 1;
}
