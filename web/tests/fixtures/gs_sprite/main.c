/* gs_sprite: flat shaded sprites and gouraud triangles with alpha blending on
 * a 640x448 interlaced frame buffer. Everything is integer arithmetic driven
 * by the frame counter; each frame prints FRAME=n to the EE console. */

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

typedef struct {
	int x, y;
	int r, g, b, a;
} vertex_rgba_t;

/* PACKED A+D: PRIM, RGBAQ, XYZ2, XYZ2. Sprites are always flat shaded. */
static qword_t *sprite(qword_t *q, int x0, int y0, int x1, int y1, u32 z,
                       int r, int g, int b, int a, int abe)
{
	PACK_GIFTAG(q, GIF_SET_TAG(4, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_PRIM(GIF_PRIM_SPRITE, 0, 0, 0, abe, 0, 0, 0, 0), GS_REG_PRIM);
	q++;
	PACK_GIFTAG(q, GS_SET_RGBAQ(r, g, b, a, Q_ONE), GS_REG_RGBAQ);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(x0), FIXTURE_GS_Y(y0), z), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(x1), FIXTURE_GS_Y(y1), z), GS_REG_XYZ2);
	q++;
	return q;
}

/* PACKED A+D: PRIM then RGBAQ, XYZ2 for each of the three vertices. With
 * iip=1 the GS interpolates the vertex colours (gouraud), with iip=0 the
 * colour of the last vertex is used for the whole triangle (flat). */
static qword_t *triangle(qword_t *q, const vertex_rgba_t *v, u32 z, int iip, int abe)
{
	int i;
	PACK_GIFTAG(q, GIF_SET_TAG(7, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_PRIM(GIF_PRIM_TRIANGLE, iip, 0, 0, abe, 0, 0, 0, 0), GS_REG_PRIM);
	q++;
	for (i = 0; i < 3; i++) {
		PACK_GIFTAG(q, GS_SET_RGBAQ(v[i].r, v[i].g, v[i].b, v[i].a, Q_ONE), GS_REG_RGBAQ);
		q++;
		PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(v[i].x), FIXTURE_GS_Y(v[i].y), z), GS_REG_XYZ2);
		q++;
	}
	return q;
}

static qword_t *build_frame(qword_t *q, u32 n)
{
	static const int row_colours[5][3] = {
		{0xff, 0x20, 0x20}, {0x20, 0xff, 0x20}, {0x20, 0x20, 0xff},
		{0xff, 0xff, 0x20}, {0x20, 0xff, 0xff},
	};
	vertex_rgba_t tri[3];
	int i;
	int slide = (int)((n * 4) % (FIXTURE_GS_WIDTH - 80));
	int apex = 320 + (int)((n * 3) % 200) - 100;

	q = fixture_gs_clear(q, 0x18, 0x18, 0x20);

	/* Standard source alpha blend: (Cs - Cd) * As >> 7 + Cd. */
	q = fixture_gs_ad(q, GS_SET_ALPHA(BLEND_COLOR_SOURCE, BLEND_COLOR_DEST, BLEND_ALPHA_SOURCE, BLEND_COLOR_DEST, 0x80), GS_REG_ALPHA);

	/* Five opaque flat sprites in a row. */
	for (i = 0; i < 5; i++)
		q = sprite(q, 40 + i * 120, 40, 140 + i * 120, 120, 0,
		           row_colours[i][0], row_colours[i][1], row_colours[i][2], 0x80, 0);

	/* A translucent white band across the row and a translucent grey band below it. */
	q = sprite(q, 20, 80, 620, 160, 0, 0xff, 0xff, 0xff, 0x40, 1);
	q = sprite(q, 20, 130, 620, 230, 0, 0x60, 0x60, 0x60, 0x60, 1);

	/* Opaque sprite sliding right by four pixels per frame. */
	q = sprite(q, slide, 140, slide + 80, 220, 0, 0xf0, 0xf0, 0xf0, 0x80, 0);

	/* Translucent magenta sprite sliding at half the speed over the same band. */
	q = sprite(q, 560 - slide / 2, 150, 640 - slide / 2, 210, 0, 0xff, 0x00, 0xff, 0x20, 1);

	/* Opaque gouraud triangle with pure red, green and blue corners. */
	tri[0].x = 100; tri[0].y = 260; tri[0].r = 0xff; tri[0].g = 0x00; tri[0].b = 0x00; tri[0].a = 0x80;
	tri[1].x = 300; tri[1].y = 260; tri[1].r = 0x00; tri[1].g = 0xff; tri[1].b = 0x00; tri[1].a = 0x80;
	tri[2].x = 200; tri[2].y = 420; tri[2].r = 0x00; tri[2].g = 0x00; tri[2].b = 0xff; tri[2].a = 0x80;
	q = triangle(q, tri, 0, 1, 0);

	/* Flat triangle: only the last vertex colour counts. */
	tri[0].x = 320; tri[0].y = 250; tri[0].r = 0xff; tri[0].g = 0x00; tri[0].b = 0x00; tri[0].a = 0x80;
	tri[1].x = 440; tri[1].y = 250; tri[1].r = 0x00; tri[1].g = 0xff; tri[1].b = 0x00; tri[1].a = 0x80;
	tri[2].x = 380; tri[2].y = 330; tri[2].r = 0xe0; tri[2].g = 0xa0; tri[2].b = 0x20; tri[2].a = 0x80;
	q = triangle(q, tri, 0, 0, 0);

	/* Translucent gouraud triangle overlapping the two above, alpha fades to zero. */
	tri[0].x = 250; tri[0].y = 240; tri[0].r = 0xff; tri[0].g = 0xff; tri[0].b = 0xff; tri[0].a = 0x80;
	tri[1].x = 560; tri[1].y = 300; tri[1].r = 0x00; tri[1].g = 0xff; tri[1].b = 0xff; tri[1].a = 0x40;
	tri[2].x = 400; tri[2].y = 440; tri[2].r = 0xff; tri[2].g = 0x80; tri[2].b = 0x00; tri[2].a = 0x00;
	q = triangle(q, tri, 0, 1, 1);

	/* Translucent gouraud triangle whose apex sweeps left to right. */
	tri[0].x = apex; tri[0].y = 250; tri[0].r = 0xff; tri[0].g = 0xff; tri[0].b = 0x00; tri[0].a = 0x60;
	tri[1].x = 560; tri[1].y = 430; tri[1].r = 0xff; tri[1].g = 0x00; tri[1].b = 0xff; tri[1].a = 0x60;
	tri[2].x = 80; tri[2].y = 430; tri[2].r = 0x00; tri[2].g = 0xff; tri[2].b = 0xff; tri[2].a = 0x60;
	q = triangle(q, tri, 0, 1, 1);

	return q;
}

int main(int argc, char *argv[])
{
	framebuffer_t frame;
	zbuffer_t z;
	packet_t *packet;
	qword_t *q;
	u32 n = 0;

	fixture_tty_init();
	fixture_tty_kv_str("HELLO", "gs_sprite");

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	fixture_gs_init(&frame, &z);

	packet = packet_init(128, PACKET_NORMAL);

	for (;;) {
		dma_wait_fast();
		q = packet->data;
		q = build_frame(q, n);
		q = draw_finish(q);
		dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
		draw_wait_finish();
		fixture_tty_kv_dec("FRAME", n);
		graph_wait_vsync();
		n++;
	}

	return 0;
}
