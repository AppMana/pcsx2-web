/* gs_blend: cycles through GS alpha blending equations, a procedurally
 * generated 64x64 RGBA texture and the Z test, changing mode every 30
 * frames. Prints FRAME=n every frame and MODE=m whenever the mode changes. */

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <packet.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include "fixture_tty.h"
#include "fixture_gs.h"

#define Q_ONE 0x3f800000u
#define TEX_SIZE 64
#define TEX_LOG2 6
#define FRAMES_PER_MODE 30

/* Depths: the Z test is GREATER_EQUAL, so larger Z is nearer. */
#define Z_FAR   0x00100000u
#define Z_MID   0x00200000u
#define Z_NEAR  0x00300000u
#define Z_FRONT 0x00400000u

typedef struct {
	int abe;
	int a, b, c, d;
	int fix;
	int ztest;
	int tme;
} mode_t_;

/* Blend colour = (A - B) * C >> 7 + D, with 0 = source, 1 = destination,
 * 2 = zero for A/B/D and 0 = source alpha, 1 = destination alpha, 2 = FIX
 * for C. */
static const mode_t_ modes[8] = {
	{0, 0, 1, 0, 1, 0x80, 0, 1}, /* no blending, no Z test, textured */
	{1, 0, 1, 0, 1, 0x80, 0, 1}, /* Cs*As + Cd*(1-As) */
	{1, 0, 2, 0, 1, 0x80, 0, 1}, /* Cs*As + Cd, additive */
	{1, 2, 1, 0, 1, 0x80, 0, 1}, /* Cd*(1-As), subtractive darken */
	{1, 0, 1, 2, 1, 0x40, 0, 0}, /* fixed alpha 0x40, flat colours */
	{1, 0, 1, 2, 1, 0xc0, 1, 0}, /* fixed alpha 0xc0 (above 1.0), Z test on */
	{1, 0, 1, 1, 1, 0x80, 1, 1}, /* destination alpha, Z test on */
	{1, 1, 0, 0, 2, 0x80, 1, 1}, /* (Cd-Cs)*As, Z test on */
};

static u32 texture[TEX_SIZE * TEX_SIZE] __attribute__((aligned(64)));

/* Checkerboard of two colours in 8 pixel cells, alpha rising left to right,
 * a white diagonal stripe every 16 pixels. */
static void generate_texture(void)
{
	int x, y;
	for (y = 0; y < TEX_SIZE; y++) {
		for (x = 0; x < TEX_SIZE; x++) {
			u32 r, g, b, a;
			if (((x >> 3) + (y >> 3)) & 1) {
				r = 0xff; g = 0x40; b = 0x20;
			} else {
				r = 0x20; g = 0x60; b = 0xff;
			}
			a = (u32)(x * 2);
			if (((x + y) & 15) == 0) {
				r = 0xff; g = 0xff; b = 0xff; a = 0x80;
			}
			texture[y * TEX_SIZE + x] = r | (g << 8) | (b << 16) | (a << 24);
		}
	}
}

/* Host to local transfer: BITBLTBUF/TRXPOS/TRXREG/TRXDIR, then an IMAGE tag
 * with the 1024 qwords of PSMCT32 pixels, then TEXFLUSH. */
static void upload_texture(u32 address)
{
	packet_t *packet = packet_init(TEX_SIZE * TEX_SIZE / 4 + 16, PACKET_NORMAL);
	qword_t *q = packet->data;
	u32 i;
	const u128 *src = (const u128 *)texture;

	PACK_GIFTAG(q, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_BITBLTBUF(0, 0, 0, address >> 6, TEX_SIZE >> 6, GS_PSM_32), GS_REG_BITBLTBUF);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXPOS(0, 0, 0, 0, 0), GS_REG_TRXPOS);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXREG(TEX_SIZE, TEX_SIZE), GS_REG_TRXREG);
	q++;
	PACK_GIFTAG(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR);
	q++;
	PACK_GIFTAG(q, GIF_SET_TAG(TEX_SIZE * TEX_SIZE / 4, 1, 0, 0, GIF_FLG_IMAGE, 0), 0);
	q++;
	for (i = 0; i < TEX_SIZE * TEX_SIZE / 4; i++) {
		q->qw = src[i];
		q++;
	}
	q = fixture_gs_ad(q, 0, GS_REG_TEXFLUSH);
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	draw_wait_finish();
	packet_free(packet);
}

/* TEX0/TEX1/CLAMP for the 64x64 RGBA texture, modulate, nearest, repeat. */
static qword_t *texture_registers(qword_t *q, u32 address)
{
	PACK_GIFTAG(q, GIF_SET_TAG(3, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_TEX0(address >> 6, TEX_SIZE >> 6, GS_PSM_32, TEX_LOG2, TEX_LOG2,
	                           TEXTURE_COMPONENTS_RGBA, TEXTURE_FUNCTION_MODULATE, 0, 0, 0, 0, 0), GS_REG_TEX0);
	q++;
	PACK_GIFTAG(q, GS_SET_TEX1(LOD_USE_K, 0, LOD_MAG_NEAREST, LOD_MIN_NEAREST, 0, 0, 0), GS_REG_TEX1);
	q++;
	PACK_GIFTAG(q, GS_SET_CLAMP(WRAP_REPEAT, WRAP_REPEAT, 0, 0, 0, 0), GS_REG_CLAMP);
	q++;
	return q;
}

/* Sprite with UV coordinates (FST=1): PRIM, RGBAQ, UV, XYZ2, UV, XYZ2. UV is
 * 12.4 fixed point texel coordinates. When tme is 0 the UV writes are
 * harmless and the sprite is flat coloured. */
static qword_t *quad(qword_t *q, int x0, int y0, int x1, int y1, u32 z,
                     int u0, int v0, int u1, int v1, int r, int g, int b, int a,
                     const mode_t_ *m)
{
	PACK_GIFTAG(q, GIF_SET_TAG(6, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_PRIM(GIF_PRIM_SPRITE, 0, m->tme, 0, m->abe, 0, 1, 0, 0), GS_REG_PRIM);
	q++;
	PACK_GIFTAG(q, GS_SET_RGBAQ(r, g, b, a, Q_ONE), GS_REG_RGBAQ);
	q++;
	PACK_GIFTAG(q, GS_SET_UV(u0, v0), GS_REG_UV);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(x0), FIXTURE_GS_Y(y0), z), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_UV(u1, v1), GS_REG_UV);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(x1), FIXTURE_GS_Y(y1), z), GS_REG_XYZ2);
	q++;
	return q;
}

typedef struct {
	int x, y;
	int r, g, b, a;
} vertex_rgba_t;

static qword_t *gouraud_triangle(qword_t *q, const vertex_rgba_t *v, u32 z, const mode_t_ *m)
{
	int i;
	PACK_GIFTAG(q, GIF_SET_TAG(7, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_PRIM(GIF_PRIM_TRIANGLE, 1, 0, 0, m->abe, 0, 0, 0, 0), GS_REG_PRIM);
	q++;
	for (i = 0; i < 3; i++) {
		PACK_GIFTAG(q, GS_SET_RGBAQ(v[i].r, v[i].g, v[i].b, v[i].a, Q_ONE), GS_REG_RGBAQ);
		q++;
		PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(v[i].x), FIXTURE_GS_Y(v[i].y), z), GS_REG_XYZ2);
		q++;
	}
	return q;
}

static qword_t *build_frame(qword_t *q, u32 n, const mode_t_ *m, u32 tex_address)
{
	vertex_rgba_t tri[3];
	int scroll = (int)((n * 16) & 1023);

	q = fixture_gs_clear(q, 0x20, 0x20, 0x20);
	q = fixture_gs_ad(q, m->ztest ? FIXTURE_GS_TEST_ZGEQUAL : FIXTURE_GS_TEST_ZALLPASS, GS_REG_TEST);
	q = fixture_gs_ad(q, GS_SET_ALPHA(m->a, m->b, m->c, m->d, m->fix), GS_REG_ALPHA);
	q = texture_registers(q, tex_address);

	/* Near quad first, then the far quad overlapping it: with the Z test on
	 * the far quad loses the overlap, with the Z test off it wins. */
	q = quad(q, 224, 160, 480, 416, Z_NEAR, 0, 0, TEX_SIZE << 4, TEX_SIZE << 4,
	         0x80, 0x80, 0x80, 0x40, m);
	q = quad(q, 96, 64, 352, 320, Z_FAR, scroll, 0, scroll + (TEX_SIZE << 4), TEX_SIZE << 4,
	         0x80, 0x80, 0x80, 0x60, m);

	/* Texture repeated four times across a wide strip, tinted, at mid depth. */
	q = quad(q, 400, 40, 624, 152, Z_MID, 0, 0, (TEX_SIZE * 4) << 4, (TEX_SIZE * 2) << 4,
	         0x80, 0xc0, 0x40, 0x80, m);

	/* Gouraud triangles with alphas 0x80, 0x40 and 0 at mid depth. */
	tri[0].x = 380; tri[0].y = 180; tri[0].r = 0xff; tri[0].g = 0xff; tri[0].b = 0xff; tri[0].a = 0x80;
	tri[1].x = 620; tri[1].y = 260; tri[1].r = 0xff; tri[1].g = 0x00; tri[1].b = 0x00; tri[1].a = 0x40;
	tri[2].x = 520; tri[2].y = 430; tri[2].r = 0x00; tri[2].g = 0x00; tri[2].b = 0xff; tri[2].a = 0x00;
	q = gouraud_triangle(q, tri, Z_MID, m);
	tri[0].x = 20;  tri[0].y = 300; tri[0].r = 0x00; tri[0].g = 0xff; tri[0].b = 0x00; tri[0].a = 0x00;
	tri[1].x = 300; tri[1].y = 440; tri[1].r = 0xff; tri[1].g = 0xff; tri[1].b = 0x00; tri[1].a = 0x80;
	tri[2].x = 60;  tri[2].y = 440; tri[2].r = 0xff; tri[2].g = 0x00; tri[2].b = 0xff; tri[2].a = 0x40;
	q = gouraud_triangle(q, tri, Z_MID, m);

	/* Small flat quad in front of everything, no texture regardless of mode. */
	{
		mode_t_ flat = *m;
		flat.tme = 0;
		q = quad(q, 300, 380, 400, 440, Z_FRONT, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x80, &flat);
	}

	return q;
}

int main(int argc, char *argv[])
{
	framebuffer_t frame;
	zbuffer_t z;
	packet_t *packet;
	qword_t *q;
	u32 tex_address;
	u32 n = 0;
	u32 last_mode = 0xffffffffu;

	fixture_tty_init();
	fixture_tty_kv_str("HELLO", "gs_blend");

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	fixture_gs_init(&frame, &z);

	tex_address = graph_vram_allocate(TEX_SIZE, TEX_SIZE, GS_PSM_32, GRAPH_ALIGN_BLOCK);
	generate_texture();
	upload_texture(tex_address);

	packet = packet_init(128, PACKET_NORMAL);

	for (;;) {
		u32 mode = (n / FRAMES_PER_MODE) % 8;
		if (mode != last_mode) {
			fixture_tty_kv_dec("MODE", mode);
			last_mode = mode;
		}
		dma_wait_fast();
		q = packet->data;
		q = build_frame(q, n, &modes[mode], tex_address);
		q = draw_finish(q);
		dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
		draw_wait_finish();
		fixture_tty_kv_dec("FRAME", n);
		graph_wait_vsync();
		n++;
	}

	return 0;
}
