/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <string>
#include <utility>

#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Transforms/RegionUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"

namespace mlir {
namespace TFDevice {

namespace {

constexpr char kXlaOutsideCompilationAttr[] = "_xla_outside_compilation";
constexpr char kAllowSoftPlacementAttr[] = "allow_soft_placement";

// This pass marks unsupported ops in a device cluster with
// `_xla_outside_compilation` attribute so the operations will run on the host
// instead of the device.  Unsupported ops are ops that can not be code
// generated to run on the device for the cluster.
struct MarkOpsForOutsideCompilation
    : public PassWrapper<MarkOpsForOutsideCompilation,
                         OperationPass<ModuleOp>> {
  void runOnOperation() override;
};

// TODO(b/159128666): Check the control flow legalization passes instead once
// added.
void AddSupportedControlFlowOps(MLIRContext* context,
                                llvm::DenseSet<OperationName>* supported_ops) {
  supported_ops->insert(
      OperationName(TF::IfRegionOp::getOperationName(), context));
  supported_ops->insert(
      OperationName(TF::WhileRegionOp::getOperationName(), context));
  supported_ops->insert(
      OperationName(TF::YieldOp::getOperationName(), context));
}

// These embedding ops are rewritten when running TPUCompileOp.
void AddRewrittenEmbeddingOps(MLIRContext* context,
                              llvm::DenseSet<OperationName>* supported_ops) {
  supported_ops->insert(OperationName(
      TF::RecvTPUEmbeddingActivationsOp::getOperationName(), context));
  supported_ops->insert(OperationName(
      TF::SendTPUEmbeddingGradientsOp::getOperationName(), context));
}

bool HasStringOperand(Operation& op) {
  for (auto operand : op.getOperands()) {
    if (getElementTypeOrSelf(operand).isa<TF::StringType>()) return true;
  }
  return false;
}

bool HasStringResult(Operation& op) {
  for (auto result : op.getResults()) {
    if (getElementTypeOrSelf(result).isa<TF::StringType>()) return true;
  }
  return false;
}

bool MatchesPattern(Operation& op,
                    const llvm::DenseSet<OperationName>& supported_ops) {
  return (supported_ops.contains(op.getName()));
}

// Checks if the op is supported inside of a device cluster.  Ops not
// in `tf_dialect` are considered supported.
bool IsSupportedOp(Operation& op,
                   const llvm::DenseSet<OperationName>& supported_ops,
                   const Dialect* tf_dialect) {
  if (op.getDialect() != tf_dialect)
    return true;
  else
    return !HasStringOperand(op) && !HasStringResult(op) &&
           (MatchesPattern(op, supported_ops) ||
            mhlo::IsOpAllowedTf2XlaFallback(&op));
}

// Checks all regions of `op` for captured string operands.
bool HasCapturedStringOperand(Operation* op) {
  bool string_operand = false;
  for (auto& region : op->getRegions()) {
    mlir::visitUsedValuesDefinedAbove(
        region, region, [&](mlir::OpOperand* operand) {
          if (getElementTypeOrSelf(operand->get()).isa<TF::StringType>())
            string_operand = true;
        });
    if (string_operand) return string_operand;
  }
  return string_operand;
}

// Marks uncompilable ops that are in `tf_dialect` for outside compilation.
LogicalResult MarkUncompilableOps(
    const Dialect* tf_dialect, Block* block,
    llvm::DenseSet<OperationName>& supported_ops) {
  block->walk([&](Operation* op) {
    if (!IsSupportedOp(*op, supported_ops, tf_dialect)) {
      op->setAttr(kXlaOutsideCompilationAttr,
                  StringAttr::get("auto", op->getContext()));
    }
    if (llvm::isa<TF::IfRegionOp, TF::WhileRegionOp>(op)) {
      if (HasCapturedStringOperand(op)) {
        op->setAttr(kXlaOutsideCompilationAttr,
                    StringAttr::get("auto", op->getContext()));
      }
    }
  });
  return success();
}

// Unmarks outside compilation for any op that has parents already
// marked for outside compilation since the child will be extracted
// anyways.
void UnmarkChildren(Block* block) {
  block->walk([&](Operation* op) {
    if (!op->getAttrOfType<StringAttr>(kXlaOutsideCompilationAttr)) return;
    Operation* iter_op = op;
    bool remove_attr = false;
    while (auto* parent_op = iter_op->getParentOp()) {
      if (parent_op->getAttrOfType<StringAttr>(kXlaOutsideCompilationAttr)) {
        remove_attr = true;
        break;
      }
      iter_op = parent_op;
    }
    if (remove_attr) op->removeAttr(kXlaOutsideCompilationAttr);
  });
}

void MarkOpsForOutsideCompilation::runOnOperation() {
  auto module = getOperation();
  const Dialect* tf_dialect = getContext().getLoadedDialect("tf");
  if (!tf_dialect) {
    getOperation().emitError() << "'tf' dialect is not registered";
    return signalPassFailure();
  }
  OwningRewritePatternList patterns;
  mhlo::PopulateLegalizeTfPatterns(module.getContext(), &patterns);

  // `supported_ops` contains the name of all of the ops that can potentially be
  // lowered into HLO on the device. This doesn't always mean that the op can
  // be lowered in the future passes but if the op is not in this set, it can't
  // be lowered in a subsequent pass.
  llvm::DenseSet<OperationName> supported_ops;
  for (auto& pattern : patterns) {
    supported_ops.insert(*pattern->getRootKind());
  }
  AddSupportedControlFlowOps(module.getContext(), &supported_ops);
  AddRewrittenEmbeddingOps(module.getContext(), &supported_ops);

  auto result = module.walk([&](tf_device::ClusterOp cluster) {
    // Only if `allow_soft_placement` attribute is true should we mark ops
    // for outside compilation.
    auto soft_placement_attr =
        cluster.getAttrOfType<BoolAttr>(kAllowSoftPlacementAttr);
    if (!(soft_placement_attr && soft_placement_attr.getValue())) {
      return WalkResult::advance();
    }
    if (failed(
            MarkUncompilableOps(tf_dialect, &cluster.GetBody(), supported_ops)))
      return WalkResult::interrupt();

    return WalkResult::advance();
  });

  if (result.wasInterrupted()) return signalPassFailure();

  module.walk([&](tf_device::ClusterOp cluster) {
    // Only if `allow_soft_placement` attribute is true should we unmark ops
    // for outside compilation.
    auto soft_placement_attr =
        cluster.getAttrOfType<BoolAttr>(kAllowSoftPlacementAttr);
    if (!(soft_placement_attr && soft_placement_attr.getValue())) {
      return;
    }
    UnmarkChildren(&cluster.GetBody());
  });
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>>
CreateMarkOpsForOutsideCompilationPass() {
  return std::make_unique<MarkOpsForOutsideCompilation>();
}

static PassRegistration<MarkOpsForOutsideCompilation> pass(
    "tf-mark-ops-for-outside-compilation",
    "Marks unsupported ops a device cluster for outside compilation.");

}  // namespace TFDevice
}  // namespace mlir
