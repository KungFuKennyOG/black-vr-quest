#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int main(void) {
  enum { N = 20000 };
  static void *p[N]; static size_t sz[N];
  uint32_t s = 12345; uint64_t sum = 0;
  for (int round = 0; round < 20; round++)
    for (int i = 0; i < N; i++) {
      s = s * 1103515245u + 12345u;
      if (p[i]) { if (((unsigned char*)p[i])[sz[i]-1] != (unsigned char)i) { printf("corrupt\n"); return 2; } free(p[i]); }
      sz[i] = 1 + (s >> 8) % ((s & 0x100) ? 20000 : 512);
      p[i] = malloc(sz[i]); if (!p[i]) { printf("oom\n"); return 3; }
      memset(p[i], (unsigned char)i, sz[i]); sum += sz[i];
    }
  for (int i = 0; i < N; i++) free(p[i]);
  void *big = calloc(64, 1 << 20); if (!big) return 4; free(big);
  char *r = NULL; for (int i = 1; i < 2000; i++) { r = realloc(r, i * 997); r[i*997-1] = 1; } free(r);
  printf("malloc ok total=%llu\n", (unsigned long long)sum);
  return 0;
}
