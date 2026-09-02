/* hello_tty: deterministic EE console output for the pcsx2-web oracle.
 *
 * Every line is KEY=VALUE. Floating point results are printed as the raw
 * IEEE single bit pattern so that the rounding mode, denormal handling and
 * overflow clamping of the emulated FPU and VU0 show up byte for byte.
 * Operands were chosen so that round to nearest and chop to zero give
 * different bit patterns; see README.md for the table. */

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <packet.h>
#include <gs_psm.h>
#include "fixture_tty.h"
#include "fixture_gs.h"

typedef union {
	float f;
	u32 u;
} fbits_t;

typedef struct {
	u32 v[4];
} __attribute__((aligned(16))) vec_t;

#define FLOAT_ONE       0x3f800000u
#define FLOAT_HALF      0x3f000000u
#define FLOAT_TWO       0x40000000u
#define FLOAT_THREE     0x40400000u
#define FLOAT_FOUR      0x40800000u
#define FLOAT_SEVEN     0x40e00000u
#define FLOAT_TEN       0x41200000u
#define FLOAT_1P23      0x3f9d70a4u
#define FLOAT_1P4       0x3fb33333u
#define FLOAT_1P5_ULP   0x3fc00001u
#define FLOAT_3P5       0x40600000u
#define FLOAT_M3P5      0xc0600000u
#define FLOAT_2P7       0x402ccccdu
#define FLOAT_1E10      0x501502f9u
#define FLOAT_M1E10     0xd01502f9u
#define FLOAT_123456P789 0x47f12065u
#define FLOAT_MAX       0x7f7fffffu
#define FLOAT_MMAX      0xff7fffffu
#define FLOAT_ADD_TINY  0x33c00000u /* 3 * 2^-25, 1.5 ulp of 1.0 */
#define FLOAT_ADD_TIE   0x33800000u /* 2^-24, exactly half an ulp of 1.0 */
#define FLOAT_SUB_TINY  0x33a00000u /* 5 * 2^-26, 1.25 ulp below 1.0 */
#define FLOAT_MIN_NORM  0x00800000u /* 2^-126 */
#define FLOAT_MIN_NORM1 0x00800001u
#define FLOAT_DENORM_H  0x00400000u /* 2^-127, a denormal */
#define FLOAT_ZERO      0x00000000u
#define FLOAT_MZERO     0x80000000u
#define FLOAT_M1P5      0xbfc00000u
#define FLOAT_M2        0xc0000000u
#define FLOAT_M4        0xc0800000u
#define INT_2P24_3      0x01000003u /* 2^24 + 3, not representable as float */

static float ld(u32 bits)
{
	fbits_t b;
	b.u = bits;
	return b.f;
}

static u32 st(float f)
{
	fbits_t b;
	b.f = f;
	return b.u;
}

#define FPU_BINOP(name, insn)                                                   \
	static float name(float a, float b)                                        \
	{                                                                          \
		float r;                                                               \
		__asm__ volatile(insn " %0, %1, %2" : "=f"(r) : "f"(a), "f"(b));       \
		return r;                                                              \
	}

#define FPU_UNOP(name, insn)                                                    \
	static float name(float a)                                                 \
	{                                                                          \
		float r;                                                               \
		__asm__ volatile(insn " %0, %1" : "=f"(r) : "f"(a));                   \
		return r;                                                              \
	}

FPU_BINOP(fpu_add, "add.s")
FPU_BINOP(fpu_sub, "sub.s")
FPU_BINOP(fpu_mul, "mul.s")
FPU_BINOP(fpu_div, "div.s")
FPU_BINOP(fpu_rsqrt, "rsqrt.s")
FPU_BINOP(fpu_max, "max.s")
FPU_BINOP(fpu_min, "min.s")
FPU_UNOP(fpu_sqrt, "sqrt.s")
FPU_UNOP(fpu_abs, "abs.s")
FPU_UNOP(fpu_neg, "neg.s")

/* madd.s/msub.s use the accumulator: adda.s loads it, then fd = ACC +/- fs*ft. */
static float fpu_madd(float acc, float a, float b)
{
	float r;
	__asm__ volatile("adda.s %1, %2\n\tmadd.s %0, %3, %4"
	                 : "=f"(r)
	                 : "f"(acc), "f"(ld(FLOAT_ZERO)), "f"(a), "f"(b));
	return r;
}

static float fpu_msub(float acc, float a, float b)
{
	float r;
	__asm__ volatile("adda.s %1, %2\n\tmsub.s %0, %3, %4"
	                 : "=f"(r)
	                 : "f"(acc), "f"(ld(FLOAT_ZERO)), "f"(a), "f"(b));
	return r;
}

static u32 fpu_cvt_w_s(float a)
{
	float r;
	u32 o;
	__asm__ volatile("cvt.w.s %0, %1" : "=f"(r) : "f"(a));
	__asm__ volatile("mfc1 %0, %1" : "=r"(o) : "f"(r));
	return o;
}

static float fpu_cvt_s_w(u32 a)
{
	float r;
	__asm__ volatile("mtc1 %1, %0\n\tcvt.s.w %0, %0" : "=f"(r) : "r"(a));
	return r;
}

static void kv_float(const char *key, float f)
{
	fixture_tty_kv_hex(key, st(f));
}

static void fpu_cases(void)
{
	/* Rounding: nearest and chop differ. */
	kv_float("FPU_ADD_1P5ULP", fpu_add(ld(FLOAT_ONE), ld(FLOAT_ADD_TINY)));
	kv_float("FPU_ADD_TIE", fpu_add(ld(FLOAT_ONE), ld(FLOAT_ADD_TIE)));
	kv_float("FPU_SUB_1P25ULP", fpu_sub(ld(FLOAT_ONE), ld(FLOAT_SUB_TINY)));
	kv_float("FPU_MUL_1P5ULP_SQ", fpu_mul(ld(FLOAT_1P5_ULP), ld(FLOAT_1P5_ULP)));
	kv_float("FPU_MUL_1P23_1P4", fpu_mul(ld(FLOAT_1P23), ld(FLOAT_1P4)));
	kv_float("FPU_DIV_1_3", fpu_div(ld(FLOAT_ONE), ld(FLOAT_THREE)));
	kv_float("FPU_DIV_2_3", fpu_div(ld(FLOAT_TWO), ld(FLOAT_THREE)));
	kv_float("FPU_DIV_10_7", fpu_div(ld(FLOAT_TEN), ld(FLOAT_SEVEN)));
	kv_float("FPU_SQRT_2", fpu_sqrt(ld(FLOAT_TWO)));
	kv_float("FPU_SQRT_5", fpu_sqrt(ld(0x40a00000u)));
	kv_float("FPU_SQRT_123456P789", fpu_sqrt(ld(FLOAT_123456P789)));
	kv_float("FPU_RSQRT_1_2", fpu_rsqrt(ld(FLOAT_ONE), ld(FLOAT_TWO)));
	kv_float("FPU_MADD_1_1P23_1P4", fpu_madd(ld(FLOAT_ONE), ld(FLOAT_1P23), ld(FLOAT_1P4)));
	kv_float("FPU_MSUB_1_1P23_1P4", fpu_msub(ld(FLOAT_ONE), ld(FLOAT_1P23), ld(FLOAT_1P4)));

	/* Denormal inputs (DAZ) and denormal results (FTZ). */
	kv_float("FPU_DAZ_ADD", fpu_add(ld(FLOAT_DENORM_H), ld(FLOAT_DENORM_H)));
	kv_float("FPU_DAZ_MUL", fpu_mul(ld(FLOAT_DENORM_H), ld(FLOAT_TWO)));
	kv_float("FPU_DAZ_MAX", fpu_max(ld(FLOAT_DENORM_H), ld(FLOAT_ZERO)));
	kv_float("FPU_FTZ_MUL", fpu_mul(ld(FLOAT_MIN_NORM), ld(FLOAT_HALF)));
	kv_float("FPU_FTZ_DIV", fpu_div(ld(FLOAT_MIN_NORM), ld(FLOAT_FOUR)));
	kv_float("FPU_FTZ_SUB", fpu_sub(ld(FLOAT_MIN_NORM1), ld(FLOAT_MIN_NORM)));

	/* Overflow and division by zero: the EE clamps instead of producing Inf/NaN. */
	kv_float("FPU_OVF_MUL", fpu_mul(ld(FLOAT_MAX), ld(FLOAT_TWO)));
	kv_float("FPU_OVF_ADD", fpu_add(ld(FLOAT_MAX), ld(FLOAT_MAX)));
	kv_float("FPU_OVF_NEG_MUL", fpu_mul(ld(FLOAT_MMAX), ld(FLOAT_TWO)));
	kv_float("FPU_DIV_1_0", fpu_div(ld(FLOAT_ONE), ld(FLOAT_ZERO)));
	kv_float("FPU_DIV_M1_0", fpu_div(ld(0xbf800000u), ld(FLOAT_ZERO)));
	kv_float("FPU_DIV_0_0", fpu_div(ld(FLOAT_ZERO), ld(FLOAT_ZERO)));
	kv_float("FPU_SQRT_M4", fpu_sqrt(ld(FLOAT_M4)));
	kv_float("FPU_RSQRT_1_0", fpu_rsqrt(ld(FLOAT_ONE), ld(FLOAT_ZERO)));

	/* Conversions. */
	fixture_tty_kv_hex("FPU_CVTW_3P5", fpu_cvt_w_s(ld(FLOAT_3P5)));
	fixture_tty_kv_hex("FPU_CVTW_M3P5", fpu_cvt_w_s(ld(FLOAT_M3P5)));
	fixture_tty_kv_hex("FPU_CVTW_2P7", fpu_cvt_w_s(ld(FLOAT_2P7)));
	fixture_tty_kv_hex("FPU_CVTW_1E10", fpu_cvt_w_s(ld(FLOAT_1E10)));
	fixture_tty_kv_hex("FPU_CVTW_M1E10", fpu_cvt_w_s(ld(FLOAT_M1E10)));
	kv_float("FPU_CVTS_INT_MAX", fpu_cvt_s_w(0x7fffffffu));
	kv_float("FPU_CVTS_2P24_3", fpu_cvt_s_w(INT_2P24_3));
	kv_float("FPU_CVTS_M1", fpu_cvt_s_w(0xffffffffu));

	/* Sign handling. */
	kv_float("FPU_MAX_1P5_M2", fpu_max(ld(0x3fc00000u), ld(FLOAT_M2)));
	kv_float("FPU_MIN_1P5_M2", fpu_min(ld(0x3fc00000u), ld(FLOAT_M2)));
	kv_float("FPU_MAX_MZERO_ZERO", fpu_max(ld(FLOAT_MZERO), ld(FLOAT_ZERO)));
	kv_float("FPU_ABS_M1P5", fpu_abs(ld(FLOAT_M1P5)));
	kv_float("FPU_NEG_ZERO", fpu_neg(ld(FLOAT_ZERO)));
}

/* VU0 macro mode. Each lane of a vector carries one test case, so one
 * instruction prints four results. Q and ACC are exercised explicitly. */
static void vu0_run(const vec_t *a, const vec_t *b, const vec_t *c, const vec_t *d,
                    const vec_t *e, const vec_t *ints, vec_t *out)
{
	__asm__ volatile(
		"lqc2 $vf1, 0(%0)\n\t"
		"lqc2 $vf2, 0(%1)\n\t"
		"lqc2 $vf3, 0(%2)\n\t"
		"lqc2 $vf4, 0(%3)\n\t"
		"lqc2 $vf5, 0(%4)\n\t"
		"lqc2 $vf6, 0(%5)\n\t"
		/* out[0..3]: add, sub, mul, max */
		"vadd.xyzw $vf10, $vf1, $vf2\n\t"
		"vsub.xyzw $vf11, $vf1, $vf2\n\t"
		"vmul.xyzw $vf12, $vf1, $vf2\n\t"
		"vmax.xyzw $vf13, $vf1, $vf3\n\t"
		"sqc2 $vf10, 0(%6)\n\t"
		"sqc2 $vf11, 16(%6)\n\t"
		"sqc2 $vf12, 32(%6)\n\t"
		"sqc2 $vf13, 48(%6)\n\t"
		/* out[4]: mini, out[5]: overflow mul (c * d), out[6]: madd = c*d + a*b */
		"vmini.xyzw $vf14, $vf1, $vf3\n\t"
		"vmul.xyzw $vf15, $vf3, $vf4\n\t"
		"vmula.xyzw $ACC, $vf3, $vf4\n\t"
		"vmadd.xyzw $vf16, $vf1, $vf2\n\t"
		"sqc2 $vf14, 64(%6)\n\t"
		"sqc2 $vf15, 80(%6)\n\t"
		"sqc2 $vf16, 96(%6)\n\t"
		/* out[7]: Q from vdiv c.x / d.x (1/3), broadcast with vmulq on vf0 (0,0,0,1) */
		"vdiv $Q, $vf3x, $vf4x\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 112(%6)\n\t"
		/* out[8]: c.y / d.y (2/3) */
		"vdiv $Q, $vf3y, $vf4y\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 128(%6)\n\t"
		/* out[9]: c.z / d.z (10/7) */
		"vdiv $Q, $vf3z, $vf4z\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 144(%6)\n\t"
		/* out[10]: c.w / d.w (1/0) */
		"vdiv $Q, $vf3w, $vf4w\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 160(%6)\n\t"
		/* out[11]: sqrt(e.x), out[12]: sqrt(e.y), out[13]: sqrt(e.z) (negative input) */
		"vsqrt $Q, $vf5x\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 176(%6)\n\t"
		"vsqrt $Q, $vf5y\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 192(%6)\n\t"
		"vsqrt $Q, $vf5z\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 208(%6)\n\t"
		/* out[14]: rsqrt c.x / sqrt(e.w) = 1/sqrt(2) */
		"vrsqrt $Q, $vf3x, $vf5w\n\t"
		"vwaitq\n\t"
		"vmulq.xyzw $vf17, $vf0, $Q\n\t"
		"sqc2 $vf17, 224(%6)\n\t"
		/* out[15]: ftoi0 of e (float to int), out[16]: itof0 of ints */
		"vftoi0.xyzw $vf18, $vf5\n\t"
		"vitof0.xyzw $vf19, $vf6\n\t"
		"sqc2 $vf18, 240(%6)\n\t"
		"sqc2 $vf19, 256(%6)\n\t"
		/* out[17]: outer product ints x c using opmula/opmsub on the xyz lanes */
		"vopmula.xyz $ACC, $vf19, $vf3\n\t"
		"vopmsub.xyz $vf20, $vf3, $vf19\n\t"
		"sqc2 $vf20, 272(%6)\n\t"
		/* out[18]: broadcast multiply add: ACC = a * b.x; r = ACC + c * d.y */
		"vmulax.xyzw $ACC, $vf1, $vf2\n\t"
		"vmaddy.xyzw $vf21, $vf3, $vf4\n\t"
		"sqc2 $vf21, 288(%6)\n\t"
		:
		: "r"(a), "r"(b), "r"(c), "r"(d), "r"(e), "r"(ints), "r"(out)
		: "memory");
}

static void kv_vec(const char *key, const vec_t *v)
{
	char name[48];
	int i = 0;
	static const char lanes[4] = {'X', 'Y', 'Z', 'W'};
	int lane;
	while (key[i]) {
		name[i] = key[i];
		i++;
	}
	name[i] = '_';
	name[i + 2] = '\0';
	for (lane = 0; lane < 4; lane++) {
		name[i + 1] = lanes[lane];
		fixture_tty_kv_hex(name, v->v[lane]);
	}
}

static void vu0_cases(void)
{
	static const vec_t a = {{FLOAT_ONE, FLOAT_1P5_ULP, FLOAT_1P23, FLOAT_DENORM_H}};
	static const vec_t b = {{FLOAT_ADD_TINY, FLOAT_1P5_ULP, FLOAT_1P4, FLOAT_DENORM_H}};
	static const vec_t c = {{FLOAT_ONE, FLOAT_TWO, FLOAT_TEN, FLOAT_ONE}};
	static const vec_t d = {{FLOAT_THREE, FLOAT_THREE, FLOAT_SEVEN, FLOAT_ZERO}};
	static const vec_t e = {{0x40a00000u, FLOAT_123456P789, FLOAT_M4, FLOAT_TWO}};
	static const vec_t ints = {{0x7fffffffu, INT_2P24_3, 0xffffffffu, 0x00000003u}};
	static const vec_t ovf_c = {{FLOAT_MAX, FLOAT_MAX, FLOAT_MMAX, FLOAT_MIN_NORM}};
	static const vec_t ovf_d = {{FLOAT_TWO, FLOAT_MAX, FLOAT_TWO, FLOAT_HALF}};
	static vec_t out[19];
	static vec_t out_ovf[19];

	vu0_run(&a, &b, &c, &d, &e, &ints, out);
	kv_vec("VU0_ADD", &out[0]);
	kv_vec("VU0_SUB", &out[1]);
	kv_vec("VU0_MUL", &out[2]);
	kv_vec("VU0_MAX", &out[3]);
	kv_vec("VU0_MINI", &out[4]);
	kv_vec("VU0_MADD", &out[6]);
	fixture_tty_kv_hex("VU0_DIV_1_3", out[7].v[3]);
	fixture_tty_kv_hex("VU0_DIV_2_3", out[8].v[3]);
	fixture_tty_kv_hex("VU0_DIV_10_7", out[9].v[3]);
	fixture_tty_kv_hex("VU0_DIV_1_0", out[10].v[3]);
	fixture_tty_kv_hex("VU0_SQRT_5", out[11].v[3]);
	fixture_tty_kv_hex("VU0_SQRT_123456P789", out[12].v[3]);
	fixture_tty_kv_hex("VU0_SQRT_M4", out[13].v[3]);
	fixture_tty_kv_hex("VU0_RSQRT_1_2", out[14].v[3]);
	kv_vec("VU0_FTOI0", &out[15]);
	kv_vec("VU0_ITOF0", &out[16]);
	kv_vec("VU0_OPMSUB", &out[17]);
	kv_vec("VU0_MADDY", &out[18]);

	/* Same program with overflow and underflow operands in c and d. */
	vu0_run(&a, &b, &ovf_c, &ovf_d, &e, &ints, out_ovf);
	kv_vec("VU0_OVF_MUL", &out_ovf[5]);
	kv_vec("VU0_OVF_MADD", &out_ovf[6]);
	fixture_tty_kv_hex("VU0_DIV_MAX_2", out_ovf[7].v[3]);
	fixture_tty_kv_hex("VU0_DIV_MAX_MAX", out_ovf[8].v[3]);
}

static void draw_background(framebuffer_t *frame)
{
	packet_t *packet = packet_init(16, PACKET_NORMAL);
	qword_t *q = packet->data;
	q = fixture_gs_clear(q, 0x10, 0x20, 0x40);
	q = draw_finish(q);
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	draw_wait_finish();
	packet_free(packet);
}

int main(int argc, char *argv[])
{
	framebuffer_t frame;
	zbuffer_t z;
	u32 n;

	fixture_tty_init();
	fixture_tty_kv_str("HELLO", "hello_tty");

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	fixture_gs_init(&frame, &z);
	draw_background(&frame);

	fpu_cases();
	vu0_cases();

	for (n = 0; n < 120; n++) {
		graph_wait_vsync();
		fixture_tty_kv_dec("FRAME", n);
	}
	fixture_tty_line("DONE");

	for (;;)
		graph_wait_vsync();

	return 0;
}
