#include <gs_gp.h>
#include <gs_psm.h>
#include <gif_tags.h>
#include <dma.h>
#include <packet.h>
#include "fixture_gs.h"

void fixture_gs_init(framebuffer_t *frame, zbuffer_t *z)
{
	packet_t *packet;
	qword_t *q;

	frame->width = FIXTURE_GS_WIDTH;
	frame->height = FIXTURE_GS_HEIGHT;
	frame->mask = 0;
	frame->psm = GS_PSM_32;
	frame->address = graph_vram_allocate(frame->width, frame->height, frame->psm, GRAPH_ALIGN_PAGE);

	z->enable = DRAW_ENABLE;
	z->mask = 0;
	z->method = ZTEST_METHOD_ALLPASS;
	z->zsm = GS_ZBUF_32;
	z->address = graph_vram_allocate(frame->width, frame->height, z->zsm, GRAPH_ALIGN_PAGE);

	/* The same sequence graph_initialize performs, with the region fixed to
	 * NTSC instead of read from the ROM, and the flicker filter off. */
	graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_DISABLE);
	graph_set_screen(0, 0, frame->width, frame->height);
	graph_set_bgcolor(0, 0, 0);
	graph_set_framebuffer_filtered(frame->address, frame->width, frame->psm, 0, 0);
	graph_enable_output();

	packet = packet_init(32, PACKET_NORMAL);
	q = packet->data;
	q = draw_setup_environment(q, 0, frame, z);
	q = fixture_gs_ad(q, GS_SET_XYOFFSET(FIXTURE_GS_ORIGIN_X << 4, FIXTURE_GS_ORIGIN_Y << 4), GS_REG_XYOFFSET);
	q = draw_finish(q);
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	draw_wait_finish();
	packet_free(packet);
}

qword_t *fixture_gs_ad(qword_t *q, u64 value, u64 reg)
{
	PACK_GIFTAG(q, GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, value, reg);
	q++;
	return q;
}

qword_t *fixture_gs_clear(qword_t *q, int r, int g, int b)
{
	/* One PACKED tag, 5 registers: TEST, PRIM, RGBAQ, XYZ2, XYZ2. */
	PACK_GIFTAG(q, GIF_SET_TAG(5, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, FIXTURE_GS_TEST_ZALLPASS, GS_REG_TEST);
	q++;
	PACK_GIFTAG(q, GS_SET_PRIM(GIF_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
	q++;
	PACK_GIFTAG(q, GS_SET_RGBAQ(r, g, b, 0x80, 0x3f800000), GS_REG_RGBAQ);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(0), FIXTURE_GS_Y(0), 0), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(FIXTURE_GS_X(FIXTURE_GS_WIDTH), FIXTURE_GS_Y(FIXTURE_GS_HEIGHT), 0), GS_REG_XYZ2);
	q++;
	return q;
}
