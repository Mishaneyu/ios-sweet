/*
 * SPDX-License-Identifier: MIT
 *
 * adspd.h: Userland Qualcomm SM8150 LPASS ADSP loader, multi-channel GLINK,
 * QRTR pd-mapper/tftp, APR v2 (Q6Core/Q6AFE/Q6ADM/Q6ASM + MVM/CVS/CVP voice),
 * and Maxim MAX98927L/R stereo speaker amplifier driver for RMX1931.
 */
#ifndef SWEET_ADSPD_H
#define SWEET_ADSPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../audio/hakios.audio.h"

/* ---- File paths ---- */
#define ADSP_FW_DIR                 "/var/local/fw/modem"
#define ADSP_LOG_PATH               "/var/local/adspd.log"
#define ADSP_STATUS_PATH            "/var/local/adsp-status.json"
#define ADSP_OFF_PATH               "/var/local/adspd.off"
#define QRTR_MUX_SOCK_PATH          "/var/run/sweet-qrtr.sock"
#define VOICE_CALL_STATE_PATH       "/tmp/sweet-voice-call.state"

/* ---- SM8150 LPASS / ADSP hardware constants ---- */
#define PAS_ADSP                    1u
#define ADSP_CARVEOUT_PA            0x9b200000ULL
#define ADSP_CARVEOUT_SIZE          0x01e00000ULL /* 30 MiB */

/*
 * Uncached <4 GiB HLOS DRAM audio DMA pool in 0xC0000000..0xC0800000 (inside
 * the 64 MiB 0xC0000000..0xC4000000 window left untouched by XNU >=0x100000000,
 * sweet_gpu.c 0xC4000000..0xE0000000 / 0xE1000000..0x100000000, and wland
 * 0xE0000000..0xE1000000). LPASS Q6 uses 32-bit IOVA + (cb_idx << 32) where
 * SID 0x1b21 maps via (1ULL << 32) through our apps_smmu Stage-1 L1 page table.
 */
#define ADSP_DMA_PA                 0xC0000000ULL
#define ADSP_DMA_IOVA               0x10000000u   /* msm_audio_ion va_start (bit 31=0) */
#define ADSP_DMA_PERIODS            8u
#define ADSP_DMA_PERIOD_ALLOC       4096u
#define ADSP_DMA_TOTAL_BYTES        (ADSP_DMA_PERIODS * ADSP_DMA_PERIOD_ALLOC) /* 32768 B */
#define ADSP_PCM_PERIOD_FRAMES      960u  /* 20 ms @ 48 kHz */
#define ADSP_PCM_PERIOD_BYTES       (ADSP_PCM_PERIOD_FRAMES * HAKIOS_AUDIO_FRAME) /* 3840 B */
#define ADSP_DSP_IOVA_HIGH          0x00000001u

/* 3-level VMSAv8-64 Stage-1 page tables (qcom,use-3-lvl-tables) for apps_smmu */
#define ADSP_SMMU_PT_PA             0xC0008000ULL /* L1 table */
#define ADSP_SMMU_L2_PA             0xC0009000ULL /* L2 table (0x10000000..0x101FFFFF) */
#define ADSP_SMMU_L3_PA             0xC000A000ULL /* L3 4 KiB pages -> 0xC0000000 */
#define ADSP_SMMU_PT_SIZE           0x00001000ULL

/*
 * FastRPC static PD ("audiopd") & reverse-RPC buffers in <4 GiB unmanaged DRAM:
 *   - 0xC0010000..0xC0014000 (16 KiB): sync CREATE_STATIC / listener_init2 page
 *   - 0xC0014000..0xC0024000 (64 KiB): async adsp_listener_next2 reverse-RPC page
 *   - 0xC0400000..0xC0800000 (4 MiB, 4 MiB-aligned per live.dtb adsp_region):
 *     remote heap hyp_assigned to {HLOS, MSS_MSA, SSC_Q6, ADSP_Q6} RWX (2 MiB passed
 *     to FASTRPC_INIT_CREATE_STATIC as pure 32-bit PA).
 *   adsprpc-smd (SID 0x1b23) maps invoke pages via cb=3 -> (3ULL << 32).
 */
#define ADSP_FASTRPC_CB_HIGH        0x00000003u
#define ADSP_FASTRPC_MSG_PA         0xC0010000ULL
#define ADSP_FASTRPC_MSG_SIZE       0x00004000ULL
#define ADSP_FASTRPC_LST_PA         0xC0014000ULL
#define ADSP_FASTRPC_LST_SIZE       0x00010000ULL
#define ADSP_FASTRPC_HEAP_PA        0xC0400000ULL
#define ADSP_FASTRPC_HEAP_HYP_SIZE  0x00400000ULL
#define ADSP_FASTRPC_HEAP_SIZE      0x00200000ULL

/* SMEM & APCS IPC doorbells (live.dtb verified: qcom,smem@86000000) */
#define SMEM_BASE_PA                0x86000000ULL
#define SMEM_SIZE_BYTES             0x00200000ULL
#define SMEM_HOST_APPS              0u
#define SMEM_HOST_ADSP              2u
#define SMEM_HOST_GLOBAL            0xfffeU

#define APCS_IPC_PA                 0x17c0000cULL
#define IPC_BIT_AOSS                0u
#define IPC_BIT_ADSP_GLINK          8u   /* mboxes = <&apss_shared 8> */
#define IPC_BIT_ADSP_SMP2P          10u  /* qcom,ipc = <&apss_shared 0 10> */

#define SMP2P_ADSP_OUT_ITEM         429u /* master-kernel */
#define SMP2P_ADSP_IN_ITEM          443u /* slave-kernel */

/* apps_smmu@15000000 stream ID for msm_audio_ion */
#define APPS_SMMU_PA                0x15000000ULL
#define ADSP_AUDIO_SMMU_SID         0x1b21u
#define ADSP_AUDIO_SMMU_MASK        0x000fu

/* SM8150 TLMM tiles (pinctrl-sm8150.c verified) */
#define TLMM_WEST_PA                0x03100000ULL
#define TLMM_EAST_PA                0x03500000ULL
#define TLMM_NORTH_PA               0x03900000ULL
#define TLMM_SOUTH_PA               0x03d00000ULL
#define TLMM_GPIO_STRIDE            0x1000ULL

/* RMX1931 Quaternary MI2S & MAX98927 GPIOs (live.dtb verified) */
#define GPIO_QUAT_MI2S_SCK          137u /* SOUTH, func 1 */
#define GPIO_QUAT_MI2S_WS           138u /* SOUTH, func 1 */
#define GPIO_QUAT_MI2S_DATA0        139u /* SOUTH, func 1 (DIN / SD0) */
#define GPIO_QUAT_MI2S_DATA1        140u /* SOUTH, func 1 (DOUT / SD1) */

#define GPIO_SPK_I2C_SDA            51u  /* SOUTH (qup4_i2c SDA) */
#define GPIO_SPK_I2C_SCL            52u  /* SOUTH (qup4_i2c SCL) */
#define GPIO_MAX98927L_RESET        148u /* SOUTH, active-low */
#define GPIO_MAX98927R_RESET        37u  /* NORTH, active-low */

#define MAX98927L_I2C_ADDR          0x39u
#define MAX98927R_I2C_ADDR          0x3au

/* ---- Daemon runtime telemetry (/var/local/adsp-status.json) ---- */
struct adsp_status {
	int      pid;
	bool     adsp_booted;
	bool     smp2p_ready;
	bool     smp2p_handover;
	bool     proxy_votes_released;
	bool     glink_ipcrtr_up;
	bool     glink_apr_up;
	bool     glink_fastrpc_up;
	bool     audiopd_spawned;
	bool     q6_core_ready;
	bool     smmu_configured;
	int      smmu_cb;
	bool     afe_quat_rx_up;
	bool     adm_copp_up;
	int      adm_copp_id;
	bool     asm_stream_up;
	uint32_t asm_map_handle;
	bool     amp_left_ok;
	bool     amp_right_ok;
	uint8_t  amp_left_rev;
	uint8_t  amp_right_rev;
	bool     amp_enabled;
	bool     voice_active;
	bool     voice_speakerphone;
	uint16_t mvm_handle;
	uint16_t cvs_handle;
	uint16_t cvp_handle;
	uint64_t periods_written;
	uint64_t frames_played;
	uint64_t underruns;
	uint32_t pd_queries;
	uint32_t tftp_reads;
	uint32_t fastrpc_rev_calls;
	char     state[32];
	char     last_error[96];
};

extern struct adsp_status g_status;
extern volatile int g_stop_requested;
extern struct sweet_audio_shm *g_shm;

/* ---- Logging & status helpers (adspd.c) ---- */
void adsp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void adsp_status_write(void);

/* ---- Hardware, SMEM, PAS, SMMU, TLMM & I2C (adsp_hw.c) ---- */
int      pm_open_dev(void);
uint32_t rd32(uint64_t pa);
void     wr32(uint64_t pa, uint32_t v);
int      pm_rd(uint64_t pa, void *buf, size_t len);
int      pm_wr(uint64_t pa, const void *buf, size_t len);
uint64_t pm_alloc(uint64_t size, uint64_t align);
int      pm_free(uint64_t pa);
uint64_t now_ms(void);
void     delay_ms(unsigned ms);
void     apcs_kick(unsigned bit);

int      smem_init_adsp(void);
int      smem_get_item(unsigned host, unsigned item, uint64_t *pa, uint32_t *size);
int      smem_alloc_item(unsigned host, unsigned item, uint32_t size, uint64_t *pa);

void     smp2p_adsp_service(void);
int      adsp_pas_boot(void);
int      adsp_pas_shutdown(void);
int      adsp_smmu_setup(void);
void     adsp_smmu_check_fault(const char *tag);
int      adsp_hyp_assign_fastrpc_heap(void);

void     tlmm_setup_quat_mi2s(void);
void     tlmm_gpio_drive(uint64_t tile_pa, unsigned pin, int high);
int      spk_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *val);
int      spk_i2c_write_reg16(uint8_t addr7, uint16_t reg, uint8_t val);
int      spk_i2c_probe7(uint8_t addr7);
void     spk_i2c_scan_bus(void);

/* ---- Maxim MAX98927L/R stereo amplifier (max98927.c) ---- */
int      max98927_init_stereo(void);
int      max98927_set_enable(bool enable);
int      max98927_set_voice_mode(bool voice_call, bool speakerphone);

/* ---- Multi-channel GLINK + QRTR pd-mapper/tftp + FastRPC audiopd (adsp_glink.c) ---- */
int      adsp_glink_prepare(void);
int      adsp_glink_bringup(unsigned timeout_ms);
int      adsp_glink_poll(unsigned wait_ms);
int      adsp_fastrpc_spawn_audiopd(void);
int      adsp_apr_send(const void *pkt, uint32_t len);
bool     adsp_apr_is_open(void);

/* ---- Q6Core / Q6AFE / Q6ADM / Q6ASM / Q6Voice (adsp_q6.c) ---- */
void     adsp_apr_rx(const uint8_t *data, uint32_t len);
int      adsp_q6_bringup_playback(void);
void     adsp_q6_tick(void);
void     adsp_q6_play_tone_Start(unsigned duration_ms);
bool     adsp_q6_tone_active(void);
void     adsp_q6_teardown(void);

#endif /* SWEET_ADSPD_H */
