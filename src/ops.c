#include "ops.h"

#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fsio.h"
#include "md5.h"
#include "serial.h"

uint64_t csk_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void csk_hex16(char *dst, const uint8_t *b, int n)
{
	static const char *h = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		dst[i * 2] = h[b[i] >> 4];
		dst[i * 2 + 1] = h[b[i] & 0xf];
	}
	dst[n * 2] = '\0';
}

cskburn_serial_chip_t csk_parse_chip(const char *s)
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

static void on_progress(int32_t wrote, uint32_t total)
{
	if (total)
		fprintf(stderr, "\r  %u / %u bytes (%u%%)", (unsigned)wrote, total,
				(unsigned)((uint64_t)wrote * 100 / total));
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

	uint64_t t0 = csk_now_ms();
	int ret = cskburn_serial_write(dev, TARGET_FLASH, p->addr, rd, 0,
			on_progress);
	res->bytes = rd->size;
	rd->close(&rd);
	fprintf(stderr, "\n");

	res->ms = (uint32_t)(csk_now_ms() - t0);
	if (ret != 0) {
		fprintf(stderr, "write failed: %d\n", ret);
		res->action = "failed";
		return ret;
	}
	res->action = "written";
	return 0;
}

int csk_flash(const char *port, cskburn_serial_chip_t chip, uint32_t baud,
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
		csk_hex16(s, chip_id, 8);
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

int csk_console(const char *port, uint32_t baud, const char *until,
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

	uint64_t start = csk_now_ms();
	char line[2048];
	size_t ll = 0;
	uint8_t buf[1024];
	int rc = 1; /* 1 = timed out without match */

	while (csk_now_ms() - start < timeout_ms) {
		ssize_t r = serial_read(s, buf, sizeof(buf), 100);
		if (r <= 0)
			continue;
		for (ssize_t i = 0; i < r; i++) {
			char c = (char)buf[i];
			if (c == '\n' || ll == sizeof(line) - 1) {
				line[ll] = '\0';
				(*lines_out)++;
				if (!json)
					printf("[%7.3f] %s\n", (csk_now_ms() - start) / 1000.0,
							line);
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

void csk_fprint_json(void *vfp, int flash_ok, int have_id,
		const uint8_t *chip_id, struct part *parts, struct part_result *results,
		int nparts, const char *until, int console_rc, const char *matched,
		int lines)
{
	FILE *fp = (FILE *)vfp;
	char idstr[17] = "";
	if (have_id)
		csk_hex16(idstr, chip_id, 8);

	fprintf(fp, "{\n");
	if (have_id)
		fprintf(fp, "  \"chip_id\": \"%s\",\n", idstr);
	else
		fprintf(fp, "  \"chip_id\": null,\n");
	fprintf(fp, "  \"flash\": \"%s\",\n", flash_ok ? "ok" : "failed");
	fprintf(fp, "  \"partitions\": [\n");
	for (int i = 0; i < nparts; i++) {
		fprintf(fp,
				"    {\"addr\": \"0x%08X\", \"bytes\": %ld, \"action\": \"%s\", "
				"\"ms\": %u}%s\n",
				parts[i].addr, results[i].bytes,
				results[i].action ? results[i].action : "not-reached",
				results[i].ms, i + 1 < nparts ? "," : "");
	}
	fprintf(fp, "  ],\n");

	fprintf(fp, "  \"console\": {\"lines\": %d, \"matched\": ", lines);
	if (until && console_rc == 0 && matched) {
		char mesc[256];
		json_escape(matched, mesc, sizeof(mesc));
		fprintf(fp, "\"%s\"", mesc);
	} else {
		fprintf(fp, "null");
	}
	fprintf(fp, "},\n");

	const char *result = !flash_ok ? "flash-failed"
			: !until               ? "ok"
			: console_rc == 0      ? "matched"
								   : "console-timeout";
	fprintf(fp, "  \"result\": \"%s\"\n", result);
	fprintf(fp, "}\n");
}
