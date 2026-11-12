#include <arm_neon.h>
#include <math.h>
#include <stdio.h>
int main(void) {
  float a[8] = {1,2,3,4,5,6,7,8}, b[8] = {.5f,.5f,.5f,.5f,2,2,2,2}, r[8];
  for (int i = 0; i < 8; i += 4)
    vst1q_f32(r + i, vfmaq_f32(vld1q_f32(a + i), vld1q_f32(a + i), vld1q_f32(b + i)));
  float exp[8] = {1.5f,3,4.5f,6,15,18,21,24};
  int bad = 0;
  for (int i = 0; i < 8; i++) if (r[i] != exp[i]) bad++;
  uint8x16_t v = vdupq_n_u8(3); uint8x16_t w = vmulq_u8(v, v);
  if (vaddvq_u8(w) != (uint8_t)(9 * 16)) bad++;
  double d = sqrt(2.0) * sin(1.0) + exp2(0.5);
  printf("neon r=%g..%g d=%.12f bad=%d\n", r[0], r[7], d, bad);
  if (fabs(d - (1.4142135623730951 * 0.8414709848078965 + 1.4142135623730951)) > 1e-12) bad++;
  return bad;
}
