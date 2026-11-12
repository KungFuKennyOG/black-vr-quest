#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static int counter; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *a) { for (int i = 0; i < 100000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); } return a; }
int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/arm64run_io_test.txt";
  FILE *f = fopen(path, "w"); if (!f) return 1; fprintf(f, "line1\nline2\n"); fclose(f);
  int fd = open(path, O_RDONLY); struct stat st; fstat(fd, &st);
  char buf[64] = {0}; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd); unlink(path);
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned char rnd[16]; ssize_t r = getrandom(rnd, sizeof rnd, 0);
  void *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memset(m, 1, 1 << 20); mprotect(m, 4096, PROT_READ); munmap(m, 1 << 20);
  pthread_t t[4]; for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, worker, 0);
  for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
  int ok = n == 12 && st.st_size == 12 && !strcmp(buf, "line1\nline2\n") && r == 16 && counter == 400000 && ts.tv_sec + ts.tv_nsec > 0;
  printf("io n=%zd size=%lld rnd=%zd threads counter=%d pagesize=%ld -> %s\n", n, (long long)st.st_size, r, counter, sysconf(_SC_PAGESIZE), ok ? "PASS" : "FAIL");
  return !ok;
}
