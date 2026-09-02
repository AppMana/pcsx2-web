#ifndef FIXTURE_GS_H
#define FIXTURE_GS_H

#include <tamtypes.h>
#include <draw.h>
#include <graph.h>

/* All GS fixtures share one display: NTSC, interlaced, FIELD mode, a 640x448
 * PSMCT32 frame buffer and a 640x448 32 bit Z buffer, primitive origin at
 * (2048-320, 2048-224) so screen pixel (x, y) is GS coordinate
 * FIXTURE_GS_X(x), FIXTURE_GS_Y(y) in 12.4 fixed point. */
#define FIXTURE_GS_WIDTH  640
#define FIXTURE_GS_HEIGHT 448
#define FIXTURE_GS_ORIGIN_X (2048 - FIXTURE_GS_WIDTH / 2)
#define FIXTURE_GS_ORIGIN_Y (2048 - FIXTURE_GS_HEIGHT / 2)
#define FIXTURE_GS_X(x) (((FIXTURE_GS_ORIGIN_X + (x)) << 4))
#define FIXTURE_GS_Y(y) (((FIXTURE_GS_ORIGIN_Y + (y)) << 4))

/* Z test register values used by the fixtures. */
#define FIXTURE_GS_TEST_ZALLPASS GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_ALLPASS)
#define FIXTURE_GS_TEST_ZGEQUAL  GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_GREATER_EQUAL)

/* Allocates VRAM, programs the CRTC and sends the drawing environment. */
void fixture_gs_init(framebuffer_t *frame, zbuffer_t *z);

/* Appends TEST (Z always pass) plus one flat sprite covering the whole screen
 * at Z=0, which clears both the frame buffer and the Z buffer. */
qword_t *fixture_gs_clear(qword_t *q, int r, int g, int b);

/* Appends a GIF tag carrying one A+D register write. */
qword_t *fixture_gs_ad(qword_t *q, u64 value, u64 reg);

#endif
