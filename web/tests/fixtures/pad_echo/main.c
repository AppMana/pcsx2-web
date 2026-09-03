/* pad_echo: echoes the DualShock 2 state once per vsync and plays a tone.
 *
 * The pad is read through the BIOS's own SIO2MAN and PADMAN modules with
 * ps2sdk's libpad, in DualShock (analog, locked) mode with pressure
 * readings enabled. Every vsync prints the raw button word (active low,
 * 0xffff idle), the four stick bytes and the twelve pressure bytes, so an
 * input recording replayed by the native oracle and a pad schedule applied
 * in the browser must produce the same console lines at the same vsyncs.
 *
 * The tone is a pair of integer triangle waves (about 440 Hz left, 657 Hz
 * right) streamed to the SPU2 through audsrv, so the SPU2 output hashed by
 * the oracle and pulled by the browser's audio worklet is not silence. */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <libpad.h>
#include <audsrv.h>
#include <dma.h>
#include <packet.h>
#include <graph.h>
#include "fixture_tty.h"
#include "fixture_gs.h"

#define PAD_PORT 0
#define PAD_SLOT 0
#define PAD_WAIT_VSYNCS 120
#define ECHO_FRAMES 900

/* 48 kHz stereo 16 bit: TONE_FRAMES frames looped, fed CHUNK_BYTES at a time. */
#define TONE_FRAMES 4800
#define TONE_BYTES (TONE_FRAMES * 4)
#define CHUNK_BYTES 3200
#define CHUNKS_PER_VSYNC 4
#define LEFT_PERIOD 109
#define RIGHT_PERIOD 73
#define AMPLITUDE 6000

extern unsigned char audsrv_irx[];
extern unsigned char audsrv_irx_end[];

static char pad_buffer[256] __attribute__((aligned(64)));
static s16 tone[TONE_FRAMES * 2] __attribute__((aligned(64)));

static char *put_hex(char *p, u32 value, int digits)
{
	static const char hex[] = "0123456789abcdef";
	int shift;
	for (shift = (digits - 1) * 4; shift >= 0; shift -= 4)
		*p++ = hex[(value >> shift) & 0xf];
	return p;
}

static char *put_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static void print_pad(const struct padButtonStatus *pad)
{
	char line[96];
	char *p = line;
	p = put_str(p, "PAD=");
	p = put_hex(p, pad->btns, 4);
	p = put_str(p, " LX=");
	p = put_hex(p, pad->ljoy_h, 2);
	p = put_str(p, " LY=");
	p = put_hex(p, pad->ljoy_v, 2);
	p = put_str(p, " RX=");
	p = put_hex(p, pad->rjoy_h, 2);
	p = put_str(p, " RY=");
	p = put_hex(p, pad->rjoy_v, 2);
	*p = '\0';
	fixture_tty_line(line);

	/* right, left, up, down, triangle, circle, cross, square, l1, r1, l2, r2 */
	p = line;
	p = put_str(p, "PRESS=");
	p = put_hex(p, pad->right_p, 2);
	p = put_hex(p, pad->left_p, 2);
	p = put_hex(p, pad->up_p, 2);
	p = put_hex(p, pad->down_p, 2);
	p = put_hex(p, pad->triangle_p, 2);
	p = put_hex(p, pad->circle_p, 2);
	p = put_hex(p, pad->cross_p, 2);
	p = put_hex(p, pad->square_p, 2);
	p = put_hex(p, pad->l1_p, 2);
	p = put_hex(p, pad->r1_p, 2);
	p = put_hex(p, pad->l2_p, 2);
	p = put_hex(p, pad->r2_p, 2);
	*p = '\0';
	fixture_tty_line(line);
}

static s16 triangle(u32 n, u32 period)
{
	u32 phase = n % period;
	u32 half = period / 2;
	s32 value;
	if (phase < half)
		value = (s32)(phase * 2 * AMPLITUDE / half) - AMPLITUDE;
	else
		value = AMPLITUDE - (s32)((phase - half) * 2 * AMPLITUDE / (period - half));
	return (s16)value;
}

static void fill_tone(void)
{
	u32 n;
	for (n = 0; n < TONE_FRAMES; n++) {
		tone[n * 2] = triangle(n, LEFT_PERIOD);
		tone[n * 2 + 1] = triangle(n, RIGHT_PERIOD);
	}
}

static int audio_init(void)
{
	struct audsrv_fmt_t format;
	int ret = 0;
	int id;

	ret = sbv_patch_enable_lmb();
	fixture_tty_kv_hex("SBV_LMB", (u32)ret);
	ret = sbv_patch_disable_prefix_check();
	fixture_tty_kv_hex("SBV_PREFIX", (u32)ret);

	id = SifLoadModule("rom0:LIBSD", 0, NULL);
	fixture_tty_kv_str("LIBSD", id >= 0 ? "ok" : "fail");
	if (id < 0)
		return -1;

	id = SifExecModuleBuffer(audsrv_irx, audsrv_irx_end - audsrv_irx, 0, NULL, &ret);
	fixture_tty_kv_hex("AUDSRV_ID", (u32)id);
	fixture_tty_kv_hex("AUDSRV_RET", (u32)ret);
	fixture_tty_kv_str("AUDSRV", (id >= 0 && ret == 0) ? "ok" : "fail");
	if (id < 0 || ret != 0)
		return -1;

	if (audsrv_init() != 0) {
		fixture_tty_kv_str("AUDIO", "init_fail");
		return -1;
	}
	format.freq = 48000;
	format.bits = 16;
	format.channels = 2;
	if (audsrv_set_format(&format) != 0) {
		fixture_tty_kv_str("AUDIO", "format_fail");
		return -1;
	}
	audsrv_set_volume(MAX_VOLUME);
	fixture_tty_kv_str("AUDIO", "ok");
	return 0;
}

static u32 audio_feed(u32 offset)
{
	int chunks;
	for (chunks = 0; chunks < CHUNKS_PER_VSYNC; chunks++) {
		if (audsrv_available() < CHUNK_BYTES)
			break;
		audsrv_play_audio((const char *)tone + offset, CHUNK_BYTES);
		offset += CHUNK_BYTES;
		if (offset >= TONE_BYTES)
			offset = 0;
	}
	return offset;
}

static int pad_wait_ready(void)
{
	int waited = 0;
	for (;;) {
		int state = padGetState(PAD_PORT, PAD_SLOT);
		if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1)
			return waited;
		if (state == PAD_STATE_DISCONN || waited >= PAD_WAIT_VSYNCS)
			return -1;
		graph_wait_vsync();
		waited++;
	}
}

static int pad_init(void)
{
	int id;
	int ret;

	id = SifLoadModule("rom0:SIO2MAN", 0, NULL);
	fixture_tty_kv_str("SIO2MAN", id >= 0 ? "ok" : "fail");
	if (id < 0)
		return -1;
	id = SifLoadModule("rom0:PADMAN", 0, NULL);
	fixture_tty_kv_str("PADMAN", id >= 0 ? "ok" : "fail");
	if (id < 0)
		return -1;

	padInit(0);
	ret = padPortOpen(PAD_PORT, PAD_SLOT, pad_buffer);
	fixture_tty_kv_str("PADOPEN", ret != 0 ? "ok" : "fail");
	if (ret == 0)
		return -1;

	ret = pad_wait_ready();
	fixture_tty_kv_dec("PADREADY", (u32)ret);
	if (ret < 0)
		return -1;

	ret = padSetMainMode(PAD_PORT, PAD_SLOT, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
	fixture_tty_kv_dec("PADANALOG", (u32)ret);
	ret = pad_wait_ready();
	if (ret < 0)
		return -1;

	ret = padEnterPressMode(PAD_PORT, PAD_SLOT);
	fixture_tty_kv_dec("PADPRESS", (u32)ret);
	ret = pad_wait_ready();
	if (ret < 0)
		return -1;
	return 0;
}

static void draw_background(void)
{
	packet_t *packet = packet_init(16, PACKET_NORMAL);
	qword_t *q = packet->data;
	q = fixture_gs_clear(q, 0x20, 0x10, 0x30);
	q = draw_finish(q);
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	draw_wait_finish();
	packet_free(packet);
}

int main(int argc, char *argv[])
{
	framebuffer_t frame;
	zbuffer_t z;
	struct padButtonStatus pad;
	int audio_ok;
	int pad_ok;
	int mode_printed = 0;
	u32 tone_offset = 0;
	u32 n;

	fixture_tty_init();
	fixture_tty_kv_str("HELLO", "pad_echo");

	SifInitRpc(0);

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	fixture_gs_init(&frame, &z);
	draw_background();

	fill_tone();
	audio_ok = audio_init() == 0;
	pad_ok = pad_init() == 0;
	fixture_tty_line("READY");

	for (n = 0; n < ECHO_FRAMES; n++) {
		graph_wait_vsync();
		if (audio_ok)
			tone_offset = audio_feed(tone_offset);
		if (!pad_ok) {
			fixture_tty_kv_str("PAD", "none");
			continue;
		}
		if (padRead(PAD_PORT, PAD_SLOT, &pad) == 0) {
			fixture_tty_kv_str("PAD", "read_fail");
			continue;
		}
		if (!mode_printed) {
			fixture_tty_kv_hex("PADMODE", pad.mode);
			mode_printed = 1;
		}
		print_pad(&pad);
	}
	fixture_tty_line("DONE");

	for (;;)
		graph_wait_vsync();

	return 0;
}
