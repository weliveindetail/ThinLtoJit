cp example/src/01_integerDistances.cpp example/src/integerDistances.cpp
clang -S -emit-llvm -O2 example/src/integerDistances.cpp -o example/01_integerDistances.ll
clang -flto=thin -c -O2 example/src/integerDistances.cpp -o example/integerDistances.s
mv example/integerDistances.s example/01_integerDistances.s

cp example/src/02_customIntAllocator.cpp example/src/customIntAllocator.cpp
clang -S -emit-llvm -O2 example/src/customIntAllocator.cpp -o example/02_customIntAllocator.ll
clang -flto=thin -c -O2 example/src/customIntAllocator.cpp -o example/customIntAllocator.s
mv example/customIntAllocator.s example/02_customIntAllocator.s

cp example/src/03_customIntAllocator.cpp example/src/customIntAllocator.cpp
clang -S -emit-llvm -O2 example/src/customIntAllocator.cpp -o example/03_customIntAllocator.ll
clang -flto=thin -c -O2 example/src/customIntAllocator.cpp -o example/customIntAllocator.s
mv example/customIntAllocator.s example/03_customIntAllocator.s

cp example/src/04_globalResults.cpp example/src/globalResults.cpp
clang -S -emit-llvm -O2 example/src/globalResults.cpp -o example/04_globalResults.ll
clang -flto=thin -c -O2 example/src/globalResults.cpp -o example/globalResults.s
mv example/globalResults.s example/04_globalResults.s

cp example/src/05_integerDistances.cpp example/src/integerDistances.cpp
clang -S -emit-llvm -O2 example/src/integerDistances.cpp -o example/05_integerDistances.ll
clang -flto=thin -c -O2 example/src/integerDistances.cpp -o example/integerDistances.s
mv example/integerDistances.s example/05_integerDistances.s

cp example/src/06_integerDistances.cpp example/src/integerDistances.cpp
clang -S -emit-llvm -O2 example/src/integerDistances.cpp -o example/06_integerDistances.ll
clang -flto=thin -c -O2 example/src/integerDistances.cpp -o example/integerDistances.s
mv example/integerDistances.s example/05_integerDistances.s

rm example/src/integerDistances.cpp
rm example/src/customIntAllocator.cpp
rm example/src/globalResults.cpp
