/*
 * csk-flowd: a persistent RPC endpoint for csk-flow.
 *
 * Listens on a Unix socket and serves one newline-delimited command per
 * connection, replying with one JSON line. It reuses the flash/console core
 * in ops.c, so an orchestrator drives flashing through a single long-lived
 * endpoint instead of re-spawning the CLI.
 *
 * NOTE: the C62 cable has no reset lines, so this does NOT make flashing
 * unattended -- a "flash" still needs a human to hold BOOT + power on, and a
 * "console" needs a power-cycle. The daemon's value is the single endpoint
 * and structured results, not hands-free operation.
 *
 * Commands (plain text, one per line; arguments are whitespace-separated):
 *   flash [--force] <addr> <file> [<addr> <file> ...]
 *   console [--until REGEX] [--timeout MS]
 *   ping
 *   quit
 * Replies are one JSON object per command.
 *
 * usage: csk-flowd -S <socket-path> -s <serial-port> [-C venus] [-b baud]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#include "ops.h"

static const char *g_port;
static cskburn_serial_chip_t g_chip = CHIP_VENUS;
static uint32_t g_flash_baud = DEFAULT_FLASH_BAUD;

/* Split a command line in place into argv; returns argc. */
static int tokenize(char *line, char **argv, int max)
{
	int argc = 0;
	char *p = line;
	while (*p && argc < max) {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (*p == '\0')
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	return argc;
}

static void cmd_flash(FILE *out, int argc, char **argv)
{
	int force = 0, i = 1;
	struct part parts[MAX_PARTS];
	int nparts = 0;

	for (; i < argc; i++) {
		if (strcmp(argv[i], "--force") == 0)
			force = 1;
		else
			break;
	}
	for (; i + 1 < argc && nparts < MAX_PARTS; i += 2) {
		parts[nparts].addr = (uint32_t)strtoul(argv[i], NULL, 0);
		parts[nparts].file = argv[i + 1];
		nparts++;
	}
	if (nparts == 0) {
		fprintf(out, "{\"result\": \"error\", \"error\": \"usage: flash "
					 "[--force] <addr> <file> ...\"}\n");
		return;
	}

	uint8_t chip_id[8];
	int have_id = 0;
	struct part_result results[MAX_PARTS];
	for (int k = 0; k < nparts; k++)
		results[k].action = NULL;

	int ret = csk_flash(g_port, g_chip, g_flash_baud, parts, nparts, force,
			chip_id, &have_id, results);
	csk_fprint_json(out, ret == 0, have_id, chip_id, parts, results, nparts,
			NULL, 1, NULL, 0);
}

static void cmd_console(FILE *out, int argc, char **argv)
{
	const char *until = NULL;
	uint64_t timeout_ms = DEFAULT_TIMEOUT_MS;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--until") == 0 && i + 1 < argc)
			until = argv[++i];
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			timeout_ms = strtoull(argv[++i], NULL, 10);
	}

	char matched[256] = "";
	int lines = 0;
	int rc = csk_console(g_port, DEFAULT_CONSOLE_BAUD, until, timeout_ms, 1,
			matched, sizeof(matched), &lines);

	fprintf(out, "{\"console\": {\"lines\": %d, \"matched\": ", lines);
	if (until && rc == 0)
		fprintf(out, "\"%s\"", matched);
	else
		fprintf(out, "null");
	fprintf(out, "}, \"result\": \"%s\"}\n",
			rc == 0 ? "matched" : rc == 1 ? "timeout" : "error");
}

static int serve_conn(int fd)
{
	FILE *in = fdopen(dup(fd), "r");
	FILE *out = fdopen(dup(fd), "w");
	if (!in || !out) {
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		return 0;
	}
	setvbuf(out, NULL, _IOLBF, 0);

	char line[4096];
	int stop = 0;
	while (fgets(line, sizeof(line), in)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *argv[2 * MAX_PARTS + 4];
		int argc = tokenize(line, argv, (int)(sizeof(argv) / sizeof(argv[0])));
		if (argc == 0)
			continue;

		if (strcmp(argv[0], "ping") == 0)
			fprintf(out, "{\"result\": \"pong\"}\n");
		else if (strcmp(argv[0], "flash") == 0)
			cmd_flash(out, argc, argv);
		else if (strcmp(argv[0], "console") == 0)
			cmd_console(out, argc, argv);
		else if (strcmp(argv[0], "quit") == 0) {
			fprintf(out, "{\"result\": \"bye\"}\n");
			stop = 1;
			break;
		} else
			fprintf(out, "{\"result\": \"error\", \"error\": \"unknown command "
						 "'%s'\"}\n",
					argv[0]);
		fflush(out);
	}
	fclose(in);
	fclose(out);
	return stop;
}

int main(int argc, char **argv)
{
	const char *sock_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-S") == 0 && i + 1 < argc)
			sock_path = argv[++i];
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			g_port = argv[++i];
		else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
			g_chip = csk_parse_chip(argv[++i]);
		else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
			g_flash_baud = (uint32_t)strtoul(argv[++i], NULL, 10);
	}

	if (!sock_path || !g_port) {
		fprintf(stderr,
				"usage: csk-flowd -S <socket-path> -s <serial-port> "
				"[-C venus] [-b baud]\n");
		return 2;
	}

	set_log_level(LOGLEVEL_INFO);

	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
	unlink(sock_path);
	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	if (listen(srv, 4) < 0) {
		perror("listen");
		return 1;
	}
	fprintf(stderr, "csk-flowd listening on %s (port %s)\n", sock_path, g_port);

	int stop = 0;
	while (!stop) {
		int c = accept(srv, NULL, NULL);
		if (c < 0)
			continue;
		stop = serve_conn(c);
		close(c);
	}

	close(srv);
	unlink(sock_path);
	return 0;
}
