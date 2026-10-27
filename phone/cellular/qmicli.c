/*
 * qmicli: standalone QMI/QRTR diagnostic & bring-up tool over /var/run/sweet-qrtr.sock.
 *
 * Usage:
 *   qmicli                      Query DMS + UIM + NAS and print redacted JSON
 *   qmicli --online             Set DMS operating mode ONLINE (0x00), then query
 *   qmicli --low-power          Set DMS operating mode LOW_POWER (0x01), then query
 *   qmicli --wds-test [apn]     Probe WDA + test WDS packet data session, print JSON
 */
#include "qmi_cellular.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
				"usage: qmicli [--sock PATH] [--online | --low-power] [--wds-test [APN] [--keep]]\n");
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
