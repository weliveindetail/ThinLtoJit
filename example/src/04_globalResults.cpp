#include <cstdio>

int *results = 0;

void printResults() {
  if (results)
    printf("Results: %d, %d, %d\n", results[0], results[1], results[2]);
  else
    printf("Results not available");
}
