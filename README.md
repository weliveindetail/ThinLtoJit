# ThinLtoJit

ThinLTO Tools for Stateful Incremental JITing

## Build

Tested on Ubuntu 18.04 compiling with Clang 6.0, linking against libLLVM from LLVM 6.0 Release:
```
$ cd ThinLtoJit
$ mkdir build
$ cd build
$ cmake -GNinja -DLLVM_DIR=path/to/llvm60/build/lib/cmake/llvm ../src
$ ninja ThinLtoJit
$ cd ..
$ ./build_example.sh 
$ ./build/ThinLtoJit test
```


