#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  printf("hello from aarch64 argc=%d\n", argc);
  for (int i = 0; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
  fprintf(stderr, "stderr line\n");
  return argc > 1 ? atoi(argv[1]) : 0;
}
