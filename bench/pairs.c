/* Reference C version of bench/pairs.nov. */
#include <stdio.h>
#include <stdlib.h>

static long collatz(long n) {
  long steps = 0;
  while (n != 1) { n = n % 2 == 0 ? n / 2 : 3 * n + 1; steps++; }
  return steps;
}

static long pairs(const long *xs, long n) {
  long s = 0;
  for (long i = 0; i < n; i++)
    for (long j = i + 1; j < n; j++) s += xs[i] * xs[j];
  return s;
}

int main(void) {
  long n = collatz(27) * 600;
  long *data = malloc(n * sizeof(long));
  for (long i = 0; i < n; i++) data[i] = i % 13;
  printf("%ld\n", pairs(data, n));
  return 0;
}
