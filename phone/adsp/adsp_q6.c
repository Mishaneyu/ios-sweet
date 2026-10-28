/*
 * SPDX-License-Identifier: MIT
 *
 * adsp_q6.c: APR v2 protocol engine for SM8150 LPASS ADSP:
 *   - Q6Core (AVCS readiness check)
 *   - Q6AFE  (QUAD_MI2S_IBIT 1.536 MHz + QUATERNARY_MI2S_RX 0x1006 SD1 stereo +
 *             QUATERNARY_MI2S_TX 0x1007 SD0 / PSEUDOPORT_01 0x8001 for voice)
 *   - Q6ADM  (COPP ADM_TOPOLOGY_NONE 0x10312 + matrix routing)
 *   - Q6ASM  (shared memory map of 0xE1000000 via SID 1<<32, 8 x 20 ms S16_LE
 *             periods fed from /tmp/sweet-audio.shm or built-in tone generator)
 *   - Q6Voice (APR_SVC_ADSP_MVM 0x09 + CVS 0x0A + CVP 0x0B "default modem voice"
 *              cellular voice call routing)
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adspd.h"

/* ---- APR v2 framing & service IDs (techpack/audio/include/ipc/apr.h) ---- */
#define APR_HDR_SIZE                            20u
#define APR_HDR_FIELD_CMD                       0x0050u /* SEQ_CMD(0), len=5 dwords, v0 */
#define APR_DOMAIN_ADSP                         4u
#define APR_DOMAIN_APPS                         5u

#define APR_SVC_ADSP_CORE                       0x03u
#define APR_SVC_AFE                             0x04u
#define APR_SVC_ASM                             0x07u
#define APR_SVC_ADM                             0x08u
#define APR_SVC_ADSP_MVM                        0x09u
#define APR_SVC_ADSP_CVS                        0x0au
#define APR_SVC_ADSP_CVP                        0x0bu

#define APR_BASIC_RSP_RESULT                    0x000110e8u
#define APR_CMD_DESTROY_SESSION                 0x0001003cu

/* ---- AVCS / Q6Core ---- */
#define AVCS_CMD_ADSP_EVENT_GET_STATE           0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE        0x0001290du

/* ---- Q6AFE ---- */
#define AFE_PORT_ID_QUATERNARY_MI2S_RX          0x1006u
#define AFE_PORT_ID_QUATERNARY_MI2S_TX          0x1007u
#define AFE_PORT_ID_PSEUDOPORT_01               0x8001u

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u

#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG                 0x0001020du
#define AFE_PARAM_ID_LPAIF_CLK_CONFIG           0x00010238u
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u

#define Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT       0x0106u
#define Q6AFE_LPASS_CLK_ATTR_COUPLE_NO          0x0001u
#define AFE_PORT_I2S_SD0                        0x0001u
#define AFE_PORT_I2S_SD1                        0x0002u

/* ---- Q6ADM ---- */
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_TOPOLOGY_NONE                       0x00010312u

/* ---- Q6ASM ---- */
#define ASM_SESSION_ID                          1u
#define ASM_STREAM_ID                           1u
#define ASM_PORT_ID                             ((ASM_SESSION_ID << 8) | ASM_STREAM_ID)

#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_STREAM_CMD_CLOSE                    0x00010dbcu
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_SESSION_CMD_PAUSE                   0x00010bd3u

#define ASM_DEFAULT_POPP_TOPOLOGY               0x00010be4u
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u

/* ---- Q6Voice (MVM / CVS / CVP, techpack/audio/include/dsp/q6voice.h) ---- */
#define VSS_IMVM_CMD_CREATE_PASSIVE_CTL_SESSION 0x000110ffu
#define VSS_IMVM_CMD_START_VOICE                0x00011190u
#define VSS_IMVM_CMD_STOP_VOICE                 0x00011192u
#define VSS_IMVM_CMD_ATTACH_VOCPROC             0x0001123eu
#define VSS_IMVM_CMD_DETACH_VOCPROC             0x0001123fu
#define VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL    0x00011327u

#define VSS_ISTREAM_CMD_CREATE_PASSIVE_CTL_SESS 0x00011140u
#define VSS_IVOLUME_CMD_MUTE_V2                 0x0001138bu

#define VSS_IVOCPROC_CMD_ENABLE                 0x000100c6u
#define VSS_IVOCPROC_CMD_DISABLE                0x000110e1u
#define VSS_IVOCPROC_CMD_CREATE_FULL_CTL_V2     0x000112bfu
#define VSS_IVOCPROC_CMD_CREATE_FULL_CTL_V3     0x000112c6u
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT        0x000112f4u
#define VSS_ICOMMON_CMD_SET_PARAM_V2            0x000112a6u
#define VSS_ICOMMON_CMD_SET_PARAM_V3            0x000113adu

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE           0x00010f70u
#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING 0x00010f7cu
#define VSS_MODULE_CVD_GENERIC                  0x0001137au
#define VSS_PARAM_VOCPROC_TX_CHANNEL_INFO       0x0001137bu
#define VSS_PARAM_VOCPROC_RX_CHANNEL_INFO       0x0001137cu
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO   0x000113acu
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO   0x000113abu

#define WRITE_TOKEN_FLAG                        0x80000000u
#define AMP_SILENCE_SHUTOFF_MS                  3000ULL

struct apr_hdr {
	uint16_t hdr_field;
	uint16_t pkt_size;
	uint8_t  src_svc;
	uint8_t  src_domain;
	uint16_t src_port;
	uint8_t  dest_svc;
	uint8_t  dest_domain;
	uint16_t dest_port;
	uint32_t token;
	uint32_t opcode;
} __attribute__((packed));

/* Synchronous APR command waiter state */
static struct {
	bool     waiting;
	uint32_t want_token;
	bool     done;
	uint32_t rsp_opcode;
	uint32_t basic_status;
	uint16_t src_port;
	uint32_t words[8];
} sync_wait;

static uint32_t next_cmd_token = 1u;
static uint8_t  period_ready[ADSP_DMA_PERIODS];
static uint64_t last_nonsilent_ms;
static uint64_t last_voice_poll_ms;
static bool     afe_tx_running;
static uint16_t voice_tx_port = AFE_PORT_ID_QUATERNARY_MI2S_TX;

/* Built-in test tone state */
static uint64_t tone_frames_left;
static uint64_t tone_frames_total;
static uint64_t tone_phase_idx;

static int
apr_send_raw(uint8_t svc, uint16_t src_port, uint16_t dst_port, uint32_t token,
    uint32_t opcode, const void *payload, uint16_t payload_size)
{
	uint8_t pkt[APR_HDR_SIZE + 512];
	struct apr_hdr h;

	if (payload_size > sizeof(pkt) - APR_HDR_SIZE) {
		return -1;
	}
	h.hdr_field = APR_HDR_FIELD_CMD;
	h.pkt_size = (uint16_t)(APR_HDR_SIZE + payload_size);
	h.src_svc = svc;
	h.src_domain = APR_DOMAIN_APPS;
	h.src_port = src_port;
	h.dest_svc = svc;
	h.dest_domain = APR_DOMAIN_ADSP;
	h.dest_port = dst_port;
	h.token = token;
	h.opcode = opcode;

	memcpy(pkt, &h, APR_HDR_SIZE);
	if (payload_size > 0 && payload != NULL) {
		memcpy(pkt + APR_HDR_SIZE, payload, payload_size);
	}
	return adsp_apr_send(pkt, APR_HDR_SIZE + payload_size);
}

static int
apr_call_ex(uint8_t svc, uint16_t src_port, uint16_t dst_port, uint32_t opcode,
    const void *payload, uint16_t payload_size, uint32_t expect_rsp_op,
    uint16_t *out_from_port, uint32_t out_words[4], unsigned timeout_ms)
{
	uint32_t tok = next_cmd_token++;
	uint64_t deadline;

	if (next_cmd_token >= 0x70000000u) {
		next_cmd_token = 1u;
	}
	memset(&sync_wait, 0, sizeof(sync_wait));
	sync_wait.waiting = true;
	sync_wait.want_token = tok;

	if (apr_send_raw(svc, src_port, dst_port, tok, opcode, payload, payload_size) != 0) {
		sync_wait.waiting = false;
		return -1;
	}

	deadline = now_ms() + timeout_ms;
	while (!sync_wait.done && now_ms() < deadline && !g_stop_requested) {
		(void)adsp_glink_poll(5);
	}
	sync_wait.waiting = false;

	if (!sync_wait.done) {
		adsp_log("apr: timeout svc=%u op=0x%x port=0x%x->0x%x",
		    svc, opcode, src_port, dst_port);
		return -1;
	}
	if (sync_wait.rsp_opcode == APR_BASIC_RSP_RESULT) {
		if (sync_wait.basic_status != 0) {
			adsp_log("apr: svc=%u op=0x%x failed status=0x%x",
			    svc, opcode, sync_wait.basic_status);
			return -(int)sync_wait.basic_status;
		}
		if (expect_rsp_op != 0 && expect_rsp_op != APR_BASIC_RSP_RESULT) {
			/* Basic ACK arrived with status 0 */
		}
	} else if (expect_rsp_op != 0 && sync_wait.rsp_opcode != expect_rsp_op) {
		adsp_log("apr: unexpected rsp 0x%x for op 0x%x", sync_wait.rsp_opcode, opcode);
		return -1;
	}

	if (out_from_port != NULL) {
		*out_from_port = sync_wait.src_port;
	}
	if (out_words != NULL) {
		memcpy(out_words, sync_wait.words, 4 * sizeof(uint32_t));
	}
	return 0;
}

static int
apr_call(uint8_t svc, uint16_t port, uint32_t opcode, const void *payload,
    uint16_t payload_size, uint32_t expect_rsp_op, uint32_t out_words[4])
{
	return apr_call_ex(svc, port, port, opcode, payload, payload_size,
	    expect_rsp_op, NULL, out_words, 2000);
}

void
adsp_apr_rx(const uint8_t *data, uint32_t len)
{
	struct apr_hdr h;
	uint32_t hdr_bytes, body_bytes;
	const uint8_t *body;

	if (len < APR_HDR_SIZE) {
		return;
	}
	memcpy(&h, data, APR_HDR_SIZE);
	hdr_bytes = ((h.hdr_field >> 4) & 0x0fu) * 4u;
	if (hdr_bytes < APR_HDR_SIZE || hdr_bytes > len) {
		return;
	}
	body = data + hdr_bytes;
	body_bytes = len - hdr_bytes;

	/* Async DMA write completion from Q6ASM */
	if (h.opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (h.token & WRITE_TOKEN_FLAG)) {
		uint32_t idx = h.token & ~WRITE_TOKEN_FLAG;
		if (idx < ADSP_DMA_PERIODS) {
			period_ready[idx] = 1;
		}
		return;
	}

	if (sync_wait.waiting && h.token == sync_wait.want_token) {
		sync_wait.rsp_opcode = h.opcode;
		sync_wait.src_port = h.src_port;
		memset(sync_wait.words, 0, sizeof(sync_wait.words));
		if (body_bytes > 0) {
			size_t cp = body_bytes < sizeof(sync_wait.words) ?
			    body_bytes : sizeof(sync_wait.words);
			memcpy(sync_wait.words, body, cp);
		}
		if (h.opcode == APR_BASIC_RSP_RESULT && body_bytes >= 8) {
			sync_wait.basic_status = sync_wait.words[1];
		}
		sync_wait.done = true;
	}
}

/* ============================================================================
 * 1. Q6Core Readiness + Q6AFE Quaternary MI2S Setup
 * ============================================================================ */

static int
q6core_wait_ready(unsigned timeout_ms)
{
	uint64_t end = now_ms() + timeout_ms;
	uint32_t words[4] = { 0 };

	while (now_ms() < end && !g_stop_requested) {
		if (apr_call_ex(APR_SVC_ADSP_CORE, 0, 0, AVCS_CMD_ADSP_EVENT_GET_STATE,
		    NULL, 0, AVCS_CMDRSP_ADSP_EVENT_GET_STATE, NULL, words, 400) == 0 &&
		    words[0] == 1u) {
			g_status.q6_core_ready = true;
			adsp_log("q6core: AVCS audio_pd ready (state=1)");
			return 0;
		}
		(void)adsp_glink_poll(150);
	}
	adsp_log("q6core: timeout waiting for AVCS_CMDRSP_ADSP_EVENT_GET_STATE==1");
	return -1;
}

static int
q6afe_set_quad_mi2s_clk(uint32_t freq_hz)
{
	struct {
		uint32_t payload_size;
		uint32_t payload_addr_lsw;
		uint32_t payload_addr_msw;
		uint32_t mem_map_handle;
		uint32_t module_id;
		uint32_t param_id;
		uint16_t param_size;
		uint16_t reserved;
		uint32_t clk_set_minor_version;
		uint32_t clk_id;
		uint32_t clk_freq_in_hz;
		uint16_t clk_attri;
		uint16_t clk_root;
		uint32_t enable;
	} __attribute__((packed)) pkt = {
		.payload_size          = 12u + 20u,
		.module_id             = AFE_MODULE_CLOCK_SET,
		.param_id              = AFE_PARAM_ID_CLOCK_SET,
		.param_size            = 20u,
		.clk_set_minor_version = 1u,
		.clk_id                = Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT,
		.clk_freq_in_hz        = freq_hz,
		.clk_attri             = Q6AFE_LPASS_CLK_ATTR_COUPLE_NO,
		.clk_root              = 0u,
		.enable                = freq_hz > 0 ? 1u : 0u,
	};

	if (apr_call(APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, &pkt, sizeof(pkt), 0, NULL) == 0) {
		return 0;
	}

	/* Legacy LPAIF_CLK_CONFIG fallback */
	struct {
		uint16_t port_id;
		uint16_t payload_size;
		uint32_t payload_addr_lsw;
		uint32_t payload_addr_msw;
		uint32_t mem_map_handle;
		uint32_t module_id;
		uint32_t param_id;
		uint16_t param_size;
		uint16_t reserved0;
		uint32_t minor_version;
		uint32_t clk_val1;
		uint32_t clk_val2;
		uint16_t clk_src;
		uint16_t clk_root;
		uint16_t clk_set_mode;
		uint16_t reserved1;
	} __attribute__((packed)) legacy = {
		.port_id       = AFE_PORT_ID_QUATERNARY_MI2S_RX,
		.payload_size  = 12u + 20u,
		.module_id     = AFE_MODULE_AUDIO_DEV_INTERFACE,
		.param_id      = AFE_PARAM_ID_LPAIF_CLK_CONFIG,
		.param_size    = 20u,
		.minor_version = 1u,
		.clk_val1      = freq_hz,
		.clk_src       = 1u,
		.clk_set_mode  = 1u,
	};
	return apr_call(APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, &legacy, sizeof(legacy), 0, NULL);
}

static int
q6afe_start_i2s_port(uint16_t port_id, uint16_t sd_line_mode, uint16_t mono_stereo)
{
	struct {
		uint16_t port_id;
		uint16_t payload_size;
		uint32_t payload_addr_lsw;
		uint32_t payload_addr_msw;
		uint32_t mem_map_handle;
		uint32_t module_id;
		uint32_t param_id;
		uint16_t param_size;
		uint16_t pad;
		uint32_t i2s_cfg_minor_version;
		uint16_t bit_width;
		uint16_t channel_mode;
		uint16_t mono_stereo;
		uint16_t ws_src;
		uint32_t sample_rate;
		uint16_t data_format;
		uint16_t reserved;
	} __attribute__((packed)) cfg = {
		.port_id               = port_id,
		.payload_size          = 12u + 20u,
		.module_id             = AFE_MODULE_AUDIO_DEV_INTERFACE,
		.param_id              = AFE_PARAM_ID_I2S_CONFIG,
		.param_size            = 20u,
		.i2s_cfg_minor_version = 1u,
		.bit_width             = 16u,
		.channel_mode          = sd_line_mode,
		.mono_stereo           = mono_stereo,
		.ws_src                = 1u, /* internal master WS */
		.sample_rate           = HAKIOS_AUDIO_RATE,
		.data_format           = 0u, /* linear PCM */
	};
	struct {
		uint16_t port_id;
		uint16_t reserved;
	} __attribute__((packed)) start = { .port_id = port_id, .reserved = 0 };

	/* Stop port first in case of warm state, ignore error */
	(void)apr_call(APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, &start, sizeof(start), 0, NULL);

	if (apr_call(APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, &cfg, sizeof(cfg), 0, NULL) != 0) {
		return -1;
	}
	if (apr_call(APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, &start, sizeof(start), 0, NULL) != 0) {
		return -1;
	}
	adsp_log("q6afe: port 0x%04x started (sd_mode=%u stereo=%u 48kHz 16-bit)",
	    port_id, sd_line_mode, mono_stereo);
	return 0;
}

static void
q6afe_stop_port(uint16_t port_id)
{
	struct {
		uint16_t port_id;
		uint16_t reserved;
	} __attribute__((packed)) stop = { .port_id = port_id, .reserved = 0 };

	(void)apr_call(APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, &stop, sizeof(stop), 0, NULL);
}

/* ============================================================================
 * 2. Q6ADM + Q6ASM Stereo 48 kHz PCM Stream & Ring Feede
 * ============================================================================ */

static int
q6asm_map_dma_pool(void)
{
	struct {
		uint16_t mem_pool_id;
		uint16_t num_regions;
		uint32_t property_flag;
		uint32_t shm_addr_lsw;
		uint32_t shm_addr_msw;
		uint32_t mem_size_bytes;
	} __attribute__((packed)) map_cmd = {
		.mem_pool_id    = 3u, /* ADSP_MEMORY_MAP_SHMEM8_4K_POOL */
		.num_regions    = 1u,
		.property_flag  = 0u, /* physical / S1-bypass IOVA */
		.shm_addr_lsw   = (uint32_t)(ADSP_DMA_PA & 0xffffffffu),
		.shm_addr_msw   = ADSP_DSP_IOVA_HIGH,
		.mem_size_bytes = ADSP_DMA_TOTAL_BYTES,
	};
	uint32_t words[4] = { 0 };

	if (g_status.asm_map_handle != 0) {
		return 0;
	}
	if (apr_call(APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map_cmd, sizeof(map_cmd),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, words) != 0 || words[0] == 0) {
		adsp_log("q6asm: SHARED_MEM_MAP_REGIONS failed");
		return -1;
	}
	g_status.asm_map_handle = words[0];
	adsp_log("q6asm: mapped DMA 0x%x:%08x (%u B) -> handle=0x%x",
	    ADSP_DSP_IOVA_HIGH, (uint32_t)ADSP_DMA_PA,
	    ADSP_DMA_TOTAL_BYTES, g_status.asm_map_handle);
	return 0;
}

static int
q6_send_period_write(uint32_t idx)
{
	uint64_t pa = ADSP_DMA_PA + (uint64_t)idx * ADSP_DMA_PERIOD_ALLOC;
	struct {
		uint32_t buf_addr_lsw;
		uint32_t buf_addr_msw;
		uint32_t mem_map_handle;
		uint32_t avail_bytes;
		uint32_t uid;
		uint32_t timestamp_lsw;
		uint32_t timestamp_msw;
		uint32_t flags;
	} __attribute__((packed)) wr = {
		.buf_addr_lsw   = (uint32_t)(pa & 0xffffffffu),
		.buf_addr_msw   = ADSP_DSP_IOVA_HIGH,
		.mem_map_handle = g_status.asm_map_handle,
		.avail_bytes    = ADSP_PCM_PERIOD_BYTES,
		.uid            = idx,
		.timestamp_lsw  = 0,
		.timestamp_msw  = 0,
		.flags          = 0,
	};

	period_ready[idx] = 0;
	if (apr_send_raw(APR_SVC_ASM, ASM_PORT_ID, ASM_PORT_ID,
	    WRITE_TOKEN_FLAG | idx, ASM_DATA_CMD_WRITE_V2, &wr, sizeof(wr)) != 0) {
		period_ready[idx] = 1;
		return -1;
	}
	g_status.periods_written++;
	return 0;
}

static bool
fill_period_pcm(int16_t *dst_stereo, uint32_t frames)
{
	bool has_signal = false;

	if (tone_frames_left > 0) {
		/* Built-in stereo C5 (523.25 Hz) + E5 (659.25 Hz) test chime */
		for (uint32_t i = 0; i < frames; i++) {
			double env = 1.0;
			if (tone_frames_left > 0) {
				uint64_t done = tone_frames_total - tone_frames_left;
				if (done < 960) {
					env = (double)done / 960.0;
				} else if (tone_frames_left < 2400) {
					env = (double)tone_frames_left / 2400.0;
				}
				double t = (double)(tone_phase_idx++) / (double)HAKIOS_AUDIO_RATE;
				int16_t l = (int16_t)lrint(sin(2.0 * M_PI * 523.25 * t) * 11000.0 * env);
				int16_t r = (int16_t)lrint(sin(2.0 * M_PI * 659.25 * t) * 11000.0 * env);
				dst_stereo[i * 2] = l;
				dst_stereo[i * 2 + 1] = r;
				tone_frames_left--;
				has_signal = true;
			} else {
				dst_stereo[i * 2] = 0;
				dst_stereo[i * 2 + 1] = 0;
			}
		}
		g_status.frames_played += frames;
		return has_signal;
	}

	if (g_shm == NULL) {
		memset(dst_stereo, 0, (size_t)frames * HAKIOS_AUDIO_FRAME);
		return false;
	}

	uint32_t w = __atomic_load_n(&g_shm->write_frames, __ATOMIC_ACQUIRE);
	uint32_t r = __atomic_load_n(&g_shm->read_frames, __ATOMIC_RELAXED);
	uint32_t vol = __atomic_load_n(&g_shm->volume_q16, __ATOMIC_ACQUIRE);
	uint32_t avail = w - r;
	uint32_t mask = SWEET_AUDIO_RING_FRAMES - 1u;

	if (avail > SWEET_AUDIO_RING_FRAMES) {
		r = w;
		__atomic_store_n(&g_shm->read_frames, r, __ATOMIC_RELEASE);
		avail = 0;
	}
	uint32_t take = avail < frames ? avail : frames;
	if (take < frames && avail > 0) {
		g_status.underruns++;
	}

	for (uint32_t i = 0; i < take; i++) {
		uint32_t idx = ((r + i) & mask) * 2u;
		int32_t sl = ((int32_t)g_shm->pcm[idx] * (int32_t)vol) >> 16;
		int32_t sr = ((int32_t)g_shm->pcm[idx + 1] * (int32_t)vol) >> 16;

		if (sl > 32767) sl = 32767;
		if (sl < -32768) sl = -32768;
		if (sr > 32767) sr = 32767;
		if (sr < -32768) sr = -32768;

		dst_stereo[i * 2] = (int16_t)sl;
		dst_stereo[i * 2 + 1] = (int16_t)sr;
		if (sl > 12 || sl < -12 || sr > 12 || sr < -12) {
			has_signal = true;
		}
	}
	if (take < frames) {
		memset(&dst_stereo[take * 2], 0, (size_t)(frames - take) * HAKIOS_AUDIO_FRAME);
	}
	if (take > 0) {
		__atomic_store_n(&g_shm->read_frames, r + take, __ATOMIC_RELEASE);
		g_status.frames_played += take;
	}
	return has_signal;
}

static int
q6_open_media_stream(void)
{
	struct {
		uint16_t flags;
		uint16_t mode_of_operation;
		uint16_t endpoint_id_1;
		uint16_t endpoint_id_2;
		uint32_t topology_id;
		uint16_t dev_num_channel;
		uint16_t bit_width;
		uint32_t sample_rate;
		uint8_t  dev_channel_mapping[8];
	} __attribute__((packed)) adm_open = {
		.flags               = 0x0200u, /* legacy stream bit */
		.mode_of_operation   = 1u,      /* playback */
		.endpoint_id_1       = AFE_PORT_ID_QUATERNARY_MI2S_RX,
		.endpoint_id_2       = 0xffffu,
		.topology_id         = ADM_TOPOLOGY_NONE,
		.dev_num_channel     = HAKIOS_AUDIO_CHANNELS,
		.bit_width           = 16u,
		.sample_rate         = HAKIOS_AUDIO_RATE,
		.dev_channel_mapping = { 1, 2, 0, 0, 0, 0, 0, 0 },
	};
	struct {
		uint32_t mode_flags;
		uint16_t sink_endpoint_type;
		uint16_t bits_per_sample;
		uint32_t postproc_topo_id;
		uint32_t dec_fmt_id;
	} __attribute__((packed)) asm_open = {
		.mode_flags         = 0u,
		.sink_endpoint_type = 0u,
		.bits_per_sample    = 16u,
		.postproc_topo_id   = ASM_TOPOLOGY_NONE,
		.dec_fmt_id         = ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2,
	};
	struct {
		uint32_t matrix_id;
		uint32_t num_sessions;
		uint16_t session_id;
		uint16_t num_copps;
		uint16_t copp_id;
		uint16_t reserved;
	} __attribute__((packed)) route = {
		.matrix_id    = 0u, /* AUDIO_RX */
		.num_sessions = 1u,
		.session_id   = ASM_SESSION_ID,
		.num_copps    = 1u,
	};
	struct {
		uint32_t fmt_blk_size;
		uint16_t num_channels;
		uint16_t bits_per_sample;
		uint32_t sample_rate;
		uint16_t is_signed;
		uint16_t reserved;
		uint8_t  channel_mapping[8];
	} __attribute__((packed)) fmt = {
		.fmt_blk_size    = 20u,
		.num_channels    = HAKIOS_AUDIO_CHANNELS,
		.bits_per_sample = 16u,
		.sample_rate     = HAKIOS_AUDIO_RATE,
		.is_signed       = 1u,
		.channel_mapping = { 1, 2, 0, 0, 0, 0, 0, 0 },
	};
	struct {
		uint32_t flags;
		uint32_t time_lsw;
		uint32_t time_msw;
	} __attribute__((packed)) run_cmd = { 0, 0, 0 };
	uint32_t words[4] = { 0 };

	if (g_status.asm_stream_up) {
		return 0;
	}
	if (q6asm_map_dma_pool() != 0) {
		return -1;
	}

	if (apr_call(APR_SVC_ADM, AFE_PORT_ID_QUATERNARY_MI2S_RX, ADM_CMD_DEVICE_OPEN_V5,
	    &adm_open, sizeof(adm_open), ADM_CMDRSP_DEVICE_OPEN_V5, words) != 0) {
		adsp_log("q6adm: ADM_CMD_DEVICE_OPEN_V5 failed");
		return -1;
	}
	if (words[0] != 0) {
		adsp_log("q6adm: ADM_CMDRSP_DEVICE_OPEN_V5 status=0x%x", words[0]);
		return -1;
	}
	g_status.adm_copp_id = (int)(words[1] & 0xffffu);
	g_status.adm_copp_up = true;

	if (apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_STREAM_CMD_OPEN_WRITE_V3,
	    &asm_open, sizeof(asm_open), 0, NULL) != 0) {
		asm_open.postproc_topo_id = ASM_DEFAULT_POPP_TOPOLOGY;
		if (apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_STREAM_CMD_OPEN_WRITE_V3,
		    &asm_open, sizeof(asm_open), 0, NULL) != 0) {
			adsp_log("q6asm: ASM_STREAM_CMD_OPEN_WRITE_V3 failed");
			return -1;
		}
	}

	route.copp_id = (uint16_t)g_status.adm_copp_id;
	if (apr_call(APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5,
	    &route, sizeof(route), 0, NULL) != 0) {
		adsp_log("q6adm: ADM_CMD_MATRIX_MAP_ROUTINGS_V5 failed");
		return -1;
	}

	if (apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2,
	    &fmt, sizeof(fmt), 0, NULL) != 0) {
		adsp_log("q6asm: MEDIA_FMT_UPDATE_V2 failed");
		return -1;
	}

	if (apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_SESSION_CMD_RUN_V2,
	    &run_cmd, sizeof(run_cmd), 0, NULL) != 0) {
		adsp_log("q6asm: ASM_SESSION_CMD_RUN_V2 failed");
		return -1;
	}

	/* Prime all 8 x 20 ms DMA buffers */
	static const int16_t silence[ADSP_PCM_PERIOD_FRAMES * HAKIOS_AUDIO_CHANNELS];
	for (uint32_t p = 0; p < ADSP_DMA_PERIODS; p++) {
		uint64_t pa = ADSP_DMA_PA + (uint64_t)p * ADSP_DMA_PERIOD_ALLOC;
		(void)pm_wr(pa, silence, ADSP_PCM_PERIOD_BYTES);
		(void)q6_send_period_write(p);
	}

	g_status.asm_stream_up = true;
	snprintf(g_status.state, sizeof(g_status.state), "streaming");
	adsp_log("q6asm: stereo 48kHz stream running (copp=%d, 8x20ms DMA primed)",
	    g_status.adm_copp_id);
	adsp_status_write();
	return 0;
}

static void
q6_close_media_stream(void)
{
	if (g_status.asm_stream_up) {
		(void)apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_SESSION_CMD_PAUSE, NULL, 0, 0, NULL);
		(void)apr_call(APR_SVC_ASM, ASM_PORT_ID, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
		g_status.asm_stream_up = false;
	}
	if (g_status.adm_copp_up) {
		(void)apr_call(APR_SVC_ADM, (uint16_t)g_status.adm_copp_id,
		    ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
		g_status.adm_copp_up = false;
	}
}

/* ============================================================================
 * 3. Cellular Voice Call Audio (MVM 0x09 + CVS 0x0A + CVP 0x0B)
 * ============================================================================ */

static bool
detect_voice_call_wanted(bool *out_speakerphone)
{
	bool spk = false;
	bool active = false;

	if (g_shm != NULL) {
		if (__atomic_load_n(&g_shm->baseband_voice_io, __ATOMIC_ACQUIRE) != 0) {
			active = true;
		}
		if (__atomic_load_n(&g_shm->speaker_running, __ATOMIC_ACQUIRE) != 0) {
			spk = true;
		}
	}

	int fd = open(VOICE_CALL_STATE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		char buf[256];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n >= 16) {
			uint32_t magic = 0, v_active = 0;
			memcpy(&magic, buf, 4);
			memcpy(&v_active, buf + 4, 4);
			if (magic == 0x53575643u /* 'SWVC' from cellulard */) {
				if (v_active != 0) {
					active = true;
				}
				n = 0;
			}
		}
		if (n > 0) {
			buf[n] = '\0';
			if (strstr(buf, "\"speaker\":true") != NULL ||
			    strstr(buf, "\"speaker\":1") != NULL ||
			    strstr(buf, "speakerphone") != NULL) {
				spk = true;
			}
			if (strstr(buf, "\"active\":true") != NULL ||
			    strstr(buf, "\"active\":1") != NULL ||
			    strstr(buf, "\"voice_active\":true") != NULL ||
			    buf[0] == '1' || strncmp(buf, "active", 6) == 0 ||
			    strncmp(buf, "connected", 9) == 0) {
				active = true;
			} else if (buf[0] == '0' || strncmp(buf, "idle", 4) == 0 ||
			    strstr(buf, "\"active\":false") != NULL) {
				/* Explicit inactive file does not override HAL Baseband Voice IO */
			}
		}
	}

	*out_speakerphone = spk;
	return active;
}

static void
cvp_send_v2_v3_media_info(uint16_t cvp_h, uint16_t tx_port, uint16_t rx_port)
{
	/* CVP v2 channel info (RX & TX) + v3 endpoint media format + topology commit */
	struct {
		uint32_t mem_handle;
		uint32_t mem_addr_lsw;
		uint32_t mem_addr_msw;
		uint32_t mem_size;
		uint32_t module_id;
		uint32_t param_id;
		uint16_t param_size;
		uint16_t reserved;
		uint16_t num_channels;
		uint16_t bits_per_sample;
		uint8_t  ch_map[8];
	} __attribute__((packed)) ch_info = {
		.mem_size        = 12u + 12u,
		.module_id       = VSS_MODULE_CVD_GENERIC,
		.param_id        = VSS_PARAM_VOCPROC_RX_CHANNEL_INFO,
		.param_size      = 12u,
		.num_channels    = 1u,
		.bits_per_sample = 16u,
		.ch_map          = { 1, 0, 0, 0, 0, 0, 0, 0 },
	};
	(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_ICOMMON_CMD_SET_PARAM_V2,
	    &ch_info, sizeof(ch_info), 0, NULL, NULL, 500);
	ch_info.param_id = VSS_PARAM_VOCPROC_TX_CHANNEL_INFO;
	(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_ICOMMON_CMD_SET_PARAM_V2,
	    &ch_info, sizeof(ch_info), 0, NULL, NULL, 500);

	struct {
		uint32_t mem_handle;
		uint32_t mem_addr_lsw;
		uint32_t mem_addr_msw;
		uint32_t mem_size;
		uint32_t module_id;
		uint16_t instance_id;
		uint16_t reserved0;
		uint32_t param_id;
		uint32_t param_size;
		uint16_t port_id;
		uint16_t num_channels;
		uint16_t bits_per_sample;
		uint16_t reserved1;
		uint32_t sample_rate;
		uint8_t  ch_map[8];
	} __attribute__((packed)) ep_fmt = {
		.mem_size        = 16u + 20u,
		.module_id       = VSS_MODULE_CVD_GENERIC,
		.instance_id     = 0u,
		.param_id        = VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO,
		.param_size      = 20u,
		.port_id         = rx_port,
		.num_channels    = 2u,
		.bits_per_sample = 16u,
		.sample_rate     = HAKIOS_AUDIO_RATE,
		.ch_map          = { 1, 2, 0, 0, 0, 0, 0, 0 },
	};
	(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_ICOMMON_CMD_SET_PARAM_V3,
	    &ep_fmt, sizeof(ep_fmt), 0, NULL, NULL, 500);
	ep_fmt.param_id = VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO;
	ep_fmt.port_id = tx_port;
	ep_fmt.num_channels = 1u;
	ep_fmt.ch_map[1] = 0;
	(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_ICOMMON_CMD_SET_PARAM_V3,
	    &ep_fmt, sizeof(ep_fmt), 0, NULL, NULL, 500);

	(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT,
	    NULL, 0, 0, NULL, NULL, 500);
}

static int
q6_voice_start(bool speakerphone)
{
	char session_name[20] = "default modem voice";
	struct {
		uint32_t enable;
	} __attribute__((packed)) dual = { .enable = 1u };
	struct {
		uint16_t direction;
		uint16_t tx_port_id;
		uint32_t tx_topology_id;
		uint16_t rx_port_id;
		uint32_t rx_topology_id;
		uint32_t profile_id;
		uint32_t vocproc_mode;
		uint16_t ec_ref_port_id;
		char     name[20];
	} __attribute__((packed)) cvp_create;
	struct {
		uint16_t handle;
		uint16_t pad;
	} __attribute__((packed)) attach;
	struct {
		uint16_t ramp_ms;
		uint16_t direction;
		uint16_t mute_flag;
		uint16_t pad;
	} __attribute__((packed)) unmute = { .ramp_ms = 20u, .direction = 1u, .mute_flag = 0u };
	uint16_t mvm_h = 0, cvs_h = 0, cvp_h = 0;

	adsp_log("q6voice: starting cellular voice session (speakerphone=%d)", speakerphone ? 1 : 0);

	/* Pause/close ASM+ADM media route while keeping QUAT_MI2S_RX 0x1006 running */
	q6_close_media_stream();

	/* Start clocked QUAT_MI2S_TX (0x1007 SD0 mono) for CVP TX; fallback to PSEUDOPORT_01 */
	if (q6afe_start_i2s_port(AFE_PORT_ID_QUATERNARY_MI2S_TX, AFE_PORT_I2S_SD0, 0) == 0) {
		afe_tx_running = true;
		voice_tx_port = AFE_PORT_ID_QUATERNARY_MI2S_TX;
	} else {
		voice_tx_port = AFE_PORT_ID_PSEUDOPORT_01;
	}

	if (apr_call_ex(APR_SVC_ADSP_MVM, 1, 0, VSS_IMVM_CMD_CREATE_PASSIVE_CTL_SESSION,
	    session_name, sizeof(session_name), 0, &mvm_h, NULL, 1500) != 0 || mvm_h == 0) {
		adsp_log("q6voice: MVM CREATE_PASSIVE_CONTROL_SESSION failed");
		goto fail;
	}
	if (apr_call_ex(APR_SVC_ADSP_CVS, 1, 0, VSS_ISTREAM_CMD_CREATE_PASSIVE_CTL_SESS,
	    session_name, sizeof(session_name), 0, &cvs_h, NULL, 1500) != 0 || cvs_h == 0) {
		adsp_log("q6voice: CVS CREATE_PASSIVE_CONTROL_SESSION failed");
		goto fail;
	}
	(void)apr_call_ex(APR_SVC_ADSP_MVM, 1, mvm_h, VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL,
	    &dual, sizeof(dual), 0, NULL, NULL, 1000);

	memset(&cvp_create, 0, sizeof(cvp_create));
	cvp_create.direction = 2u; /* RX_TX */
	cvp_create.tx_port_id = voice_tx_port;
	cvp_create.tx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_NONE;
	cvp_create.rx_port_id = AFE_PORT_ID_QUATERNARY_MI2S_RX;
	cvp_create.rx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_NONE;
	cvp_create.profile_id = 0u;
	cvp_create.vocproc_mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	cvp_create.ec_ref_port_id = 0xffffu;
	memcpy(cvp_create.name, session_name, sizeof(session_name));

	if (apr_call_ex(APR_SVC_ADSP_CVP, 1, 0, VSS_IVOCPROC_CMD_CREATE_FULL_CTL_V3,
	    &cvp_create, sizeof(cvp_create), 0, &cvp_h, NULL, 1500) == 0 && cvp_h != 0) {
		cvp_send_v2_v3_media_info(cvp_h, voice_tx_port, AFE_PORT_ID_QUATERNARY_MI2S_RX);
	} else if (apr_call_ex(APR_SVC_ADSP_CVP, 1, 0, VSS_IVOCPROC_CMD_CREATE_FULL_CTL_V2,
	    &cvp_create, sizeof(cvp_create), 0, &cvp_h, NULL, 1500) != 0 || cvp_h == 0) {
		adsp_log("q6voice: CVP CREATE_FULL_CONTROL_SESSION failed");
		goto fail;
	}

	if (apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, VSS_IVOCPROC_CMD_ENABLE,
	    NULL, 0, 0, NULL, NULL, 1500) != 0) {
		adsp_log("q6voice: CVP ENABLE failed");
		goto fail;
	}

	memset(&attach, 0, sizeof(attach));
	attach.handle = cvp_h;
	if (apr_call_ex(APR_SVC_ADSP_MVM, 1, mvm_h, VSS_IMVM_CMD_ATTACH_VOCPROC,
	    &attach, 2u, 0, NULL, NULL, 1500) != 0) {
		adsp_log("q6voice: MVM ATTACH_VOCPROC failed");
		goto fail;
	}

	/* Unmute RX (1) and TX (0) on CVS */
	(void)apr_call_ex(APR_SVC_ADSP_CVS, 1, cvs_h, VSS_IVOLUME_CMD_MUTE_V2,
	    &unmute, 6u, 0, NULL, NULL, 800);
	unmute.direction = 0u;
	(void)apr_call_ex(APR_SVC_ADSP_CVS, 1, cvs_h, VSS_IVOLUME_CMD_MUTE_V2,
	    &unmute, 6u, 0, NULL, NULL, 800);

	if (apr_call_ex(APR_SVC_ADSP_MVM, 1, mvm_h, VSS_IMVM_CMD_START_VOICE,
	    NULL, 0, 0, NULL, NULL, 1500) != 0) {
		adsp_log("q6voice: MVM START_VOICE failed");
		goto fail;
	}

	g_status.mvm_handle = mvm_h;
	g_status.cvs_handle = cvs_h;
	g_status.cvp_handle = cvp_h;
	g_status.voice_active = true;
	g_status.voice_speakerphone = speakerphone;
	snprintf(g_status.state, sizeof(g_status.state), "voice_call");
	(void)max98927_set_voice_mode(true, speakerphone);
	adsp_log("q6voice: active (mvm=0x%x cvs=0x%x cvp=0x%x)", mvm_h, cvs_h, cvp_h);
	adsp_status_write();
	return 0;

fail:
	if (cvp_h != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, cvp_h, APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 500);
	}
	if (cvs_h != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_CVS, 1, cvs_h, APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 500);
	}
	if (mvm_h != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_MVM, 1, mvm_h, APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 500);
	}
	if (afe_tx_running) {
		q6afe_stop_port(AFE_PORT_ID_QUATERNARY_MI2S_TX);
		afe_tx_running = false;
	}
	(void)q6_open_media_stream();
	return -1;
}

static void
q6_voice_stop(void)
{
	struct {
		uint16_t handle;
	} __attribute__((packed)) detach = { .handle = g_status.cvp_handle };

	if (!g_status.voice_active) {
		return;
	}
	adsp_log("q6voice: stopping cellular voice session");

	if (g_status.mvm_handle != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_MVM, 1, g_status.mvm_handle,
		    VSS_IMVM_CMD_STOP_VOICE, NULL, 0, 0, NULL, NULL, 1000);
		if (g_status.cvp_handle != 0) {
			(void)apr_call_ex(APR_SVC_ADSP_MVM, 1, g_status.mvm_handle,
			    VSS_IMVM_CMD_DETACH_VOCPROC, &detach, sizeof(detach), 0, NULL, NULL, 1000);
		}
	}
	if (g_status.cvp_handle != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, g_status.cvp_handle,
		    VSS_IVOCPROC_CMD_DISABLE, NULL, 0, 0, NULL, NULL, 800);
		(void)apr_call_ex(APR_SVC_ADSP_CVP, 1, g_status.cvp_handle,
		    APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 800);
	}
	if (g_status.cvs_handle != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_CVS, 1, g_status.cvs_handle,
		    APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 800);
	}
	if (g_status.mvm_handle != 0) {
		(void)apr_call_ex(APR_SVC_ADSP_MVM, 1, g_status.mvm_handle,
		    APR_CMD_DESTROY_SESSION, NULL, 0, 0, NULL, NULL, 800);
	}
	if (afe_tx_running) {
		q6afe_stop_port(AFE_PORT_ID_QUATERNARY_MI2S_TX);
		afe_tx_running = false;
	}

	g_status.mvm_handle = 0;
	g_status.cvs_handle = 0;
	g_status.cvp_handle = 0;
	g_status.voice_active = false;
	g_status.voice_speakerphone = false;

	(void)max98927_set_voice_mode(false, false);
	(void)max98927_set_enable(false);
	(void)q6_open_media_stream();
	adsp_status_write();
}

/* ============================================================================
 * 4. Top-level Bringup, Real-Time Tick & Teardown
 * ============================================================================ */

int
adsp_q6_bringup_playback(void)
{
	/*
	 * Configure apps_smmu (SID 0x1b21 mask 0x0f -> covers 0x1b21 audio DMA &
	 * 0x1b23 adsprpc-smd!), hyp_assign the 4 MiB-aligned remote heap, and spawn
	 * the "audiopd" static PD over FastRPC BEFORE issuing Q6AFE/Q6ADM/Q6ASM APR
	 * commands so audio_pd registers all Elite/AVS APR services first.
	 */
	if (adsp_smmu_setup() != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error), "apps_smmu 0x1b21 setup failed");
		return -1;
	}
	(void)adsp_hyp_assign_fastrpc_heap();
	if (adsp_fastrpc_spawn_audiopd() != 0) {
		adsp_log("fastrpc: warning: audiopd static PD spawn did not ACK, continuing");
	}
	/* Pump reverse-RPC (apps_remotectl / apps_std ACDB load) briefly while audiopd initializes */
	(void)adsp_glink_poll(250);

	if (q6core_wait_ready(15000) != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error), "Q6Core AVCS not ready");
		return -1;
	}
	tlmm_setup_quat_mi2s();

	/*
	 * Right after CREATE_STATIC("audiopd") returns, audio_pd finishes loading
	 * adsp_avs_config.acdb via apps_std reverse-RPC and registering Q6AFE (svc=4).
	 * Retry Q6AFE clock setup for up to ~4s while pumping GLINK/reverse-RPC.
	 */
	{
		int clk_rc = -1;
		for (int attempt = 0; attempt < 8 && !g_stop_requested; attempt++) {
			clk_rc = q6afe_set_quad_mi2s_clk(1536000u);
			if (clk_rc == 0) {
				break;
			}
			(void)adsp_glink_poll(300);
		}
		if (clk_rc != 0) {
			adsp_log("q6afe: warning: QUAD_MI2S_IBIT clock set returned error, continuing");
		}
	}
	if (q6afe_start_i2s_port(AFE_PORT_ID_QUATERNARY_MI2S_RX, AFE_PORT_I2S_SD1, 1) != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error), "Q6AFE 0x1006 start failed");
		return -1;
	}
	g_status.afe_quat_rx_up = true;

	/* Initialize MAX98927L@0x39 & MAX98927R@0x3a while I2S BCLK/WS are live */
	if (max98927_init_stereo() != 0) {
		adsp_log("max98927: warning: neither 0x39 nor 0x3a responded to probe");
	}

	if (q6_open_media_stream() != 0) {
		snprintf(g_status.last_error, sizeof(g_status.last_error), "Q6ADM/Q6ASM stream open failed");
		return -1;
	}
	last_nonsilent_ms = 0;
	return 0;
}

void
adsp_q6_play_tone_Start(unsigned duration_ms)
{
	tone_frames_total = ((uint64_t)duration_ms * HAKIOS_AUDIO_RATE) / 1000ULL;
	tone_frames_left = tone_frames_total;
	tone_phase_idx = 0;
}

bool
adsp_q6_tone_active(void)
{
	return tone_frames_left > 0;
}

void
adsp_q6_tick(void)
{
	uint64_t cur = now_ms();

	if (cur - last_voice_poll_ms >= 100ULL) {
		bool want_spk = false;
		bool want_voice = detect_voice_call_wanted(&want_spk);
		last_voice_poll_ms = cur;

		if (want_voice && !g_status.voice_active) {
			(void)q6_voice_start(want_spk);
		} else if (!want_voice && g_status.voice_active) {
			q6_voice_stop();
		} else if (want_voice && g_status.voice_active &&
		    want_spk != g_status.voice_speakerphone) {
			g_status.voice_speakerphone = want_spk;
			(void)max98927_set_voice_mode(true, want_spk);
			adsp_status_write();
		}
	}

	if (g_status.voice_active || !g_status.asm_stream_up) {
		return;
	}

	int16_t pcm_buf[ADSP_PCM_PERIOD_FRAMES * HAKIOS_AUDIO_CHANNELS];
	for (uint32_t p = 0; p < ADSP_DMA_PERIODS; p++) {
		if (!period_ready[p]) {
			continue;
		}
		bool signal = fill_period_pcm(pcm_buf, ADSP_PCM_PERIOD_FRAMES);
		uint64_t pa = ADSP_DMA_PA + (uint64_t)p * ADSP_DMA_PERIOD_ALLOC;
		(void)pm_wr(pa, pcm_buf, ADSP_PCM_PERIOD_BYTES);
		(void)q6_send_period_write(p);

		if (signal) {
			last_nonsilent_ms = cur;
			if (!g_status.amp_enabled) {
				(void)max98927_set_enable(true);
			}
		}
	}

	if (g_status.amp_enabled && last_nonsilent_ms != 0 &&
	    cur - last_nonsilent_ms > AMP_SILENCE_SHUTOFF_MS) {
		(void)max98927_set_enable(false);
	}
}

void
adsp_q6_teardown(void)
{
	(void)max98927_set_enable(false);
	if (g_status.voice_active) {
		q6_voice_stop();
	}
	q6_close_media_stream();
	if (g_status.asm_map_handle != 0) {
		uint32_t h = g_status.asm_map_handle;
		(void)apr_call(APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS,
		    &h, sizeof(h), 0, NULL);
		g_status.asm_map_handle = 0;
	}
	if (g_status.afe_quat_rx_up) {
		q6afe_stop_port(AFE_PORT_ID_QUATERNARY_MI2S_RX);
		(void)q6afe_set_quad_mi2s_clk(0);
		g_status.afe_quat_rx_up = false;
	}
}
