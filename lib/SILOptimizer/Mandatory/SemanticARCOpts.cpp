//===--- SemanticARCOpts.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-semantic-arc-opts"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;

STATISTIC(NumEliminatedInsts, "number of removed instructions");
STATISTIC(NumLoadCopyConvertedToLoadBorrow,
          "number of load_copy converted to load_borrow");

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

/// Return true if v only has invalidating uses that are destroy_value.
///
/// Semantically this implies that a value is never passed off as +1 to memory
/// or another function implying it can be used everywhere at +0.
static bool isConsumed(
    SILValue v, SmallVectorImpl<DestroyValueInst *> &destroys,
    NullablePtr<SmallVectorImpl<SILInstruction *>> forwardingInsts = nullptr) {
  assert(v.getOwnershipKind() == ValueOwnershipKind::Owned);
  SmallVector<Operand *, 32> worklist(v->use_begin(), v->use_end());
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();

    // Skip type dependent operands.
    if (op->isTypeDependent())
      continue;

    auto *user = op->getUser();

    // We know that a copy_value produces an @owned value. Look through all of
    // our uses and classify them as either invalidating or not
    // invalidating. Make sure that all of the invalidating ones are
    // destroy_value since otherwise the live_range is not complete.
    auto map = op->getOwnershipKindMap();
    auto constraint = map.getLifetimeConstraint(ValueOwnershipKind::Owned);
    switch (constraint) {
    case UseLifetimeConstraint::MustBeInvalidated: {
      // See if we have a destroy value. If we don't we have an
      // unknown consumer. Return false, we need this live range.
      if (auto *dvi = dyn_cast<DestroyValueInst>(user)) {
        destroys.push_back(dvi);
        continue;
      }

      // Otherwise, see if we have a forwarding value that has a single
      // non-trivial operand that can accept a guaranteed value. If so, at its
      // users to the worklist and continue.
      //
      // DISCUSSION: For now we do not support forwarding instructions with
      // multiple non-trivial arguments since we would need to optimize all of
      // the non-trivial arguments at the same time.
      //
      // NOTE: Today we do not support TermInsts for simplicity... we /could/
      // support it though if we need to.
      if (forwardingInsts.isNonNull() && !isa<TermInst>(user) &&
          isGuaranteedForwardingInst(user) &&
          1 == count_if(user->getOperandValues(
                            true /*ignore type dependent operands*/),
                        [&](SILValue v) {
                          return v.getOwnershipKind() ==
                                 ValueOwnershipKind::Owned;
                        })) {
        forwardingInsts.get()->push_back(user);
        for (SILValue v : user->getResults()) {
          if (v.getOwnershipKind() != ValueOwnershipKind::Owned)
            continue;
          llvm::copy(v->getUses(), std::back_inserter(worklist));
        }
        continue;
      }

      // Otherwise be conservative and assume that we /may consume/ the value.
      return true;
    }
    case UseLifetimeConstraint::MustBeLive:
      // Ok, this constraint can take something owned as live. Lets
      // see if it can also take something that is guaranteed. If it
      // can not, then we bail.
      map.canAcceptKind(ValueOwnershipKind::Guaranteed);
      continue;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

namespace {

struct SemanticARCOptVisitor
    : SILInstructionVisitor<SemanticARCOptVisitor, bool> {
  bool visitSILInstruction(SILInstruction *i) { return false; }
  bool visitCopyValueInst(CopyValueInst *cvi);
  bool visitBeginBorrowInst(BeginBorrowInst *bbi);
  bool visitLoadInst(LoadInst *li);
};

} // end anonymous namespace

bool SemanticARCOptVisitor::visitBeginBorrowInst(BeginBorrowInst *bbi) {
  auto kind = bbi->getOperand().getOwnershipKind();
  SmallVector<EndBorrowInst *, 16> endBorrows;
  for (auto *op : bbi->getUses()) {
    auto *user = op->getUser();
    switch (user->getKind()) {
    case SILInstructionKind::EndBorrowInst:
      endBorrows.push_back(cast<EndBorrowInst>(user));
      break;
    default:
      // Make sure that this operand can accept our arguments kind.
      auto map = op->getOwnershipKindMap();
      if (map.canAcceptKind(kind))
        continue;
      return false;
    }
  }

  // At this point, we know that the begin_borrow's operand can be
  // used as an argument to all non-end borrow uses. Eliminate the
  // begin borrow and end borrows.
  while (!endBorrows.empty()) {
    auto *ebi = endBorrows.pop_back_val();
    ebi->eraseFromParent();
    ++NumEliminatedInsts;
  }
  bbi->replaceAllUsesWith(bbi->getOperand());
  bbi->eraseFromParent();
  ++NumEliminatedInsts;
  return true;
}

static bool canHandleOperand(SILValue operand, SmallVectorImpl<SILValue> &out) {
  if (!getUnderlyingBorrowIntroducers(operand, out))
    return false;

  /// TODO: Add support for begin_borrow, load_borrow.
  return all_of(out, [](SILValue v) { return isa<SILFunctionArgument>(v); });
}

// Eliminate a copy of a borrowed value, if:
//
// 1. All of the copies users do not consume the copy (and thus can accept a
//    borrowed value instead).
// 2. The copies's non-destroy_value users are strictly contained within the
//    scope of the borrowed value.
//
// Example:
//
//   %0 = @guaranteed (argument or instruction)
//   %1 = copy_value %0
//   apply %f(%1) : $@convention(thin) (@guaranteed ...) ...
//   other_non_consuming_use %1
//   destroy_value %1
//   end_borrow %0 (if an instruction)
//
// =>
//
//   %0 = @guaranteed (argument or instruction)
//   apply %f(%0) : $@convention(thin) (@guaranteed ...) ...
//   other_non_consuming_use %0
//   end_borrow %0 (if an instruction)
//
// NOTE: This means that the destroy_value technically can be after the
// end_borrow. In practice, this will not be the case but we use this to avoid
// having to reason about the ordering of the end_borrow and destroy_value.
//
// NOTE: Today we only perform this for guaranteed parameters since this enables
// us to avoid doing the linear lifetime check to make sure that all destroys
// are within the borrow scope.
//
// TODO: This needs a better name.
static bool performGuaranteedCopyValueOptimization(CopyValueInst *cvi) {
  SmallVector<SILValue, 16> borrowIntroducers;

  // Whitelist the operands that we know how to support and make sure
  // our operand is actually guaranteed.
  if (!canHandleOperand(cvi->getOperand(), borrowIntroducers))
    return false;

  // Then go over all of our uses. Find our destroying instructions (ignoring
  // forwarding instructions that can forward both owned and guaranteed) and
  // make sure all of them are destroy_value. For our non-destroying
  // instructions, make sure that they accept a guaranteed value. After that,
  // make sure that our destroys are within the lifetime of our borrowed values.
  SmallVector<DestroyValueInst *, 16> destroys;
  SmallVector<SILInstruction *, 16> guaranteedForwardingInsts;
  if (isConsumed(cvi, destroys, &guaranteedForwardingInsts))
    return false;

  // If we reached this point, then we know that all of our users can
  // accept a guaranteed value and our owned value is destroyed only
  // by destroy_value. Check if all of our destroys are joint
  // post-dominated by the end_borrow set. If they do not, then the
  // copy_value is lifetime extending the guaranteed value, we can not
  // eliminate it.
  //
  // TODO: When we support begin_borrow/load_borrow a linear linfetime
  // check will be needed here.
  assert(all_of(borrowIntroducers,
                [](SILValue v) { return isa<SILFunctionArgument>(v); }));

  // Otherwise, we know that our copy_value/destroy_values are all completely
  // within the guaranteed value scope. First delete the destroys/copies.
  while (!destroys.empty()) {
    auto *dvi = destroys.pop_back_val();
    dvi->eraseFromParent();
    ++NumEliminatedInsts;
  }
  cvi->replaceAllUsesWith(cvi->getOperand());
  cvi->eraseFromParent();

  // Then change all of our guaranteed forwarding insts to have guaranteed
  // ownership kind instead of what ever they previously had (ignoring trivial
  // results);
  while (!guaranteedForwardingInsts.empty()) {
    auto *i = guaranteedForwardingInsts.pop_back_val();

    assert(i->hasResults());

    for (SILValue result : i->getResults()) {
      if (auto *svi = dyn_cast<OwnershipForwardingSingleValueInst>(result)) {
        if (svi->getOwnershipKind() == ValueOwnershipKind::Owned) {
          svi->setOwnershipKind(ValueOwnershipKind::Guaranteed);
        }
        continue;
      }

      if (auto *ofci = dyn_cast<OwnershipForwardingConversionInst>(result)) {
        if (ofci->getOwnershipKind() == ValueOwnershipKind::Owned) {
          ofci->setOwnershipKind(ValueOwnershipKind::Guaranteed);
        }
        continue;
      }

      if (auto *sei = dyn_cast<OwnershipForwardingSelectEnumInstBase>(result)) {
        if (sei->getOwnershipKind() == ValueOwnershipKind::Owned) {
          sei->setOwnershipKind(ValueOwnershipKind::Guaranteed);
        }
        continue;
      }

      if (auto *mvir = dyn_cast<MultipleValueInstructionResult>(result)) {
        if (mvir->getOwnershipKind() == ValueOwnershipKind::Owned) {
          mvir->setOwnershipKind(ValueOwnershipKind::Guaranteed);
        }
        continue;
      }

      llvm_unreachable("unhandled forwarding instruction?!");
    }
  }
  ++NumEliminatedInsts;
  return true;
}

/// If cvi only has destroy value users, then cvi is a dead live range. Lets
/// eliminate all such dead live ranges.
static bool eliminateDeadLiveRangeCopyValue(CopyValueInst *cvi) {
  // See if we are lucky and have a simple case.
  if (auto *op = cvi->getSingleUse()) {
    if (auto *dvi = dyn_cast<DestroyValueInst>(op->getUser())) {
      dvi->eraseFromParent();
      cvi->eraseFromParent();
      NumEliminatedInsts += 2;
      return true;
    }
  }

  // If all of our copy_value users are destroy_value, zap all of the
  // instructions. We begin by performing that check and gathering up our
  // destroy_value.
  SmallVector<DestroyValueInst *, 16> destroys;
  if (!all_of(cvi->getUses(), [&](Operand *op) {
        auto *dvi = dyn_cast<DestroyValueInst>(op->getUser());
        if (!dvi)
          return false;

        // Stash dvi in destroys so we can easily eliminate it later.
        destroys.push_back(dvi);
        return true;
      })) {
    return false;
  }

  // Now that we have a truly dead live range copy value, eliminate it!
  while (!destroys.empty()) {
    destroys.pop_back_val()->eraseFromParent();
    ++NumEliminatedInsts;
  }
  cvi->eraseFromParent();
  ++NumEliminatedInsts;
  return true;
}

bool SemanticARCOptVisitor::visitCopyValueInst(CopyValueInst *cvi) {
  // If our copy value inst has only destroy_value users, it is a dead live
  // range. Try to eliminate them.
  if (eliminateDeadLiveRangeCopyValue(cvi))
    return true;

  // Then try to perform the guaranteed copy value optimization.
  if (performGuaranteedCopyValueOptimization(cvi))
    return true;

  return false;
}

//===----------------------------------------------------------------------===//
//                         load [copy] Optimizations
//===----------------------------------------------------------------------===//

// A flow insensitive analysis that tells the load [copy] analysis if the
// storage has 0, 1, >1 writes to it.
//
// In the case of 0 writes, we return CanOptimizeLoadCopyResult::Always.
//
// In the case of 1 write, we return OnlyIfStorageIsLocal. We are taking
// advantage of definite initialization implying that an alloc_stack must be
// written to once before any loads from the memory location. Thus if we are
// local and see 1 write, we can still change to load_borrow if all other uses
// check out.
//
// If there is 2+ writes, we can not optimize = (.

bool mayFunctionMutateArgument(const AccessedStorage &storage, SILFunction &f) {
  auto *arg = cast<SILFunctionArgument>(storage.getArgument());

  // Then check if we have an in_guaranteed argument. In this case, we can
  // always optimize load [copy] from this.
  if (arg->hasConvention(SILArgumentConvention::Indirect_In_Guaranteed))
    return false;

  // For now just return false.
  return true;
}

static bool isWrittenTo(SILFunction &f, SILValue value) {
  // Then find our accessed storage. If we can not find anything, be
  // conservative and assume that the value is written to.
  const auto &storage = findAccessedStorageNonNested(value);
  if (!storage)
    return false;

  // Then see if we ever write to this address in a flow insensitive
  // way (ignoring stores that are obviously the only initializer to
  // memory). We have to do this since load_borrow assumes that the
  // underlying memory is never written to.
  switch (storage.getKind()) {
  case AccessedStorage::Box:
  case AccessedStorage::Stack:
  case AccessedStorage::Global:
  case AccessedStorage::Class:
  case AccessedStorage::Yield:
  case AccessedStorage::Nested:
  case AccessedStorage::Unidentified:
    return true;

  case AccessedStorage::Argument:
    return mayFunctionMutateArgument(storage, f);
  }
  llvm_unreachable("covered switch");
}

// Convert a load [copy] from unique storage [read] that has all uses that can
// accept a guaranteed parameter to a load_borrow.
bool SemanticARCOptVisitor::visitLoadInst(LoadInst *li) {
  if (li->getOwnershipQualifier() != LoadOwnershipQualifier::Copy)
    return false;

  // Ok, we have our load [copy]. Make sure its value is never
  // consumed. If it is consumed, we need to pass off a +1 value, so
  // bail.
  //
  // FIXME: We should consider if it is worth promoting a load [copy]
  // -> load_borrow if we can put a copy_value on a cold path and thus
  // eliminate RR traffic on a hot path.
  SmallVector<DestroyValueInst *, 32> destroyValues;
  if (isConsumed(li, destroyValues))
    return false;

  // Then check if our address is ever written to. If it is, then we
  // can not use the load_borrow.
  if (isWrittenTo(*li->getFunction(), li->getOperand()))
    return false;

  // Ok, we can perform our optimization. Convert the load [copy] into a
  // load_borrow.
  auto *lbi =
      SILBuilderWithScope(li).createLoadBorrow(li->getLoc(), li->getOperand());

  // Since we are looking through forwarding uses that can accept guaranteed
  // parameters, we can have multiple destroy_value along the same path. We need
  // to find the post-dominating block set of these destroy value to ensure that
  // we do not insert multiple end_borrow.
  while (!destroyValues.empty()) {
    auto *dvi = destroyValues.pop_back_val();
    SILBuilderWithScope(dvi).createEndBorrow(dvi->getLoc(), lbi);
    dvi->eraseFromParent();
    ++NumEliminatedInsts;
  }

  li->replaceAllUsesWith(lbi);
  li->eraseFromParent();
  ++NumEliminatedInsts;
  ++NumLoadCopyConvertedToLoadBorrow;
  return true;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

// Even though this is a mandatory pass, it is rerun after deserialization in
// case DiagnosticConstantPropagation exposed anything new in this assert
// configuration.
struct SemanticARCOpts : SILFunctionTransform {
  void run() override {
    SILFunction &f = *getFunction();

    // Make sure we are running with ownership verification enabled.
    assert(f.getModule().getOptions().VerifySILOwnership &&
           "Can not perform semantic arc optimization unless ownership "
           "verification is enabled");

    // Iterate over all of the arguments, performing small peephole
    // ARC optimizations.
    //
    // FIXME: Should we iterate or use a RPOT order here?
    bool madeChange = false;
    for (auto &bb : f) {
      auto ii = bb.rend();
      auto start = bb.rbegin();

      // If the bb is empty, continue.
      if (start == ii)
        continue;

      // Go to the first instruction to process.
      --ii;

      // Then until we process the first instruction of the block...
      while (ii != start) {
        // Move the iterator before ii.
        auto tmp = std::next(ii);

        // Then try to optimize. If we succeeded, then we deleted
        // ii. Move ii from the next value back onto the instruction
        // after ii's old value in the block instruction list and then
        // process that.
        if (SemanticARCOptVisitor().visit(&*ii)) {
          madeChange = true;
          ii = std::prev(tmp);
          continue;
        }

        // Otherwise, we didn't delete ii. Just visit the next instruction.
        --ii;
      }

      // Finally visit the first instruction of the block.
      madeChange |= SemanticARCOptVisitor().visit(&*ii);
    }

    if (madeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createSemanticARCOpts() { return new SemanticARCOpts(); }
