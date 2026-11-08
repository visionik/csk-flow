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
 *   csk-flow [-v] [--json] [--force] [--no-console] -s <port> [-b baud]
 *            [-C venus] [--console-baud N] [--until REGEX] [--timeout MS]
 *            <addr> <file> [<addr> <file> ...]
 *
 * See README.md for the flags. The flash/console core lives in ops.c, shared
 * with the daemon.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ops.h"

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
			chip = csk_parse_chip(argv[++i]);
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

	int flash_ret = csk_flash(port, chip, flash_baud, parts, nparts, force,
			chip_id, &have_id, results);

	char matched[256] = "";
	int console_rc = 1, lines = 0;
	if (flash_ret == 0 && observe)
		console_rc = csk_console(port, console_baud, until, timeout_ms, json,
				matched, sizeof(matched), &lines);
	if (!observe)
		until = NULL; /* nothing to match on when not observing */

	if (json)
		csk_fprint_json(stdout, flash_ret == 0, have_id, chip_id, parts,
				results, nparts, until, console_rc, matched, lines);

	if (flash_ret != 0)
		return 1;
	return console_rc == 0 || !until ? 0 : 1;
}
