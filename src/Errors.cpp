#include "Errors.h"

namespace llvm::thinltojit {

char MissingModuleError::ID;
char CantCompileFunctionError::ID;
char CrossImportError::ID;
char InvocationFailedError::ID;

} // namespace llvm::thinltojit
