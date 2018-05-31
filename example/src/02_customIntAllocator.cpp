int *customIntAllocator(unsigned items) {
  static int memory[100];
  static unsigned allocIdx = 0;

  if (allocIdx + items < 100) {
    int *block = memory + allocIdx;
    allocIdx += items;
    return block;
  }
  return 0;
}