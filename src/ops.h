/*
 * csk-flow shared operations: flash (with incremental skip) and console
 * streaming, used by both the CLI (main.c) and the daemon (daemon.c).
 */
#ifndef CSK_FLOW_OPS_H
#define CSK_FLOW_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "cskburn_serial.h"

#define MAX_PARTS 8
#define DEFAULT_FLASH_BAUD 115200
#define DEFAULT_CONSOLE_BAUD 115200
#define DEFAULT_TIMEOUT_MS 180000
#define RESET_DELAY_MS 500
#define PROBE_TIMEOUT_MS 60000 /* generous: human holds BOOT + powers on */

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

uint64_t csk_now_ms(void);
void csk_hex16(char *dst, const uint8_t *b, int n);
cskburn_serial_chip_t csk_parse_chip(const char *s);

/*
 * Enter burn mode, read the chip id, and write each partition (skipping any
 * whose on-device MD5 already matches the file unless force). Fills chip_id /
 * have_id and results[0..nparts). Returns 0 on success.
 */
int csk_flash(const char *port, cskburn_serial_chip_t chip, uint32_t baud,
		struct part *parts, int nparts, int force, uint8_t chip_id[8],
		int *have_id, struct part_result *results);

/*
 * Open the port and stream the console until until matches or timeout_ms.
 * json != 0 suppresses the live timestamped print. Returns 0 matched,
 * 1 timeout, -1 error; fills matched_out and *lines_out.
 */
int csk_console(const char *port, uint32_t baud, const char *until,
		uint64_t timeout_ms, int json, char *matched_out, size_t matched_cap,
		int *lines_out);

/* Write one JSON result object to fp. */
void csk_fprint_json(void *fp, int flash_ok, int have_id,
		const uint8_t *chip_id, struct part *parts, struct part_result *results,
		int nparts, const char *until, int console_rc, const char *matched,
		int lines);

#endif
