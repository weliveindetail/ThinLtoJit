#include <llvm/ExecutionEngine/Orc/OrcError.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/Bitcode/BitcodeReader.h>

#include "ThinLtoJit.h"

using namespace llvm;

static cl::list<std::string> InputFileNames(cl::Positional, cl::OneOrMore,
                                            cl::desc("<input bitcode files>"));

// Dump error message and exit.
LLVM_ATTRIBUTE_NORETURN static void fatalError(Error Err) {
  llvm::logAllUnhandledErrors(std::move(Err), outs(), "\n\nFatal Error\n");
  llvm::outs() << "\n\n";
  llvm::outs().flush();
  exit(1);
}

void copyFile(StringRef From, StringRef To) {
  llvm::sys::fs::remove(To);
  llvm::sys::fs::copy_file(From, To);
}

void dumpMem(llvm::Expected<int *> mem) {
  if (Error Err = mem.takeError())
    fatalError(std::move(Err));

  dbgs() << "\nmem = " << (uint64_t)*mem;
  dbgs().flush();
}

void dumpIntDists(llvm::Expected<int *> Res) {
  if (Error Err = Res.takeError())
    fatalError(std::move(Err));

  int *Arr = *Res;
  dbgs() << "\nres = " << (uint64_t)Arr << " (";
  dbgs() << Arr[0] << ", " << Arr[1] << ", " << Arr[2] << ")";
  dbgs().flush();
}

int main(int Argc, char **Argv) {
  sys::PrintStackTraceOnErrorSignal(Argv[0]);
  PrettyStackTraceProgram X(Argc, Argv);

  atexit(llvm_shutdown);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  // Parse -debug and -debug-only options.
  cl::ParseCommandLineOptions(Argc, Argv, "JitFromScratch example project\n");

  llvm::thinltojit::ThinLtoLazyOrcJitRuntime Runtime;
  Runtime.ErrorHandler = fatalError;

  // Ignore input files from the command line in this example.
  // for (const std::string &fileName : InputFileNames)
  //  Runtime.addBitcodeFile(fileName);

  // Prepare initial bitcode files.
  copyFile("example/01_integerDistances.s", "example/integerDistances.s");
  copyFile("example/02_customIntAllocator.s", "example/customIntAllocator.s");

  // Register the bitcode file to use. Corrupt or missing files should not cause
  // problems, as long as we don't miss any symbols.
  Runtime.addBitcodeFile("example/integerDistances.s");
  Runtime.addBitcodeFile("example/customIntAllocator.s");
  Runtime.addBitcodeFile("missingFileA");

  // Compile module with no external dependencies (except host-process symbols).
  // Only one module will be compiled.
  auto AllocFn1 = Runtime.compile<int *, int>("_Z18customIntAllocatorj");

  // Calling this function affects global state.
  dumpMem(AllocFn1.call(3));

  // Compile module that depends on previous module. The previous module
  // remains unchanged, so it won't be touched (we just provide its symbols).
  // Again only one module will be compiled.
  auto IntDistsFn1 =
      Runtime.compile<int *, int *, int *, int>("_Z16integerDistancesPiS_j");

  // Recompile the module with no changes (early exit).
  auto IntDistsFn2 =
      Runtime.compile<int *, int *, int *, int>("_Z16integerDistancesPiS_j");

  // Both function handles refer to the customIntAllocator function, which
  // remained unchanged. Global state was preserved.
  int x[]{0, 1, 2};
  int y[]{3, 1, -1};

  dumpMem(AllocFn1.call(3));
  dumpIntDists(IntDistsFn1.call(x, y, 3));
  dumpIntDists(IntDistsFn2.call(x, y, 3));

  // Add a printf call to customIntAllocator, to dump allocation index.
  copyFile("example/03_customIntAllocator.s", "example/customIntAllocator.s");

  // Recompile the integerDistances function. As it depends on the module of
  // customIntAllocator, which changed on disk, this will trigger recompilation.
  // Both modules will be compiled.
  auto IntDistsFn3 =
      Runtime.compile<int *, int *, int *, int>("_Z16integerDistancesPiS_j");

  // Calling all the functions again, they will automatically rebind to the
  // latest compiled version.
  dumpMem(AllocFn1.call(3));
  dumpIntDists(IntDistsFn1.call(x, y, 3));
  dumpIntDists(IntDistsFn2.call(x, y, 3));
  dumpIntDists(IntDistsFn3.call(x, y, 3));

  // Add another bitcode file that provides a global sink for the result
  // of intergerDistances function.
  copyFile("example/04_globalResults.s", "example/globalResults.s");
  Runtime.addBitcodeFile("example/globalResults.s");

  // For now this should not change anything (early exit).
  auto AllocFn2 = Runtime.compile<int *, int>("_Z18customIntAllocatorj");
  dumpMem(AllocFn2.call(3));

  // Let integerDistances write to the newly available global sink.
  copyFile("example/05_integerDistances.s", "example/integerDistances.s");

  // This change affects the function signature!
  auto IntDistsFn4 =
      Runtime.compile<void, int *, int *, int>("_Z16integerDistancesPiS_j");

  // Let integerDistances call out to dump the values of the global sink.
  copyFile("example/05_integerDistances.s", "example/integerDistances.s");

  // This change affects the function signature!
  auto IntDistsFn5 =
      Runtime.compile<void, int *, int *, int>("_Z16integerDistancesPiS_j");

  dbgs().flush();
  return 0;
}
