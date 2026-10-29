/* Reference C version of bench/sum.nov. */
#include <stdio.h>
#include <stdlib.h>

static long sum(const long *xs, long n) {
  long s = 0;
  for (long i = 0; i < n; i++) s += xs[i];
  return s;
}

int main(void) {
  long n = 1000000;
  long *data = malloc(n * sizeof(long));
  for (long i = 0; i < n; i++) data[i] = i % 7;
  long total = 0;
  for (long r = 0; r < 2000; r++) {
    data[r % n] += 1;
    total += sum(data, n);
  }
  printf("%ld\n", total);
  return 0;
}
