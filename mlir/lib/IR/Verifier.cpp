//===- Verifier.cpp - MLIR Verifier Implementation ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the verify() methods on the various IR types, performing
// (potentially expensive) checks on the holistic structure of the code.  This
// can be used for detecting bugs in compiler transformations and hand written
// .mlir files.
//
// The checks in this file are only for things that can occur as part of IR
// transformations: e.g. violation of dominance information, malformed operation
// attributes, etc.  MLIR supports transformations moving IR through locally
// invalid states (e.g. unlinking an operation from a block before re-inserting
// it in a new place), but each transformation must complete with the IR in a
// valid form.
//
// This should not check for things that are always wrong by construction (e.g.
// attributes or other immutable structures that are incorrect), because those
// are not mutable and can be checked at time of construction.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Verifier.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/RegionKindInterface.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"

using namespace mlir;

namespace {
/// This class encapsulates all the state used to verify an operation region.
class OperationVerifier {
public:
  explicit OperationVerifier() {}

  /// Verify the given operation.
  LogicalResult verifyOpAndDominance(Operation &op);

private:
  /// Verify the given potentially nested region or block.
  LogicalResult verifyRegion(Region &region);
  LogicalResult verifyBlock(Block &block);
  LogicalResult verifyOperation(Operation &op);

  /// Verify the dominance property of regions contained within the given
  /// Operation.
  LogicalResult verifyDominanceOfContainedRegions(Operation &op);

  /// Emit an error for the given block.
  InFlightDiagnostic emitError(Block &bb, const Twine &message) {
    // Take the location information for the first operation in the block.
    if (!bb.empty())
      return bb.front().emitError(message);

    // Worst case, fall back to using the parent's location.
    return mlir::emitError(bb.getParent()->getLoc(), message);
  }

  /// Dominance information for this operation, when checking dominance.
  DominanceInfo *domInfo = nullptr;
};
} // end anonymous namespace

/// Verify the given operation.
LogicalResult OperationVerifier::verifyOpAndDominance(Operation &op) {
  // Verify the operation first.
  if (failed(verifyOperation(op)))
    return failure();

  // Since everything looks structurally ok to this point, we do a dominance
  // check for any nested regions. We do this as a second pass since malformed
  // CFG's can cause dominator analysis constructure to crash and we want the
  // verifier to be resilient to malformed code.
  DominanceInfo theDomInfo(&op);
  domInfo = &theDomInfo;
  if (failed(verifyDominanceOfContainedRegions(op)))
    return failure();

  domInfo = nullptr;
  return success();
}

LogicalResult OperationVerifier::verifyRegion(Region &region) {
  if (region.empty())
    return success();

  // Verify the first block has no predecessors.
  auto *firstBB = &region.front();
  if (!firstBB->hasNoPredecessors())
    return mlir::emitError(region.getLoc(),
                           "entry block of region may not have predecessors");

  // Verify each of the blocks within the region.
  for (Block &block : region)
    if (failed(verifyBlock(block)))
      return failure();
  return success();
}

/// Returns true if this block may be valid without terminator. That is if:
/// - it does not have a parent region.
/// - Or the parent region have a single block and:
///    - This region does not have a parent op.
///    - Or the parent op is unregistered.
///    - Or the parent op has the NoTerminator trait.
static bool mayBeValidWithoutTerminator(Block *block) {
  if (!block->getParent())
    return true;
  if (!llvm::hasSingleElement(*block->getParent()))
    return false;
  Operation *op = block->getParentOp();
  return !op || op->mightHaveTrait<OpTrait::NoTerminator>();
}

LogicalResult OperationVerifier::verifyBlock(Block &block) {
  for (auto arg : block.getArguments())
    if (arg.getOwner() != &block)
      return emitError(block, "block argument not owned by block");

  // Verify that this block has a terminator.
  if (block.empty()) {
    if (mayBeValidWithoutTerminator(&block))
      return success();
    return emitError(block, "empty block: expect at least a terminator");
  }

  // Check each operation, and make sure there are no branches out of the
  // middle of this block.
  for (auto &op : llvm::make_range(block.begin(), block.end())) {
    // Only the last instructions is allowed to have successors.
    if (op.getNumSuccessors() != 0 && &op != &block.back())
      return op.emitError(
          "operation with block successors must terminate its parent block");

    if (failed(verifyOperation(op)))
      return failure();
  }

  // Verify that this block is not branching to a block of a different
  // region.
  for (Block *successor : block.getSuccessors())
    if (successor->getParent() != block.getParent())
      return block.back().emitOpError(
          "branching to block of a different region");

  // If this block doesn't have to have a terminator, don't require it.
  if (mayBeValidWithoutTerminator(&block))
    return success();

  Operation &terminator = block.back();
  if (!terminator.mightHaveTrait<OpTrait::IsTerminator>())
    return block.back().emitError("block with no terminator, has ")
           << terminator;

  return success();
}

LogicalResult OperationVerifier::verifyOperation(Operation &op) {
  // Check that operands are non-nil and structurally ok.
  for (auto operand : op.getOperands())
    if (!operand)
      return op.emitError("null operand found");

  /// Verify that all of the attributes are okay.
  for (auto attr : op.getAttrs()) {
    // Check for any optional dialect specific attributes.
    if (auto *dialect = attr.first.getDialect())
      if (failed(dialect->verifyOperationAttribute(&op, attr)))
        return failure();
  }

  // If we can get operation info for this, check the custom hook.
  OperationName opName = op.getName();
  auto *opInfo = opName.getAbstractOperation();
  if (opInfo && failed(opInfo->verifyInvariants(&op)))
    return failure();

  if (unsigned numRegions = op.getNumRegions()) {
    auto kindInterface = dyn_cast<mlir::RegionKindInterface>(op);

    // Verify that all child regions are ok.
    for (unsigned i = 0; i < numRegions; ++i) {
      Region &region = op.getRegion(i);
      RegionKind kind =
          kindInterface ? kindInterface.getRegionKind(i) : RegionKind::SSACFG;
      // Check that Graph Regions only have a single basic block. This is
      // similar to the code in SingleBlockImplicitTerminator, but doesn't
      // require the trait to be specified. This arbitrary limitation is
      // designed to limit the number of cases that have to be handled by
      // transforms and conversions.
      if (op.isRegistered() && kind == RegionKind::Graph) {
        // Empty regions are fine.
        if (region.empty())
          continue;

        // Non-empty regions must contain a single basic block.
        if (std::next(region.begin()) != region.end())
          return op.emitOpError("expects graph region #")
                 << i << " to have 0 or 1 blocks";
      }
      if (failed(verifyRegion(region)))
        return failure();
    }
  }

  // If this is a registered operation, there is nothing left to do.
  if (opInfo)
    return success();

  // Otherwise, verify that the parent dialect allows un-registered operations.
  Dialect *dialect = opName.getDialect();
  if (!dialect) {
    if (!op.getContext()->allowsUnregisteredDialects()) {
      return op.emitOpError()
             << "created with unregistered dialect. If this is "
                "intended, please call allowUnregisteredDialects() on the "
                "MLIRContext, or use -allow-unregistered-dialect with "
                "mlir-opt";
    }
    return success();
  }

  if (!dialect->allowsUnknownOperations()) {
    return op.emitError("unregistered operation '")
           << op.getName() << "' found in dialect ('" << dialect->getNamespace()
           << "') that does not allow unknown operations";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// Dominance Checking
//===----------------------------------------------------------------------===//

/// Emit an error when the specified operand of the specified operation is an
/// invalid use because of dominance properties.
static void diagnoseInvalidOperandDominance(Operation &op, unsigned operandNo) {
  InFlightDiagnostic diag = op.emitError("operand #")
                            << operandNo << " does not dominate this use";

  Value operand = op.getOperand(operandNo);

  /// Attach a note to an in-flight diagnostic that provide more information
  /// about where an op operand is defined.
  if (auto *useOp = operand.getDefiningOp()) {
    Diagnostic &note = diag.attachNote(useOp->getLoc());
    note << "operand defined here";
    Block *block1 = op.getBlock();
    Block *block2 = useOp->getBlock();
    Region *region1 = block1->getParent();
    Region *region2 = block2->getParent();
    if (block1 == block2)
      note << " (op in the same block)";
    else if (region1 == region2)
      note << " (op in the same region)";
    else if (region2->isProperAncestor(region1))
      note << " (op in a parent region)";
    else if (region1->isProperAncestor(region2))
      note << " (op in a child region)";
    else
      note << " (op is neither in a parent nor in a child region)";
    return;
  }
  // Block argument case.
  Block *block1 = op.getBlock();
  Block *block2 = operand.cast<BlockArgument>().getOwner();
  Region *region1 = block1->getParent();
  Region *region2 = block2->getParent();
  Location loc = UnknownLoc::get(op.getContext());
  if (block2->getParentOp())
    loc = block2->getParentOp()->getLoc();
  Diagnostic &note = diag.attachNote(loc);
  if (!region2) {
    note << " (block without parent)";
    return;
  }
  if (block1 == block2)
    llvm::report_fatal_error("Internal error in dominance verification");
  int index = std::distance(region2->begin(), block2->getIterator());
  note << "operand defined as a block argument (block #" << index;
  if (region1 == region2)
    note << " in the same region)";
  else if (region2->isProperAncestor(region1))
    note << " in a parent region)";
  else if (region1->isProperAncestor(region2))
    note << " in a child region)";
  else
    note << " neither in a parent nor in a child region)";
}

/// Verify the dominance of each of the nested blocks within the given operation
LogicalResult
OperationVerifier::verifyDominanceOfContainedRegions(Operation &op) {
  for (Region &region : op.getRegions()) {
    // Verify the dominance of each of the held operations.
    for (Block &block : region) {
      // Dominance is only meaningful inside reachable blocks.
      bool isReachable = domInfo->isReachableFromEntry(&block);

      for (Operation &op : block) {
        if (isReachable) {
          // Check that operands properly dominate this use.
          for (unsigned operandNo = 0, e = op.getNumOperands(); operandNo != e;
               ++operandNo) {
            if (domInfo->properlyDominates(op.getOperand(operandNo), &op))
              continue;

            diagnoseInvalidOperandDominance(op, operandNo);
            return failure();
          }
        }

        // Recursively verify dominance within each operation in the
        // block, even if the block itself is not reachable, or we are in
        // a region which doesn't respect dominance.
        if (op.getNumRegions() != 0)
          if (failed(verifyDominanceOfContainedRegions(op)))
            return failure();
      }
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Entrypoint
//===----------------------------------------------------------------------===//

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs.  On error, this reports the error through the MLIRContext and
/// returns failure.
LogicalResult mlir::verify(Operation *op) {
  return OperationVerifier().verifyOpAndDominance(*op);
}
