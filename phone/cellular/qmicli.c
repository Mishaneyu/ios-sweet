/*
 * qmicli: standalone QMI/QRTR diagnostic & voice call CLI tool.
 *
 * Usage:
 *   qmicli                      Query DMS + UIM + NAS + VOICE and print redacted JSON
 *   qmicli --online             Set DMS operating mode ONLINE (0x00), then query
 *   qmicli --low-power          Set DMS operating mode LOW_POWER (0x01), then query
 *   qmicli --wds-test [apn]     Probe WDA + test WDS packet data session, print JSON
 *   qmicli dial [--emergency] <num>  Place MO voice call (NEVER logs or prints <num>)
 *   qmicli answer [call_id]     Answer incoming/waiting MT voice call
 *   qmicli hangup [call_id]     End active/ringing voice call (0 = all/current)
 *   qmicli calls                Query active QMI_SVC_VOICE calls (redacted JSON)
 */
#include "qmi_cellular.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void wipe_mem(void *p, size_t n)
{
	volatile uint8_t *vp = (volatile uint8_t *)p;
	while (n--)
		*vp++ = 0;
}

static bool is_emergency_str(const char *s)
{
	if (!s)
		return false;
	return !strcmp(s, "112") || !strcmp(s, "911") || !strcmp(s, "100") ||
	       !strcmp(s, "101") || !strcmp(s, "102") || !strcmp(s, "108") ||
	       !strcmp(s, "999") || !strcmp(s, "000");
}

static int try_cellulard_ctrl(const char *cmd, size_t cmd_len, char *reply, size_t reply_cap)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
#ifdef SO_NOSIGPIPE
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", SWEET_CELLULARD_CTRL_SOCK);
#ifdef __APPLE__
	sun.sun_len = (uint8_t)sizeof(sun);
#endif
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		close(fd);
		return -1;
	}
	size_t off = 0;
	while (off < cmd_len) {
		ssize_t nw = send(fd, cmd + off, cmd_len - off, 0);
		if (nw <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)nw;
	}
	size_t roff = 0;
	while (roff + 1 < reply_cap) {
		ssize_t nr = recv(fd, reply + roff, reply_cap - 1 - roff, 0);
		if (nr <= 0)
			break;
		roff += (size_t)nr;
	}
	reply[roff] = '\0';
	close(fd);
	return roff > 0 ? 0 : -1;
}

static int format_redacted_calls_json(const struct qmi_ctx *ctx, const struct cellular_state *st,
				      char *buf, size_t cap)
{
	char items[2048];
	size_t io = 0;
	items[0] = '\0';
	for (size_t i = 0; i < ctx->voice_call_count; i++) {
		const struct qmi_voice_call *c = &ctx->voice_calls[i];
		if (!c->present)
			continue;
		int w = snprintf(items + io, sizeof(items) - io,
				 "%s{\"call_id\": %u, \"state\": \"%s\", \"direction\": \"%s\", "
				 "\"mode\": %u, \"call_type\": %u, \"is_mpty\": %s, "
				 "\"number_present\": %s, \"number_len\": %zu}",
				 io ? ", " : "",
				 c->call_id,
				 qmi_voice_state_str(c->state),
				 c->direction == 1 ? "mo" : (c->direction == 2 ? "mt" : "unknown"),
				 c->mode,
				 c->call_type,
				 c->is_mpty ? "true" : "false",
				 c->number[0] ? "true" : "false",
				 strlen(c->number));
		if (w < 0 || (size_t)w >= sizeof(items) - io)
			break;
		io += (size_t)w;
	}
	int n = snprintf(buf, cap,
		"{\n"
		"  \"ok\": true,\n"
		"  \"voice_svc_ready\": %s,\n"
		"  \"active_calls\": %u,\n"
		"  \"last_call_state\": \"%s\",\n"
		"  \"last_call_end_reason\": %u,\n"
		"  \"calls\": [%s]\n"
		"}\n",
		st->voice_svc_ready ? "true" : "false",
		st->active_calls,
		st->last_call_state[0] ? st->last_call_state : "idle",
		st->last_call_end_reason,
		items);
	return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int handle_voice_subcommand(const char *sock_path, int argc, char **argv, int sub_idx)
{
	const char *sub = argv[sub_idx];
	char cmd[128];
	char reply[4096];
	wipe_mem(cmd, sizeof(cmd));

	if (!strcmp(sub, "dial")) {
		bool emerg = false;
		char *num_arg = NULL;
		for (int i = sub_idx + 1; i < argc; i++) {
			if (!strcmp(argv[i], "--emergency") || !strcmp(argv[i], "-e"))
				emerg = true;
			else if (!num_arg)
				num_arg = argv[i];
		}
		if (!num_arg || !num_arg[0]) {
			fprintf(stderr, "usage: qmicli dial [--emergency] <number>\n");
			return 1;
		}
		if (is_emergency_str(num_arg))
			emerg = true;
		char dial_buf[64];
		wipe_mem(dial_buf, sizeof(dial_buf));
		snprintf(dial_buf, sizeof(dial_buf), "%s", num_arg);
		/* Wipe argv copy in process memory immediately */
		wipe_mem(num_arg, strlen(num_arg));

		int clen = snprintf(cmd, sizeof(cmd), "DIAL %d %s\n", emerg ? 1 : 0, dial_buf);
		if (clen > 0 && (size_t)clen < sizeof(cmd) &&
		    try_cellulard_ctrl(cmd, (size_t)clen, reply, sizeof(reply)) == 0) {
			wipe_mem(cmd, sizeof(cmd));
			wipe_mem(dial_buf, sizeof(dial_buf));
			fputs(reply, stdout);
			return strstr(reply, "\"ok\": true") || strstr(reply, "\"ok\":true") ? 0 : 2;
		}
		wipe_mem(cmd, sizeof(cmd));

		/* Direct QMI fallback when cellulard control socket is unavailable */
		struct qmi_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.fd = -1;
		if (qmi_open(&ctx, sock_path) < 0) {
			wipe_mem(dial_buf, sizeof(dial_buf));
			printf("{\"ok\": false, \"action\": \"dial\", \"error\": \"qrtr_unavailable\"}\n");
			return 1;
		}
		(void)qmi_sync_servers(&ctx, 800);
		uint8_t cid = 0;
		int rc = qmi_voice_dial(&ctx, dial_buf, emerg, &cid);
		wipe_mem(dial_buf, sizeof(dial_buf));
		qmi_close(&ctx);
		printf("{\"ok\": %s, \"action\": \"dial\", \"call_id\": %u, \"rc\": %d}\n",
		       rc == 0 ? "true" : "false", cid, rc);
		return rc == 0 ? 0 : 2;
	}

	if (!strcmp(sub, "answer")) {
		uint8_t cid = (sub_idx + 1 < argc) ? (uint8_t)atoi(argv[sub_idx + 1]) : 0;
		int clen = snprintf(cmd, sizeof(cmd), "ANSWER %u\n", cid);
		if (clen > 0 && try_cellulard_ctrl(cmd, (size_t)clen, reply, sizeof(reply)) == 0) {
			fputs(reply, stdout);
			return strstr(reply, "\"ok\": true") || strstr(reply, "\"ok\":true") ? 0 : 2;
		}
		struct qmi_ctx ctx;
		struct cellular_state st;
		memset(&ctx, 0, sizeof(ctx));
		ctx.fd = -1;
		cellular_state_init(&st);
		if (qmi_open(&ctx, sock_path) < 0) {
			printf("{\"ok\": false, \"action\": \"answer\", \"error\": \"qrtr_unavailable\"}\n");
			return 1;
		}
		(void)qmi_sync_servers(&ctx, 800);
		(void)qmi_voice_query_calls(&ctx, &st);
		int rc = qmi_voice_answer(&ctx, cid);
		qmi_close(&ctx);
		printf("{\"ok\": %s, \"action\": \"answer\", \"call_id\": %u, \"rc\": %d}\n",
		       rc == 0 ? "true" : "false", cid, rc);
		return rc == 0 ? 0 : 2;
	}

	if (!strcmp(sub, "hangup") || !strcmp(sub, "end")) {
		uint8_t cid = (sub_idx + 1 < argc) ? (uint8_t)atoi(argv[sub_idx + 1]) : 0;
		int clen = snprintf(cmd, sizeof(cmd), "HANGUP %u\n", cid);
		if (clen > 0 && try_cellulard_ctrl(cmd, (size_t)clen, reply, sizeof(reply)) == 0) {
			fputs(reply, stdout);
			return strstr(reply, "\"ok\": true") || strstr(reply, "\"ok\":true") ? 0 : 2;
		}
		struct qmi_ctx ctx;
		struct cellular_state st;
		memset(&ctx, 0, sizeof(ctx));
		ctx.fd = -1;
		cellular_state_init(&st);
		if (qmi_open(&ctx, sock_path) < 0) {
			printf("{\"ok\": false, \"action\": \"hangup\", \"error\": \"qrtr_unavailable\"}\n");
			return 1;
		}
		(void)qmi_sync_servers(&ctx, 800);
		(void)qmi_voice_query_calls(&ctx, &st);
		int rc = qmi_voice_end(&ctx, cid);
		if (rc == 0)
			sweet_voice_write_shm(0, QMI_VOICE_CALL_STATE_END, 0);
		qmi_close(&ctx);
		printf("{\"ok\": %s, \"action\": \"hangup\", \"call_id\": %u, \"rc\": %d}\n",
		       rc == 0 ? "true" : "false", cid, rc);
		return rc == 0 ? 0 : 2;
	}

	if (!strcmp(sub, "calls")) {
		int clen = snprintf(cmd, sizeof(cmd), "CALLS\n");
		if (clen > 0 && try_cellulard_ctrl(cmd, (size_t)clen, reply, sizeof(reply)) == 0) {
			fputs(reply, stdout);
			return 0;
		}
		struct qmi_ctx ctx;
		struct cellular_state st;
		memset(&ctx, 0, sizeof(ctx));
		ctx.fd = -1;
		cellular_state_init(&st);
		if (qmi_open(&ctx, sock_path) < 0) {
			printf("{\"ok\": false, \"voice_svc_ready\": false, \"active_calls\": 0, \"calls\": []}\n");
			return 1;
		}
		(void)qmi_sync_servers(&ctx, 800);
		(void)qmi_voice_query_calls(&ctx, &st);
		if (format_redacted_calls_json(&ctx, &st, reply, sizeof(reply)) == 0)
			fputs(reply, stdout);
		qmi_close(&ctx);
		return 0;
	}

	return 1;
}

int main(int argc, char **argv)
{
	const char *sock_path = SWEET_QRTR_SOCK;
	const char *wds_apn = "";
	bool do_online = false;
	bool do_low_power = false;
	bool do_wds_test = false;
	bool keep_wds = false;

	signal(SIGPIPE, SIG_IGN);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sock") && i + 1 < argc) {
			sock_path = argv[++i];
		} else if (!strcmp(argv[i], "dial") || !strcmp(argv[i], "answer") ||
			   !strcmp(argv[i], "hangup") || !strcmp(argv[i], "end") ||
			   !strcmp(argv[i], "calls")) {
			return handle_voice_subcommand(sock_path, argc, argv, i);
		} else if (!strcmp(argv[i], "--online")) {
			do_online = true;
		} else if (!strcmp(argv[i], "--low-power") || !strcmp(argv[i], "--offline")) {
			do_low_power = true;
		} else if (!strcmp(argv[i], "--wds-test")) {
			do_wds_test = true;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				wds_apn = argv[++i];
		} else if (!strcmp(argv[i], "--keep")) {
			keep_wds = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fprintf(stderr,
				"usage: qmicli [--sock PATH] [--online | --low-power] [--wds-test [APN] [--keep]]\n"
				"       qmicli dial [--emergency] <number>\n"
				"       qmicli answer [call_id]\n"
				"       qmicli hangup [call_id]\n"
				"       qmicli calls\n");
			return 0;
		}
	}

	struct qmi_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.fd = -1;

	struct cellular_state st;
	cellular_state_init(&st);

	if (qmi_open(&ctx, sock_path) < 0) {
		char json[4096];
		if (cellular_state_format_json(&st, json, sizeof(json)) == 0)
			fputs(json, stdout);
		return 1;
	}

	(void)qmi_sync_servers(&ctx, 800);

	if ((do_online || do_wds_test) && qmi_has_service(&ctx, QMI_SVC_DMS)) {
		(void)qmi_dms_set_operating_mode(&ctx, 0x00);
		usleep(250000);
	} else if (do_low_power && qmi_has_service(&ctx, QMI_SVC_DMS)) {
		(void)qmi_dms_set_operating_mode(&ctx, 0x01);
		usleep(150000);
	}

	(void)qmi_poll_all(&ctx, &st, true);

	/* If --online was just requested and SIM is present, wait briefly for registration */
	if (do_online && st.sim_present && st.reg_state_raw != 1) {
		for (int attempt = 0; attempt < 8 && st.reg_state_raw != 1; attempt++) {
			usleep(300000);
			(void)qmi_poll_fast(&ctx, &st);
		}
	}

	int wds_rc = 0;
	if (do_wds_test)
		wds_rc = qmi_wds_test_session(&ctx, &st, wds_apn, keep_wds);

	char json[4096];
	if (cellular_state_format_json(&st, json, sizeof(json)) == 0)
		fputs(json, stdout);

	qmi_close(&ctx);
	return (do_wds_test && wds_rc != 0) ? 2 : 0;
}
