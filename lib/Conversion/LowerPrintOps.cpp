//===----------------------------------------------------------------------===//
//
// Copyright 2021-2022 The HCL-MLIR Authors.
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//
// LowerPrintOps.cpp defines a pass to lower PrintOp and PrintMemRefOp to
// MLIR's utility printing functions or C printf functions. It also handles
// Fixed-point values/memref casting to float.
// We define our own memref printing and value printing operations to support
// following cases:
// - Multiple values printed with format string.
// - Print memref. Note that memref printing doesn't support formating.
//===----------------------------------------------------------------------===//

#include "hcl/Conversion/Passes.h"
#include "hcl/Dialect/HeteroCLDialect.h"
#include "hcl/Dialect/HeteroCLOps.h"
#include "hcl/Dialect/HeteroCLTypes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

using namespace mlir;
using namespace hcl;

namespace mlir {
namespace hcl {

/// Helper functions to decalare C printf function and format string
/// Return a symbol reference to the printf function, inserting it into the
/// module if necessary.
static FlatSymbolRefAttr getOrInsertPrintf(OpBuilder &rewriter,
                                           ModuleOp module) {
  auto *context = module.getContext();
  if (module.lookupSymbol<LLVM::LLVMFuncOp>("printf"))
    return SymbolRefAttr::get(context, "printf");

  // Create a function declaration for printf, the signature is:
  //   * `i32 (i8*, ...)`
  auto llvmI32Ty = IntegerType::get(context, 32);
  auto llvmI8PtrTy = LLVM::LLVMPointerType::get(IntegerType::get(context, 8));
  auto llvmFnType = LLVM::LLVMFunctionType::get(llvmI32Ty, llvmI8PtrTy,
                                                /*isVarArg=*/true);

  // Insert the printf function into the body of the parent module.
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToStart(module.getBody());
  rewriter.create<LLVM::LLVMFuncOp>(module.getLoc(), "printf", llvmFnType);
  return SymbolRefAttr::get(context, "printf");
}

/// Return a value representing an access into a global string with the given
/// name, creating the string if necessary.
static Value getOrCreateGlobalString(Location loc, OpBuilder &builder,
                                     StringRef name, StringRef value,
                                     ModuleOp module) {
  // Create the global at the entry of the module.
  LLVM::GlobalOp global;
  if (!(global = module.lookupSymbol<LLVM::GlobalOp>(name))) {
    OpBuilder::InsertionGuard insertGuard(builder);
    builder.setInsertionPointToStart(module.getBody());
    auto type = LLVM::LLVMArrayType::get(
        IntegerType::get(builder.getContext(), 8), value.size());
    global = builder.create<LLVM::GlobalOp>(loc, type, /*isConstant=*/true,
                                            LLVM::Linkage::Internal, name,
                                            builder.getStringAttr(value),
                                            /*alignment=*/0);
  }

  // Get the pointer to the first character in the global string.
  Value globalPtr = builder.create<LLVM::AddressOfOp>(loc, global);
  Value cst0 = builder.create<LLVM::ConstantOp>(
      loc, IntegerType::get(builder.getContext(), 64),
      builder.getIntegerAttr(builder.getIndexType(), 0));
  return builder.create<LLVM::GEPOp>(
      loc,
      LLVM::LLVMPointerType::get(IntegerType::get(builder.getContext(), 8)),
      globalPtr, ArrayRef<Value>({cst0, cst0}));
}

void lowerPrintOpToPrintf(Operation *op) {
  OpBuilder builder(op);
  auto loc = op->getLoc();
  ModuleOp parentModule = op->getParentOfType<ModuleOp>();

  // If the PrintOp has string attribute, it is the format string
  std::string format_str = "%f \0";
  if (op->hasAttr("format")) {
    format_str = op->getAttr("format").cast<StringAttr>().getValue().str();
  }
  // Get a symbol reference to the printf function, inserting it if
  // necessary. Create global strings for format and new line
  auto printfRef = getOrInsertPrintf(builder, parentModule);
  Value formatSpecifierCst = getOrCreateGlobalString(
      loc, builder, "frmt_spec", StringRef(format_str), parentModule);

  // Create a call to printf with the format string and the values to print.
  SmallVector<Value, 4> operands;
  operands.push_back(formatSpecifierCst);
  for (auto value : op->getOperands()) {
    operands.push_back(value);
  }
  // builder.create<func::CallOp>(loc, printfRef, builder.getIntegerType(32),
  //                              operands);
}

void PrintOpLoweringDispatcher(func::FuncOp &funcOp) {
  SmallVector<Operation *, 4> printOps;
  funcOp.walk([&](Operation *op) {
    if (auto printOp = dyn_cast<PrintOp>(op)) {
      printOps.push_back(printOp);
    }
  });
  for (auto printOp : printOps) {
    lowerPrintOpToPrintf(printOp);
  }
  std::reverse(printOps.begin(), printOps.end());
  for (auto printOp : printOps) {
    printOp->erase();
  }
}

/// Pass entry point
bool applyLowerPrintOps(ModuleOp &module) {
  for (func::FuncOp func : module.getOps<func::FuncOp>()) {
    PrintOpLoweringDispatcher(func);
  }
  return true;
}
} // namespace hcl
} // namespace mlir

namespace {
struct HCLLowerPrintOpsTransformation
    : public LowerPrintOpsBase<HCLLowerPrintOpsTransformation> {
  void runOnOperation() override {
    auto module = getOperation();
    if (!applyLowerPrintOps(module)) {
      return signalPassFailure();
    }
  }
};
} // namespace

namespace mlir {
namespace hcl {

std::unique_ptr<OperationPass<ModuleOp>> createLowerPrintOpsPass() {
  return std::make_unique<HCLLowerPrintOpsTransformation>();
}
} // namespace hcl
} // namespace mlir