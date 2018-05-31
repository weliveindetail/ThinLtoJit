#include <cstdlib>

int *customIntAllocator(unsigned items);

int *integerDistances(int *x, int *y, unsigned items) {
  int *results = customIntAllocator(items);

  for (int i = 0; i < items; i++) {
    results[i] = abs(x[i] - y[i]);
  }

  return results;
}
