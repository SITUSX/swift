//===--- ResultPlan.cpp ---------------------------------------------------===//
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

#include "ResultPlan.h"
#include "Initialization.h"
#include "RValue.h"
#include "SILGenFunction.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                                Result Plans
//===----------------------------------------------------------------------===//

namespace {

/// A result plan for evaluating an indirect result into the address
/// associated with an initialization.
class InPlaceInitializationResultPlan : public ResultPlan {
  Initialization *init;

public:
  InPlaceInitializationResultPlan(Initialization *init) : init(init) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    init->finishInitialization(SGF);
    return RValue();
  }
};

/// A result plan for working with a single value and potentially
/// reabstracting it.  The value can actually be a tuple if the
/// abstraction is opaque.
class ScalarResultPlan : public ResultPlan {
  std::unique_ptr<TemporaryInitialization> temporary;
  AbstractionPattern origType;
  Initialization *init;
  SILFunctionTypeRepresentation rep;

public:
  ScalarResultPlan(std::unique_ptr<TemporaryInitialization> &&temporary,
                   AbstractionPattern origType, Initialization *init,
                   SILFunctionTypeRepresentation rep)
      : temporary(std::move(temporary)), origType(origType), init(init),
        rep(rep) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    // Lower the unabstracted result type.
    auto &substTL = SGF.getTypeLowering(substType);

    // Claim the value:
    ManagedValue value;

    // If we were created with a temporary, that address was passed as
    // an indirect result.
    if (temporary) {
      // Establish the cleanup.
      temporary->finishInitialization(SGF);
      value = temporary->getManagedAddress();

      // If the value isn't address-only, go ahead and load.
      if (!substTL.isAddressOnly()) {
        auto load = substTL.emitLoad(SGF.B, loc, value.forward(SGF),
                                     LoadOwnershipQualifier::Take);
        value = SGF.emitManagedRValueWithCleanup(load);
      }

      // Otherwise, it was returned as a direct result.
    } else {
      value = directResults.front();
      directResults = directResults.slice(1);
    }

    // Reabstract the value if the types don't match.  This can happen
    // due to either substitution reabstractions or bridging.
    if (value.getType().hasAbstractionDifference(rep,
                                                 substTL.getLoweredType())) {
      // Assume that a C-language API doesn't have substitution
      // reabstractions.  This shouldn't be necessary, but
      // emitOrigToSubstValue can get upset.
      if (getSILFunctionLanguage(rep) == SILFunctionLanguage::C) {
        value = SGF.emitBridgedToNativeValue(loc, value, rep, substType);

      } else {
        value = SGF.emitOrigToSubstValue(loc, value, origType, substType,
                                         SGFContext(init));

        // If that successfully emitted into the initialization, we're done.
        if (value.isInContext())
          return RValue();
      }
    }

    // Otherwise, forcibly emit into the initialization if it exists.
    if (init) {
      init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
      init->finishInitialization(SGF);
      return RValue();

      // Otherwise, we've got the r-value we want.
    } else {
      return RValue(SGF, loc, substType, value);
    }
  }
};

/// A result plan which calls copyOrInitValueInto on an Initialization
/// using a temporary buffer initialized by a sub-plan.
class InitValueFromTemporaryResultPlan : public ResultPlan {
  Initialization *init;
  ResultPlanPtr subPlan;
  std::unique_ptr<TemporaryInitialization> temporary;

public:
  InitValueFromTemporaryResultPlan(
      Initialization *init, ResultPlanPtr &&subPlan,
      std::unique_ptr<TemporaryInitialization> &&temporary)
      : init(init), subPlan(std::move(subPlan)),
        temporary(std::move(temporary)) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    RValue subResult = subPlan->finish(SGF, loc, substType, directResults);
    assert(subResult.isUsed() && "sub-plan didn't emit into context?");
    (void)subResult;

    ManagedValue value = temporary->getManagedAddress();
    init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
    init->finishInitialization(SGF);

    return RValue();
  }
};

/// A result plan which calls copyOrInitValueInto using the result of
/// a sub-plan.
class InitValueFromRValueResultPlan : public ResultPlan {
  Initialization *init;
  ResultPlanPtr subPlan;

public:
  InitValueFromRValueResultPlan(Initialization *init, ResultPlanPtr &&subPlan)
      : init(init), subPlan(std::move(subPlan)) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    RValue subResult = subPlan->finish(SGF, loc, substType, directResults);
    ManagedValue value = std::move(subResult).getAsSingleValue(SGF, loc);

    init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
    init->finishInitialization(SGF);

    return RValue();
  }
};

/// A result plan which produces a larger RValue from a bunch of
/// components.
class TupleRValueResultPlan : public ResultPlan {
  SmallVector<ResultPlanPtr, 4> eltPlans;

public:
  TupleRValueResultPlan(ResultPlanBuilder &builder, AbstractionPattern origType,
                        CanTupleType substType) {
    // Create plans for all the elements.
    eltPlans.reserve(substType->getNumElements());
    for (auto i : indices(substType->getElementTypes())) {
      AbstractionPattern origEltType = origType.getTupleElementType(i);
      CanType substEltType = substType.getElementType(i);
      eltPlans.push_back(builder.build(nullptr, origEltType, substEltType));
    }
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    RValue tupleRV(substType);

    // Finish all the component tuples.
    auto substTupleType = cast<TupleType>(substType);
    assert(substTupleType.getElementTypes().size() == eltPlans.size());
    for (auto i : indices(substTupleType.getElementTypes())) {
      RValue eltRV = eltPlans[i]->finish(
          SGF, loc, substTupleType.getElementType(i), directResults);
      tupleRV.addElement(std::move(eltRV));
    }

    return tupleRV;
  }
};

/// A result plan which evaluates into the sub-components
/// of a splittable tuple initialization.
class TupleInitializationResultPlan : public ResultPlan {
  Initialization *tupleInit;
  SmallVector<InitializationPtr, 4> eltInitsBuffer;
  MutableArrayRef<InitializationPtr> eltInits;
  SmallVector<ResultPlanPtr, 4> eltPlans;

public:
  TupleInitializationResultPlan(ResultPlanBuilder &builder,
                                Initialization *tupleInit,
                                AbstractionPattern origType,
                                CanTupleType substType)
      : tupleInit(tupleInit) {

    // Get the sub-initializations.
    eltInits = tupleInit->splitIntoTupleElements(builder.SGF, builder.loc,
                                                 substType, eltInitsBuffer);

    // Create plans for all the sub-initializations.
    eltPlans.reserve(substType->getNumElements());
    for (auto i : indices(substType->getElementTypes())) {
      AbstractionPattern origEltType = origType.getTupleElementType(i);
      CanType substEltType = substType.getElementType(i);
      Initialization *eltInit = eltInits[i].get();
      eltPlans.push_back(builder.build(eltInit, origEltType, substEltType));
    }
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                ArrayRef<ManagedValue> &directResults) override {
    auto substTupleType = cast<TupleType>(substType);
    assert(substTupleType.getElementTypes().size() == eltPlans.size());
    for (auto i : indices(substTupleType.getElementTypes())) {
      auto eltType = substTupleType.getElementType(i);
      RValue eltRV = eltPlans[i]->finish(SGF, loc, eltType, directResults);
      assert(eltRV.isUsed());
      (void)eltRV;
    }
    tupleInit->finishInitialization(SGF);

    return RValue();
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                            Result Plan Builder
//===----------------------------------------------------------------------===//

/// Build a result plan for the results of an apply.
///
/// If the initialization is non-null, the result plan will emit into it.
ResultPlanPtr ResultPlanBuilder::build(Initialization *init,
                                       AbstractionPattern origType,
                                       CanType substType) {
  // Destructure original tuples.
  if (origType.isTuple()) {
    return buildForTuple(init, origType, cast<TupleType>(substType));
  }

  // Otherwise, grab the next result.
  auto result = allResults.front();
  allResults = allResults.slice(1);

  SILValue initAddr;
  if (init) {
    initAddr = init->getAddressForInPlaceInitialization();

    // If the result is indirect, and we have an address to emit into, and
    // there are no abstraction differences, then just do it.
    if (initAddr && SGF.silConv.isSILIndirect(result) &&
        !initAddr->getType().hasAbstractionDifference(
            rep, result.getSILStorageType())) {
      indirectResultAddrs.push_back(initAddr);
      return ResultPlanPtr(new InPlaceInitializationResultPlan(init));
    }
  }

  // Otherwise, we need to:
  //   - get the value, either directly or indirectly
  //   - possibly reabstract it
  //   - store it to the destination
  // We could break this down into different ResultPlan implementations,
  // but it's easier not to.

  // Create a temporary if the result is indirect.
  std::unique_ptr<TemporaryInitialization> temporary;
  if (SGF.silConv.isSILIndirect(result)) {
    auto &resultTL = SGF.getTypeLowering(result.getType());
    temporary = SGF.emitTemporary(loc, resultTL);
    indirectResultAddrs.push_back(temporary->getAddress());
  }

  return ResultPlanPtr(
      new ScalarResultPlan(std::move(temporary), origType, init, rep));
}

ResultPlanPtr ResultPlanBuilder::buildForTuple(Initialization *init,
                                               AbstractionPattern origType,
                                               CanTupleType substType) {
  // If we don't have an initialization for the tuple, just build the
  // individual components.
  if (!init) {
    return ResultPlanPtr(new TupleRValueResultPlan(*this, origType, substType));
  }

  // Okay, we have an initialization for the tuple that we need to emit into.

  // If we can just split the initialization, do so.
  if (init->canSplitIntoTupleElements()) {
    return ResultPlanPtr(
        new TupleInitializationResultPlan(*this, init, origType, substType));
  }

  // Otherwise, we're going to have to call copyOrInitValueInto, which only
  // takes a single value.

  // If the tuple is address-only, we'll get much better code if we
  // emit into a single buffer.
  auto &substTL = SGF.getTypeLowering(substType);
  if (substTL.isAddressOnly()) {
    // Create a temporary.
    auto temporary = SGF.emitTemporary(loc, substTL);

    // Build a sub-plan to emit into the temporary.
    auto subplan = buildForTuple(temporary.get(), origType, substType);

    // Make a plan to initialize into that.
    return ResultPlanPtr(new InitValueFromTemporaryResultPlan(
        init, std::move(subplan), std::move(temporary)));
  }

  // Build a sub-plan that doesn't know about the initialization.
  auto subplan = buildForTuple(nullptr, origType, substType);

  // Make a plan that calls copyOrInitValueInto.
  return ResultPlanPtr(
      new InitValueFromRValueResultPlan(init, std::move(subplan)));
}