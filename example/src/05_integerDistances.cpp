#include <cstdlib>

extern int *results;

int *customIntAllocator(unsigned items);

void integerDistances(int *x, int *y, unsigned items) {
  results = customIntAllocator(items);

  for (int i = 0; i < items; i++) {
    results[i] = abs(x[i] - y[i]);
  }
}
