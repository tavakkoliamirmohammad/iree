// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "CommonExtensions.h"

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree-dialects/Dialect/LinalgTransform/SimplePatternRewriter.h"
#include "iree-dialects/Dialect/LinalgTransform/StructuredTransformOpsExt.h"
#include "iree-dialects/Transforms/ListenerCSE.h"
#include "iree-dialects/Transforms/TransformMatchers.h"
#include "iree/compiler/Codegen/Common/Passes.h"
#include "iree/compiler/Codegen/Common/Transforms.h"
#include "iree/compiler/Codegen/Interfaces/BufferizationInterfaces.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/GPUUtils.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "mlir/Conversion/VectorToGPU/VectorToGPU.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/Transforms.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/TransformOps/LinalgTransformOps.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Utils/IndexingUtils.h"
#include "mlir/Dialect/MemRef/Transforms/Transforms.h"
#include "mlir/Dialect/SCF/Transforms/Transforms.h"
#include "mlir/Dialect/SCF/Utils/Utils.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Transforms/Transforms.h"
#include "mlir/Dialect/Transform/IR/TransformInterfaces.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/Dialect/Vector/Transforms/LoweringPatterns.h"
#include "mlir/Dialect/Vector/Transforms/Passes.h"
#include "mlir/Dialect/Vector/Transforms/VectorRewritePatterns.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/LoopInvariantCodeMotionUtils.h"
#include "iree/compiler/Codegen/Utils/MarkerUtils.h"

using namespace mlir;
using namespace mlir::iree_compiler;
using namespace mlir::iree_compiler::IREE;

iree_compiler::IREE::transform_dialect::CommonExtensions::CommonExtensions() {
  registerTransformOps<
#define GET_OP_LIST
#include "iree/compiler/Codegen/Common/TransformExtensions/CommonExtensionsOps.cpp.inc"
      >();
}

void mlir::iree_compiler::registerTransformDialectCommonExtension(
    DialectRegistry &registry) {
  registry.addExtensions<
      mlir::iree_compiler::IREE::transform_dialect::CommonExtensions>();
}

// Return true if all the uses of op are either Store/transfer_write.
// There can be SubviewOp users as long as all its users are also
// StoreOp/transfer_write. If return true it also fills out the uses, if it
// returns false uses is unchanged.
static bool allUsesAreStores(Operation *op, std::vector<Operation *> &uses) {
  std::vector<Operation *> opUses;
  for (OpOperand &use : op->getUses()) {
    Operation *useOp = use.getOwner();
    if (isa<memref::DeallocOp, vector::TransferWriteOp, memref::StoreOp>(
            useOp) ||
        (isa<memref::SubViewOp>(useOp) && allUsesAreStores(useOp, opUses))) {
      opUses.push_back(useOp);
      continue;
    }
    return false;
  }
  uses.insert(uses.end(), opUses.begin(), opUses.end());
  return true;
}

// Track temporary allocations that are never read from. If this is the case
// it means both the allocations and associated stores can be removed.
static void eraseDeadAllocAndStores(RewriterBase &rewriter,
                                    Operation *parentOp) {
  std::vector<Operation *> opToErase;
  parentOp->walk([&](memref::AllocOp op) {
    if (allUsesAreStores(op, opToErase)) {
      opToErase.push_back(op.getOperation());
    }
  });
  for (Operation *op : opToErase)
    rewriter.eraseOp(op);
}

//===---------------------------------------------------------------------===//
// ApplyBufferOptimizationsOp
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::ApplyBufferOptimizationsOp::applyToOne(
    transform::TransformRewriter &rewriter, Operation *target,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  // Apply store to load forwarding and dead store elimination.
  vector::transferOpflowOpt(rewriter, target);
  eraseDeadAllocAndStores(rewriter, target);
  return DiagnosedSilenceableFailure::success();
}

void transform_dialect::ApplyBufferOptimizationsOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

void transform_dialect::ApplyBufferOptimizationsOp::build(
    OpBuilder &builder, OperationState &result, Value target) {
  result.addOperands(target);
}

//===---------------------------------------------------------------------===//
// ApplyIreeLinalgElementwiseGreedyFusionPatternsOp
//===---------------------------------------------------------------------===//

static void addOperands(Operation *op, SetVector<Value> &operandSet) {
  if (!op)
    return;
  TypeSwitch<Operation *, void>(op)
      .Case<linalg::LinalgOp>([&](linalg::LinalgOp linalgOp) {
        SmallVector<Value> inputOperands{linalgOp.getDpsInputOperands()};
        operandSet.insert(inputOperands.begin(), inputOperands.end());
      })
      .Default([&](Operation *operation) {
        operandSet.insert(operation->operand_begin(), operation->operand_end());
      });
}

template <int limit = 3>
static bool setFusedOpOperandLimit(OpOperand *fusedOperand) {
  Operation *producer = fusedOperand->get().getDefiningOp();
  if (!producer)
    return false;
  Operation *consumer = fusedOperand->getOwner();
  SetVector<Value> fusedOpOperands;
  if (producer->getNumResults() != 1)
    return false;
  addOperands(consumer, fusedOpOperands);
  fusedOpOperands.remove(producer->getResult(0));
  addOperands(producer, fusedOpOperands);
  return fusedOpOperands.size() <= limit;
}

void transform_dialect::ApplyIreeLinalgElementwiseGreedyFusionPatternsOp::
    populatePatterns(RewritePatternSet &patterns) {
  linalg::populateElementwiseOpsFusionPatterns(patterns,
                                               setFusedOpOperandLimit<3>);
}

//===---------------------------------------------------------------------===//
// ApplyFoldFillIntoPadPatternsOp
//===---------------------------------------------------------------------===//

namespace {
/// Fold `tensor.pad(cst, tensor.extract*(linalg.fill(cst)))` into
/// `linalg.fill(cst, empty)` when the padding constant and the fill constant
/// are the same.
/// This seems generally desirable as a folding but may be too intrusive, so we
/// only apply it selectively for now.
// TODO: atm hardcoded on linalg.fill but we could take any result of any
// generic that yields a constant in that result.
struct FoldFillIntoPad : public OpRewritePattern<tensor::PadOp> {
  using OpRewritePattern<tensor::PadOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(tensor::PadOp padOp,
                                PatternRewriter &rewriter) const final {
    Operation *currentOp = padOp.getSource().getDefiningOp();
    auto maybeExtractSlice =
        dyn_cast_or_null<tensor::ExtractSliceOp>(currentOp);
    while (currentOp && maybeExtractSlice) {
      currentOp = maybeExtractSlice.getSource().getDefiningOp();
      maybeExtractSlice = dyn_cast_or_null<tensor::ExtractSliceOp>(currentOp);
    }
    auto fillOp = dyn_cast_or_null<linalg::FillOp>(currentOp);
    if (!fillOp) {
      return rewriter.notifyMatchFailure(
          padOp, "not coming from a linalg.fill op via tensor.extract_slice*");
    }

    Value padValue = padOp.getConstantPaddingValue();
    RankedTensorType resultType = padOp.getResultType();
    if (!padValue ||
        getAsOpFoldResult(padValue) !=
            getAsOpFoldResult(fillOp.getDpsInputOperand(0)->get())) {
      return rewriter.notifyMatchFailure(
          padOp, "not a constant value matching the fill value");
    }

    Location loc = padOp.getLoc();
    auto emptyOp = rewriter.create<tensor::EmptyOp>(
        loc, resultType,
        linalg::createDynamicDimensions(rewriter, loc, padOp.getResult()));
    rewriter.replaceOpWithNewOp<linalg::FillOp>(padOp, padValue,
                                                emptyOp.getResult());

    return success();
  }
};
} // namespace

void transform_dialect::ApplyFoldFillIntoPadPatternsOp::populatePatterns(
    RewritePatternSet &patterns) {
  patterns.insert<FoldFillIntoPad>(patterns.getContext());
}

//===---------------------------------------------------------------------===//
// ApplyUnrollVectorsGpuMmaSyncPatternsOp
//===---------------------------------------------------------------------===//

static std::optional<SmallVector<int64_t>>
getGPUTensorCoreNativeMmaSyncVectorSize(Operation *op) {
  return getMmaNativeVectorSize(op);
}

void transform_dialect::ApplyUnrollVectorsGpuMmaSyncPatternsOp::
    populatePatterns(RewritePatternSet &patterns) {
  auto unrollOrder = [](Operation *op) -> std::optional<SmallVector<int64_t>> {
    auto contract = dyn_cast<vector::ContractionOp>(op);
    if (!contract)
      return std::nullopt;
    return mlir::iree_compiler::gpuMmaUnrollOrder(contract);
  };
  vector::populateVectorUnrollPatterns(
      patterns, vector::UnrollVectorOptions()
                    .setNativeShapeFn(getGPUTensorCoreNativeMmaSyncVectorSize)
                    .setUnrollTraversalOrderFn(unrollOrder));
}

//===---------------------------------------------------------------------===//
// ApplyUnrollVectorsGpuWmmaSyncPatternsOp
//===---------------------------------------------------------------------===//

static std::optional<SmallVector<int64_t>>
getGPUTensorCoreNativeWmmaVectorSize(Operation *op) {
  return getWmmaNativeVectorSize(op);
}

void transform_dialect::ApplyUnrollVectorsGpuWmmaSyncPatternsOp::
    populatePatterns(RewritePatternSet &patterns) {
  auto unrollOrder = [](Operation *op) -> std::optional<SmallVector<int64_t>> {
    auto contract = dyn_cast<vector::ContractionOp>(op);
    if (!contract)
      return std::nullopt;
    return mlir::iree_compiler::gpuMmaUnrollOrder(contract);
  };
  vector::populateVectorUnrollPatterns(
      patterns, vector::UnrollVectorOptions()
                    .setNativeShapeFn(getGPUTensorCoreNativeWmmaVectorSize)
                    .setUnrollTraversalOrderFn(unrollOrder));
}

//===---------------------------------------------------------------------===//
// Remaining Apply...PatternsOp
//===---------------------------------------------------------------------===//

void transform_dialect::ApplyBubbleCollapsePatternsOp::populatePatterns(
    RewritePatternSet &patterns) {
  linalg::populateFoldReshapeOpsByCollapsingPatterns(
      patterns, [](OpOperand *) { return true; });
}

void transform_dialect::ApplyBubbleExpandPatternsOp::populatePatterns(
    RewritePatternSet &patterns) {
  linalg::populateFoldReshapeOpsByExpansionPatterns(
      patterns, [](OpOperand *) { return true; });
}

void transform_dialect::ApplyBubblePackUnpackPatternsOp::populatePatterns(
    RewritePatternSet &patterns) {
  linalg::populateDataLayoutPropagationPatterns(
      patterns, [](Operation *op) { return true; });
}

void transform_dialect::ApplyFoldReshapeIntoTensorHalInterfacePatternsOp::
    populatePatterns(RewritePatternSet &patterns) {
  populateReshapeToInterfaceTensorPatterns(patterns);
}

void transform_dialect::ApplyPrepareVectorToMMAPatternsOp::populatePatterns(
    RewritePatternSet &patterns) {
  populatePrepareVectorToMMAPatterns(patterns, getUseNvGpu());
}

//===---------------------------------------------------------------------===//
// ApplyCommonSubexpressionEliminationOp
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::ApplyCommonSubexpressionEliminationOp::applyToOne(
    transform::TransformRewriter &rewriter, Operation *target,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  ErrorCheckingTrackingListener listener(state, *this);
  Operation *lastOpVisited = nullptr;

  WalkResult status = target->walk<WalkOrder::PreOrder>([&](Operation *op) {
    if (op->hasTrait<OpTrait::IsIsolatedFromAbove>()) {
      lastOpVisited = op;
      if (failed(eliminateCommonSubexpressions(op, /*domInfo=*/nullptr,
                                               &listener)))
        return WalkResult::interrupt();
      if (listener.failed())
        return WalkResult::interrupt();
      return WalkResult::skip();
    }
    return WalkResult::advance();
  });

  if (!status.wasInterrupted())
    return DiagnosedSilenceableFailure::success();

  if (listener.failed())
    return listener.checkAndResetError();

  return mlir::emitDefiniteFailure(lastOpVisited, "CSE failed");
}

void transform_dialect::ApplyCommonSubexpressionEliminationOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

//===---------------------------------------------------------------------===//
// ApplyLoopIndependentCodeMotionOp
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::ApplyLoopIndependentCodeMotionOp::applyToOne(
    transform::TransformRewriter &rewriter, Operation *target,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  ErrorCheckingTrackingListener listener(state, *this);
  target->walk([&](func::FuncOp funcOp) {
    // This assumes LICM never removes operations so we don't need tracking.
    // TODO: confirm / revisit this assumption and plumb a rewriter through
    // upstream moveLoopInvariantCode if necessary.
    funcOp->walk(
        [](LoopLikeOpInterface loopLike) { moveLoopInvariantCode(loopLike); });
    // For now, put single loop promotion as part of licm. Underlying
    // implementations perform splice operations which shouldn't need
    // tracking.
    // TODO: confirm / revisit this assumption and plumb a rewriter through
    // upstream moveLoopInvariantCode if necessary.
    funcOp->walk([](Operation *op) {
      (void)llvm::TypeSwitch<Operation *, LogicalResult>(op)
          .Case<affine::AffineForOp, scf::ForOp>(
              [](auto loop) { return promoteIfSingleIteration(loop); })
          .Default([](Operation *) { return success(); });
    });
  });

  return listener.checkAndResetError();
}

void transform_dialect::ApplyLoopIndependentCodeMotionOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

//===----------------------------------------------------------------------===//
// HoistStaticAllocOp
//===----------------------------------------------------------------------===//

DiagnosedSilenceableFailure transform_dialect::HoistStaticAllocOp::applyToOne(
    transform::TransformRewriter &rewriter, func::FuncOp target,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  mlir::iree_compiler::hoistStaticallyBoundAllocationsInFunc<memref::AllocOp>(
      rewriter, target);
  return DiagnosedSilenceableFailure::success();
}

void transform_dialect::HoistStaticAllocOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

//===----------------------------------------------------------------------===//
// ShareForallOperandsOp
//===----------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::ShareForallOperandsOp::applyToOne(
    transform::TransformRewriter &rewriter, scf::ForallOp forallOp,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  SmallVector<int64_t> shareOperands(getShareOperands());
  // Empty case: consider all operands need to be shared.
  if (shareOperands.empty()) {
    shareOperands =
        llvm::to_vector(llvm::seq<int64_t>(0, forallOp.getOutputs().size()));
  }
  for (int64_t outputIdx : getShareOperands()) {
    if (outputIdx < 0 || outputIdx >= forallOp.getOutputs().size())
      return mlir::emitDefiniteFailure(forallOp, "operand idx overflow");
    Value toShare = forallOp.getOutputs()[outputIdx];
    if (std::distance(toShare.getUses().begin(), toShare.getUses().end()) !=
        2) {
      /*return mlir::emitSilenceableFailure(
          forallOp,
          "operand to share must have exactly 2 uses, the forall op "
          "and an extract_slice op.");*/
      continue;
    }
    tensor::ExtractSliceOp extractSliceOp;
    for (Operation *user : toShare.getUsers()) {
      extractSliceOp = dyn_cast<tensor::ExtractSliceOp>(user);
      if (extractSliceOp)
        break;
    }
    if (!extractSliceOp) {
      /*return mlir::emitSilenceableFailure(
        forallOp,
        "shared operands use must be extractSliceOp.");*/
      continue;
    }
    // Get the corresponding bbArg.
    BlockArgument bbArg = forallOp.getOutputBlockArguments()[outputIdx];

    // Check if the extract_slice has a matching parallel_insert_slice
    // (i.e., same source/target, offsets, sizes and strides).
    auto isMatchingParallelInsertSlice = [&](Operation &op) {
      auto insertSlice = dyn_cast<tensor::ParallelInsertSliceOp>(&op);
      if (!insertSlice)
        return false;
      if (insertSlice.getDest() != bbArg)
        return false;
      return llvm::equal(insertSlice.getMixedOffsets(),
                         extractSliceOp.getMixedOffsets()) &&
             llvm::equal(insertSlice.getMixedSizes(),
                         extractSliceOp.getMixedSizes()) &&
             llvm::equal(insertSlice.getMixedStrides(),
                         extractSliceOp.getMixedStrides());
    };
    if (llvm::none_of(forallOp.getTerminator().getYieldingOps(),
                      isMatchingParallelInsertSlice)) {
      continue;
    }

    // Promote extract_slice source to bbArg.
    rewriter.updateRootInPlace(extractSliceOp, [&]() {
      extractSliceOp.getSourceMutable().assign(bbArg);
    });
  }

  results.push_back(forallOp);
  return DiagnosedSilenceableFailure::success();
}

//===---------------------------------------------------------------------===//
// ForallToWorkgroupOp
//===---------------------------------------------------------------------===//

LogicalResult rewriteForallToWorkgroup(RewriterBase &rewriter,
                                       scf::ForallOp forallOp,
                                       IREE::HAL::ExecutableExportOp exportOp) {
  // Step 0. Target-specific verifications. There is no good place to anchor
  // those right now: the ForallOp is target-independent and the
  // transform op does not apply to individual ForallOp.
  MLIRContext *ctx = forallOp->getContext();
  Location loc = forallOp->getLoc();
  // TODO iree should have own device mapping like #hal.workgroup<x/y/z>
  Attribute bX = gpu::GPUBlockMappingAttr::get(ctx, gpu::Blocks::DimX);
  Attribute bY = gpu::GPUBlockMappingAttr::get(ctx, gpu::Blocks::DimY);
  Attribute bZ = gpu::GPUBlockMappingAttr::get(ctx, gpu::Blocks::DimZ);
  if (forallOp.getNumResults() > 0)
    return forallOp->emitError(
        "only bufferized scf.forall lowers to workgroup");
  if (forallOp.getRank() > 3)
    return forallOp->emitError(
        "scf.forall with rank > 3 does not lower to workgroup");

  if (!forallOp.getMapping().has_value())
    return forallOp->emitError("mapping must be present");
  SmallVector<Attribute> blockMapping =
      llvm::to_vector(forallOp.getMapping()->getValue());
  if (llvm::any_of(blockMapping, [](Attribute map) {
        return !llvm::isa<gpu::GPUBlockMappingAttr>(map);
      })) {
    return forallOp->emitError("mapping must be #gpu.block<x/y/z/>");
  }

  // Step 1. Complete the blockMapping to a full mapping (with 1s) if
  // necessary.
  SmallVector<Value> numBlocks =
      llvm::to_vector(forallOp.getUpperBound(rewriter));
  // Ensure we have 3 block sizes, one for each id.
  Value one;
  for (auto attr : {bX, bY, bZ}) {
    if (!llvm::is_contained(blockMapping, attr)) {
      blockMapping.push_back(attr);
      one = one ? one : rewriter.create<arith::ConstantIndexOp>(loc, 1);
      numBlocks.push_back(one);
    }
  }
  // Step 2. sort the values by the corresponding GPUBlockMappingAttr.
  auto comparator = [](Attribute a, Attribute b) -> bool {
    return static_cast<int64_t>(
               llvm::cast<gpu::GPUBlockMappingAttr>(a).getBlock()) <
           static_cast<int64_t>(
               llvm::cast<gpu::GPUBlockMappingAttr>(b).getBlock());
  };
  SmallVector<Value> gridDimValues =
      getValuesSortedByKey(blockMapping, numBlocks, comparator);

  // Step 3. Create the workgroup id and count ops.
  IRMapping bvm;
  SmallVector<Value> workgroupIdOps, workgroupCountOps;
  for (Attribute attr : blockMapping) {
    auto idx = static_cast<int64_t>(
        llvm::cast<gpu::GPUBlockMappingAttr>(attr).getBlock());
    workgroupIdOps.push_back(
        rewriter.create<HAL::InterfaceWorkgroupIDOp>(loc, idx));
    workgroupCountOps.push_back(
        rewriter.create<HAL::InterfaceWorkgroupCountOp>(loc, idx));
  }
  bvm.map(forallOp.getInductionVars(), workgroupIdOps);
  bvm.map(forallOp.getUpperBound(rewriter), workgroupCountOps);

  // Step 4. Predicate omitted given unique topLevel scf::ForallOp.

  // Step 5. Move the body of forallOp.
  // Erase the terminator first, it will not be used since we are on buffers.
  rewriter.eraseOp(forallOp.getTerminator());
  Block *targetBlock;
  Block::iterator insertionPoint;
  targetBlock = forallOp->getBlock();
  insertionPoint = Block::iterator(forallOp);
  Block &sourceBlock = forallOp.getRegion().front();
  targetBlock->getOperations().splice(insertionPoint,
                                      sourceBlock.getOperations());

  // Step 6. RAUW thread indices to thread ops.
  for (Value blockIdx : forallOp.getInductionVars()) {
    for (Operation *user : llvm::make_early_inc_range(blockIdx.getUsers())) {
      rewriter.updateRootInPlace(user, [&]() {
        user->replaceUsesOfWith(blockIdx, bvm.lookup(blockIdx));
      });
    }
  }

  // Step 6. Barriers omitted given unique topLevel scf::ForallOp.

  // Step 7. Erase old op.
  rewriter.eraseOp(forallOp);

  return success();
}

//===---------------------------------------------------------------------===//
// IREE-specific transformations defined outside of iree_linalg_transform.
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure transform_dialect::ForallToWorkgroupOp::applyToOne(
    transform::TransformRewriter &rewriter, func::FuncOp target,
    transform::ApplyToEachResultList &results,
    transform::TransformState &state) {
  if (!isa<HAL::ExecutableOp, HAL::ExecutableVariantOp>(state.getTopLevel())) {
    return mlir::emitDefiniteFailure(
        state.getTopLevel(),
        "requires HAL::ExecutableOp or HAL::ExecutableVariantOp toplevel "
        "to attach the workgroup size information to a nested "
        "ExecutableExportOp");
  }

  IREE::HAL::ExecutableExportOp exportOp;
  state.getTopLevel()->walk([&](IREE::HAL::ExecutableExportOp op) {
    if (op.getSymName() == target.getName())
      exportOp = op;
  });
  if (!exportOp) {
    return mlir::emitSilenceableFailure(
        target, "no IREE::HAL::ExecutableExportOp found");
  }

  scf::ForallOp topLevelForallOp;
  auto walkResult = target->walk([&](scf::ForallOp forallOp) {
    if (forallOp->getParentOfType<scf::ForallOp>())
      return WalkResult::advance();
    if (topLevelForallOp)
      return WalkResult::interrupt();
    topLevelForallOp = forallOp;
    return WalkResult::advance();
  });

  if (walkResult.wasInterrupted()) {
    return mlir::emitSilenceableFailure(
        target, "could not find a unique topLevel scf.forall");
  }

  rewriter.setInsertionPoint(topLevelForallOp);
  if (failed(rewriteForallToWorkgroup(rewriter, topLevelForallOp, exportOp)))
    return mlir::emitDefiniteFailure(target, "rewriteForallToWorkgroup failed");

  return DiagnosedSilenceableFailure::success();
}

void transform_dialect::ForallToWorkgroupOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

//===---------------------------------------------------------------------===//
// IREEPopulateWorkgroupCountRegionUsingNumThreadsSliceOp
//===---------------------------------------------------------------------===//

void transform_dialect::IREEPopulateWorkgroupCountRegionUsingNumThreadsSliceOp::
    getEffects(SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getForAllOp(), effects);
  transform::modifiesPayload(effects);
}

DiagnosedSilenceableFailure
transform_dialect::IREEPopulateWorkgroupCountRegionUsingNumThreadsSliceOp::
    applyToOne(transform::TransformRewriter &rewriter, Operation *target,
               transform::ApplyToEachResultList &results,
               transform::TransformState &state) {
  auto forAllOp = dyn_cast<scf::ForallOp>(target);
  if (!forAllOp) {
    return mlir::emitDefiniteFailure(state.getTopLevel(),
                                     "expected scf.forall operation handle");
  }
  if (!forAllOp.isNormalized()) {
    return mlir::emitDefiniteFailure(state.getTopLevel(),
                                     "Expect the for op to be normalized");
  }
  auto workgroupCount =
      getMixedValues(forAllOp.getStaticUpperBound(),
                     forAllOp.getDynamicUpperBound(), rewriter);

  // Account for mapping attribute if present. The attribute used for mapping
  // provides a mapping ID that is ordered in `x` = 0, `y`=1, and `z` = 2. Use
  // this to shuffle the workgroup count around.
  if (auto blockMapping = forAllOp.getMapping()) {
    // Get the mapping IDs.
    auto mappingIds = llvm::map_to_vector(
        blockMapping.value(), [](Attribute mappingAttr) -> int {
          return llvm::cast<DeviceMappingAttrInterface>(mappingAttr)
              .getMappingId();
        });
    int maxId = 0;
    for (auto id : mappingIds) {
      maxId = std::max(maxId, id);
    }
    SmallVector<OpFoldResult> workgroupCountOrdered(maxId + 1,
                                                    rewriter.getIndexAttr(1));
    for (auto [index, mapId] : llvm::enumerate(mappingIds)) {
      workgroupCountOrdered[maxId - mapId] = workgroupCount[index];
    }
    workgroupCount = workgroupCountOrdered;
  }

  auto funcOp = forAllOp->getParentOfType<func::FuncOp>();
  if (failed(
          lowerWorkgroupCountFromSliceOp(rewriter, funcOp, workgroupCount))) {
    return mlir::emitDefiniteFailure(state.getTopLevel(),
                                     "failed to lower workgroup count region");
  }
  return DiagnosedSilenceableFailure::success();
}

//===---------------------------------------------------------------------===//
// IREEBufferizeOp
//===---------------------------------------------------------------------===//

// Important note: this transform is load-bearing and is the glue between
// different dialects that want to operate on tensors.
// Originaly, it used to just call `addIREEComprehensiveBufferizePasses` but
// this introduces a lot of complexity in the registration process due to the
// use of nested pass pipelines, to a point that it is a major endeavor to
// connect a new dialect.
// Instead, avoid calling the passes and only take what we need from them.
//
// TODO: Maybe we need both a transform.iree.cpu.bufferize and a
// transform.iree.gpu.bufferize rather than a single common bufferize op?
//
// Note: This has become so specific that it may be worth it to separate in
// its own .cpp file.
using mlir::bufferization::BufferizationOptions;
using mlir::bufferization::OneShotAnalysisState;
using mlir::bufferization::OneShotBufferizationOptions;

void transform_dialect::IREEBufferizeOp::build(OpBuilder &builder,
                                               OperationState &result,
                                               Value target, bool targetGpu,
                                               bool testAnalysisOnly,
                                               bool printConflicts) {
  result.addOperands(target);
  if (targetGpu) {
    result.addAttribute(IREEBufferizeOp::getTargetGpuAttrName(result.name),
                        builder.getUnitAttr());
  }
  if (testAnalysisOnly) {
    result.addAttribute(
        IREEBufferizeOp::getTestAnalysisOnlyAttrName(result.name),
        builder.getUnitAttr());
  }
  if (printConflicts) {
    result.addAttribute(IREEBufferizeOp::getPrintConflictsAttrName(result.name),
                        builder.getUnitAttr());
  }
  MLIRContext *ctx = builder.getContext();
  result.addTypes(transform::AnyOpType::get(ctx));
}

//===---------------------------------------------------------------------===//
// Default allocation functions for CPU backend
// TODO: register the bufferization behavior in a target-specific way.
// TODO: Maybe bufferize should have a separate cpu and a gpu version. This is
// unclear though: what happens on heterogeneous HW ?
//===---------------------------------------------------------------------===//

// Allocation callbacks to use with upstream comprehensive bufferization
static FailureOr<Value> cpuComprehensiveBufferizeAllocationFn(
    OpBuilder &builder, Location loc, MemRefType memRefType,
    ValueRange dynamicSizes, unsigned alignment) {
  return builder
      .create<memref::AllocaOp>(loc, memRefType, dynamicSizes,
                                builder.getI64IntegerAttr(alignment))
      .getResult();
}

static LogicalResult cpuComprehensiveBufferizeDeallocationFn(OpBuilder &builder,
                                                             Location loc,
                                                             Value allocation) {
  return success();
}

static LogicalResult cpuComprehensiveBufferizeCopyFn(OpBuilder &builder,
                                                     Location loc, Value from,
                                                     Value to) {
  // TODO: ideally we should use linalg.copy which was recently reintroduced
  // as an OpDSL named op. However, IREE-specific patterns to cleanup spurious
  // post-bufferization copies do not trigger properly.
  // So we keep using `createLinalgCopyOp` which builds a GenericOp.
  // builder.create<linalg::CopyOp>(loc, from, to);
  mlir::iree_compiler::createLinalgCopyOp(builder, loc, from, to);
  return success();
}

static FailureOr<Value> gpuComprehensiveBufferizeAllocationFn(
    OpBuilder &builder, Location loc, MemRefType memRefType,
    ValueRange dynamicSizes, unsigned alignment) {
  auto addressSpaceAttr = gpu::AddressSpaceAttr::get(
      builder.getContext(), gpu::GPUDialect::getWorkgroupAddressSpace());
  MemRefType allocType =
      MemRefType::get(memRefType.getShape(), memRefType.getElementType(),
                      AffineMap(), addressSpaceAttr);
  return builder
      .create<memref::AllocOp>(loc, allocType, dynamicSizes,
                               builder.getI64IntegerAttr(alignment))
      .getResult();
}

static LogicalResult gpuComprehensiveBufferizeDeallocationFn(OpBuilder &builder,
                                                             Location loc,
                                                             Value allocation) {
  builder.create<memref::DeallocOp>(loc, allocation);
  return success();
}

static LogicalResult gpuComprehensiveBufferizeCopyFn(OpBuilder &builder,
                                                     Location loc, Value from,
                                                     Value to) {
  // Insert barriers for copies from and to shared memory.
  bool needsBarrier = false;
  if (hasSharedMemoryAddressSpace(llvm::cast<MemRefType>(from.getType())) !=
      hasSharedMemoryAddressSpace(llvm::cast<MemRefType>(to.getType()))) {
    needsBarrier = true;
  }
  if (needsBarrier)
    builder.create<gpu::BarrierOp>(loc);
  // TODO: ideally we should use linalg.copy which was recently reintroduced
  // as an OpDSL named op. However, IREE-specific patterns to cleanup spurious
  // post-bufferization copies do not trigger properly.
  // So we keep using `createLinalgCopyOp` which builds a GenericOp.
  // builder.create<linalg::CopyOp>(loc, from, to);
  Operation* copy = mlir::iree_compiler::createLinalgCopyOp(builder, loc, from, to);
  setMarker(copy, getCopyToWorkgroupMemoryMarker());
  if (needsBarrier)
    builder.create<gpu::BarrierOp>(loc);
  return success();
}

static IREEOneShotBufferizationOptions getBufferizationOptions() {
  IREEOneShotBufferizationOptions options;
  // options.testAnalysisOnly = testAnalysisOnly;
  // options.printConflicts = printConflicts;

  // bufferization.to_memref is used to bufferize constants in IREE. IREE has
  // it's own logic to handle constants. We'd like to leave the arith.constant
  // as is and insert bufferization.to_memref to convert the tensor to memref.
  options.opFilter.denyOperation<arith::ConstantOp>();
  options.opFilter.denyOperation<bufferization::ToMemrefOp>();

  // This type converter converts tensor types to memref types when no exact
  // memref type can be inferred from the context.
  options.unknownTypeConverterFn = [](Value value, Attribute memorySpace,
                                      const BufferizationOptions &options) {
    auto tensorType = llvm::cast<TensorType>(value.getType());

    // Special rule for ConstantOps: These always lower to some memref with a
    // static identity layout.
    if (value.getDefiningOp<arith::ConstantOp>())
      return bufferization::getMemRefTypeWithStaticIdentityLayout(tensorType,
                                                                  memorySpace);

    // Default case: Fully dynamic layout map for best compatibility.
    return bufferization::getMemRefTypeWithFullyDynamicLayout(tensorType,
                                                              memorySpace);
  };

  return options;
}

namespace {
/// Pattern to rewrite tensor.empty to tensor.alloc.
struct EmptyTensorLoweringPattern : public OpRewritePattern<tensor::EmptyOp> {
  using OpRewritePattern<tensor::EmptyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::EmptyOp op,
                                PatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<bufferization::AllocTensorOp>(
        op, op.getType(), op.getDynamicSizes());
    return success();
  }
};
} // namespace

DiagnosedSilenceableFailure transform_dialect::IREEBufferizeOp::apply(
    transform::TransformRewriter &rewriter,
    transform::TransformResults &results, transform::TransformState &state) {
  auto payload = state.getPayloadOps(getTarget());
  if (!llvm::hasSingleElement(payload) ||
      !isa<ModuleOp, HAL::ExecutableOp, HAL::ExecutableVariantOp>(
          *payload.begin())) {
    return mlir::emitDefiniteFailure(
        state.getTopLevel(), "requires exactly a single HAL::ExecutableOp or "
                             "HAL::ExecutableVariantOp target op.");
  }

  //===-------------------------------------------------------------------===//
  // DO NOT JUST CALL `addIREEComprehensiveBufferizePasses` as this results in
  // a lot of registration issues due to nested pass pipeline mess.
  // Instead, take what we need from it.
  //===-------------------------------------------------------------------===//
  // Bufferize the dispatch.
  using mlir::bufferization::BufferizationOptions;
  BufferizationOptions::AllocationFn allocationFn =
      cpuComprehensiveBufferizeAllocationFn;
  BufferizationOptions::DeallocationFn deallocationFn =
      cpuComprehensiveBufferizeDeallocationFn;
  BufferizationOptions::MemCpyFn memCpyFn = cpuComprehensiveBufferizeCopyFn;
  if (getTargetGpu()) {
    allocationFn = gpuComprehensiveBufferizeAllocationFn;
    deallocationFn = gpuComprehensiveBufferizeDeallocationFn;
    memCpyFn = gpuComprehensiveBufferizeCopyFn;
  }

  Operation *target = *payload.begin();
  ErrorCheckingTrackingListener listener(state, *this);
  //   1. Rewrite tensor.empty to tensor.alloc, without the pass baggage.
  {
    RewritePatternSet patterns(getContext());
    patterns.add<EmptyTensorLoweringPattern>(patterns.getContext());
    GreedyRewriteConfig config;
    config.listener = &listener;
    // Manually gather list of ops because the other GreedyPatternRewriteDriver
    // overloads only accepts ops that are isolated from above.
    SmallVector<Operation *> ops;
    state.getTopLevel()->walk([&](Operation *nestedOp) {
      if (state.getTopLevel() != nestedOp)
        ops.push_back(nestedOp);
    });
    LogicalResult result =
        applyOpPatternsAndFold(ops, std::move(patterns), config);
    if (failed(result)) {
      return mlir::emitDefiniteFailure(state.getTopLevel(),
                                       "greedy pattern application failed");
    }
    if (listener.failed())
      return listener.checkAndResetError();
  }

  //   2. Run one-shot-bufferize, without the pass baggage.
  IREEOneShotBufferizationOptions options = getBufferizationOptions();
  options.allocationFn = allocationFn;
  options.deallocationFn = deallocationFn;
  options.memCpyFn = memCpyFn;
  options.testAnalysisOnly = getTestAnalysisOnly();
  options.printConflicts = getPrintConflicts();
  if (failed(runIREEOneShotBufferize(state.getTopLevel(), options)))
    return mlir::emitDefiniteFailure(state.getTopLevel(),
                                     "bufferization failed");

  // Early exit if test_analysis_only is set.
  if (getTestAnalysisOnly()) {
    results.set(getOperation()->getOpResult(0), {*payload.begin()});
    return listener.checkAndResetError();
  }

  //   3. Post-bufferization passes are fine.
  PassManager pm(getContext());
  addIREEPostBufferizationPasses(pm);
  WalkResult res = state.getTopLevel()->walk([&](ModuleOp moduleOp) {
    if (failed(pm.run(moduleOp))) {
      getOperation()->emitError()
          << "failed to post-bufferization passes on module:\n"
          << *(moduleOp.getOperation()) << "\nunder top-level:\n"
          << *state.getTopLevel();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  if (res.wasInterrupted())
    return mlir::emitDefiniteFailure(target)
           << "post-bufferization passes failed";

  results.set(getOperation()->getOpResult(0), {*payload.begin()});
  return listener.checkAndResetError();
}

//===---------------------------------------------------------------------===//
// IREEEliminateEmptyTensorsOp
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::IREEEliminateEmptyTensorsOp::applyToOne(
    transform::TransformRewriter &rewriter, ::mlir::Operation *target,
    ::mlir::transform::ApplyToEachResultList &results,
    ::mlir::transform::TransformState &state) {
  if (failed(
          eliminateEmptyTensors(rewriter, target, getBufferizationOptions())))
    return emitDefaultDefiniteFailure(target)
           << "failed to eliminate tensor.empty ops";
  return DiagnosedSilenceableFailure::success();
}

void transform_dialect::IREEEliminateEmptyTensorsOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

//===---------------------------------------------------------------------===//
// EraseHALDescriptorTypeFromMemRef
//===---------------------------------------------------------------------===//

DiagnosedSilenceableFailure
transform_dialect::IREEEraseHALDescriptorTypeFromMemRefOp::applyToOne(
    transform::TransformRewriter &rewriter, ::mlir::Operation *target,
    ::mlir::transform::ApplyToEachResultList &results,
    ::mlir::transform::TransformState &state) {
  if (!isa<func::FuncOp>(target)) {
    return mlir::emitDefiniteFailure(state.getTopLevel(),
                                     "expects a func::FuncOp as the target op");
  }
  auto funcOp = cast<func::FuncOp>(target);

  if (failed(eraseHALDescriptorTypeFromMemRef(funcOp))) {
    return mlir::emitDefiniteFailure(
        state.getTopLevel(),
        "failed to erase #hal.descriptor_type as MemRef memory space");
  }

  return DiagnosedSilenceableFailure::success();
}

void transform_dialect::IREEEraseHALDescriptorTypeFromMemRefOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  transform::onlyReadsHandle(getTarget(), effects);
  transform::modifiesPayload(effects);
}

#define GET_OP_CLASSES
#include "iree/compiler/Codegen/Common/TransformExtensions/CommonExtensionsOps.cpp.inc"
