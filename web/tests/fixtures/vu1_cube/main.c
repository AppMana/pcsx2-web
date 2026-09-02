/* vu1_cube: a gouraud shaded cube transformed by a VU1 microprogram and sent
 * to the GS with XGKICK. The rotation is a pure function of the frame
 * counter through a fixed sine table; nothing reads a timer. Each frame
 * prints FRAME=n to the EE console. */

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <packet.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include "fixture_tty.h"
#include "fixture_gs.h"
#include "sin_table.h"

extern u32 VU1Cube_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Cube_CodeEnd __attribute__((section(".vudata")));

#define VERTEX_COUNT 36
#define VU_BUFFER_BASE 8
#define VU_BUFFER_OFFSET 496

/* Camera: cube half size 10 at distance 45, near 30, far 60, so every clip
 * coordinate stays inside |w| and the VU clip flags are always zero. */
#define CAMERA_DISTANCE 45.0f
#define NEAR_PLANE 30.0f
#define FAR_PLANE 60.0f
#define FOCAL_X 1.05f
#define FOCAL_Y 1.5f

/* NDC to GS: x = ndc.x * 320 + 2048, y = ndc.y * -224 + 2048, z in 0..1
 * times 0xFFFFF, all scaled by 16 by ftoi4 in the microprogram. */
#define SCALE_X 320.0f
#define SCALE_Y -224.0f
#define SCALE_Z 1048575.0f
#define OFFSET_X 2048.0f
#define OFFSET_Y 2048.0f

typedef struct {
	float x, y, z, w;
} __attribute__((aligned(16))) vec4_t;

typedef struct {
	u32 r, g, b, a;
} __attribute__((aligned(16))) rgba_t;

typedef struct {
	float scale[3];
	s32 count;
	float offset[4];
	u64 prim_tag[2];
	u64 finish_tag[2];
	u64 finish_data[2];
} __attribute__((aligned(16))) vu_header_t;

static const float corners[8][3] = {
	{-10.0f, -10.0f, -10.0f}, {10.0f, -10.0f, -10.0f},
	{10.0f, 10.0f, -10.0f},   {-10.0f, 10.0f, -10.0f},
	{-10.0f, -10.0f, 10.0f},  {10.0f, -10.0f, 10.0f},
	{10.0f, 10.0f, 10.0f},    {-10.0f, 10.0f, 10.0f},
};

/* Six faces as quads, split into two triangles each. */
static const int faces[6][4] = {
	{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
	{1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
};

static const u32 face_colours[6][3] = {
	{0xff, 0x30, 0x30}, {0x30, 0xff, 0x30}, {0x30, 0x30, 0xff},
	{0xff, 0xff, 0x30}, {0xff, 0x30, 0xff}, {0x30, 0xff, 0xff},
};

static vec4_t vertices[VERTEX_COUNT] __attribute__((aligned(16)));
static rgba_t colours[VERTEX_COUNT] __attribute__((aligned(16)));
static vu_header_t header __attribute__((aligned(16)));
static float matrix[16] __attribute__((aligned(16)));

static void build_mesh(void)
{
	static const int tri[6] = {0, 1, 2, 0, 2, 3};
	int f, i;
	int n = 0;
	for (f = 0; f < 6; f++) {
		for (i = 0; i < 6; i++) {
			int c = faces[f][tri[i]];
			u32 shade = 0x80 - 0x18 * (u32)tri[i];
			vertices[n].x = corners[c][0];
			vertices[n].y = corners[c][1];
			vertices[n].z = corners[c][2];
			vertices[n].w = 1.0f;
			colours[n].r = (face_colours[f][0] * shade) >> 7;
			colours[n].g = (face_colours[f][1] * shade) >> 7;
			colours[n].b = (face_colours[f][2] * shade) >> 7;
			colours[n].a = 0x80;
			n++;
		}
	}
}

static void build_header(void)
{
	header.scale[0] = SCALE_X;
	header.scale[1] = SCALE_Y;
	header.scale[2] = SCALE_Z;
	header.count = VERTEX_COUNT;
	header.offset[0] = OFFSET_X;
	header.offset[1] = OFFSET_Y;
	header.offset[2] = 0.0f;
	header.offset[3] = 0.0f;
	/* PACKED, PRE=1 with a gouraud triangle PRIM, two registers per vertex. */
	header.prim_tag[0] = GIF_SET_TAG(VERTEX_COUNT, 0, 1,
	                                 GS_SET_PRIM(GIF_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0),
	                                 GIF_FLG_PACKED, 2);
	header.prim_tag[1] = ((u64)GIF_REG_RGBAQ) | (((u64)GIF_REG_XYZ2) << 4);
	header.finish_tag[0] = GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1);
	header.finish_tag[1] = GIF_REG_AD;
	header.finish_data[0] = 1;
	header.finish_data[1] = GS_REG_FINISH;
}

/* Column major local to clip matrix for rotation Ry(ay) * Rx(ax), then a
 * translation by -CAMERA_DISTANCE along z, then the projection. The VU
 * computes col0*x + col1*y + col2*z + col3*w, so qword j holds column j. */
static void build_matrix(u32 frame)
{
	float sy = sin_table[frame % SIN_TABLE_SIZE];
	float cy = sin_table[(frame + 60) % SIN_TABLE_SIZE];
	float sx = sin_table[(frame * 2 + 30) % SIN_TABLE_SIZE];
	float cx = sin_table[(frame * 2 + 90) % SIN_TABLE_SIZE];
	float za = NEAR_PLANE / (FAR_PLANE - NEAR_PLANE);
	float zb = NEAR_PLANE * FAR_PLANE / (FAR_PLANE - NEAR_PLANE);
	float r[3][3];
	float m[4][4];
	int i, j;

	/* Ry * Rx */
	r[0][0] = cy;       r[0][1] = sy * sx;  r[0][2] = sy * cx;
	r[1][0] = 0.0f;     r[1][1] = cx;       r[1][2] = -sx;
	r[2][0] = -sy;      r[2][1] = cy * sx;  r[2][2] = cy * cx;

	for (j = 0; j < 3; j++) {
		m[0][j] = FOCAL_X * r[0][j];
		m[1][j] = FOCAL_Y * r[1][j];
		m[2][j] = za * r[2][j];
		m[3][j] = -r[2][j];
	}
	m[0][3] = 0.0f;
	m[1][3] = 0.0f;
	m[2][3] = za * -CAMERA_DISTANCE + zb;
	m[3][3] = CAMERA_DISTANCE;

	for (j = 0; j < 4; j++)
		for (i = 0; i < 4; i++)
			matrix[j * 4 + i] = m[i][j];
}

static void vu1_upload_program(void)
{
	u32 size = packet2_utils_get_packet_size_for_program(&VU1Cube_CodeStart, &VU1Cube_CodeEnd) + 1;
	packet2_t *packet = packet2_create(size, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(packet, 0, &VU1Cube_CodeStart, &VU1Cube_CodeEnd);
	packet2_utils_vu_add_end_tag(packet);
	dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(packet);
}

static void vu1_set_double_buffer(void)
{
	packet2_t *packet = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(packet, VU_BUFFER_BASE, VU_BUFFER_OFFSET);
	packet2_utils_vu_add_end_tag(packet);
	dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(packet);
}

static void clear_screen(packet_t *packet)
{
	qword_t *q = packet->data;
	q = fixture_gs_clear(q, 0x10, 0x18, 0x28);
	q = fixture_gs_ad(q, FIXTURE_GS_TEST_ZGEQUAL, GS_REG_TEST);
	q = draw_finish(q);
	dma_wait_fast();
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	draw_wait_finish();
}

static void draw_cube(packet2_t *packet)
{
	u32 header_qwords = sizeof(vu_header_t) / 16;
	packet2_reset(packet, 0);
	packet2_utils_vu_add_unpack_data(packet, 0, matrix, 4, 0);
	packet2_utils_vu_add_unpack_data(packet, 0, &header, header_qwords, 1);
	packet2_utils_vu_add_unpack_data(packet, header_qwords, vertices, VERTEX_COUNT, 1);
	packet2_utils_vu_add_unpack_data(packet, header_qwords + VERTEX_COUNT, colours, VERTEX_COUNT, 1);
	packet2_utils_vu_add_start_program(packet, 0);
	packet2_utils_vu_add_end_tag(packet);
	dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	/* The microprogram ends its XGKICK packet with a FINISH write. */
	draw_wait_finish();
}

int main(int argc, char *argv[])
{
	framebuffer_t frame;
	zbuffer_t z;
	packet_t *gif_packet;
	packet2_t *vif_packet;
	u32 n = 0;

	fixture_tty_init();
	fixture_tty_kv_str("HELLO", "vu1_cube");

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);

	vu1_upload_program();
	vu1_set_double_buffer();
	fixture_gs_init(&frame, &z);

	build_mesh();
	build_header();

	gif_packet = packet_init(16, PACKET_NORMAL);
	vif_packet = packet2_create(16, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);

	for (;;) {
		build_matrix(n);
		clear_screen(gif_packet);
		draw_cube(vif_packet);
		fixture_tty_kv_dec("FRAME", n);
		graph_wait_vsync();
		n++;
	}

	return 0;
}
