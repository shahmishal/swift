//===--- ConstraintLocator.h - Constraint Locator ---------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides the \c ConstraintLocator class and its related types,
// which is used by the constraint-based type checker to describe how
// a particular constraint was derived.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_CONSTRAINTLOCATOR_H
#define SWIFT_SEMA_CONSTRAINTLOCATOR_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ErrorHandling.h"
#include <utility>

namespace swift {

class Expr;
class SourceManager;

namespace constraints {
  class ConstraintSystem;

/// Locates a given constraint within the expression being
/// type-checked, which may refer down into subexpressions and parts of
/// the types of those subexpressions.
///
/// Each locator as anchored at some expression, e.g., (3, (x, 3.14)),
/// and contains a path that digs further into the type of that expression.
/// For example, the path "tuple element #1" -> "tuple element #0" with the
/// above expression would refer to 'x'. If 'x' had function type, the
/// path could be further extended with either "-> argument" or "-> result",
/// to indicate constraints on its argument or result type.
class ConstraintLocator : public llvm::FoldingSetNode {
public:
  /// Describes the kind of a particular path element, e.g.,
  /// "tuple element", "call result", "base of member lookup", etc.
  enum PathElementKind : unsigned char {
    /// The argument of function application.
    ApplyArgument,
    /// The function being applied.
    ApplyFunction,
    /// Matching an argument to a parameter.
    ApplyArgToParam,
    /// A generic parameter being opened.
    ///
    /// Also contains the generic parameter type itself.
    GenericParameter,
    /// The argument type of a function.
    FunctionArgument,
    /// The default argument type of a function.
    DefaultArgument,
    /// The result type of a function.
    FunctionResult,
    /// A tuple element referenced by position.
    TupleElement,
    /// A tuple element referenced by name.
    NamedTupleElement,
    /// An optional payload.
    OptionalPayload,
    /// A generic argument.
    /// FIXME: Add support for named generic arguments?
    GenericArgument,
    /// A member.
    /// FIXME: Do we need the actual member name here?
    Member,
    /// An unresolved member.
    UnresolvedMember,
    /// The base of a member expression.
    MemberRefBase,
    /// The lookup for a subscript member.
    SubscriptMember,
    /// The lookup for a constructor member.
    ConstructorMember,
    /// An implicit @lvalue-to-inout conversion; only valid for operator
    /// arguments.
    LValueConversion,
    /// RValue adjustment.
    RValueAdjustment,
    /// The result of a closure.
    ClosureResult,
    /// The parent of a nested type.
    ParentType,
    /// The superclass of a protocol existential type.
    ExistentialSuperclassType,
    /// The instance of a metatype type.
    InstanceType,
    /// The element type of a sequence in a for ... in ... loop.
    SequenceElementType,
    /// An argument passed in an autoclosure parameter
    /// position, which must match the autoclosure return type.
    AutoclosureResult,
    /// The requirement that we're matching during protocol conformance
    /// checking.
    Requirement,
    /// The candidate witness during protocol conformance checking.
    Witness,
    /// This is referring to a type produced by opening a generic type at the
    /// base of the locator.
    OpenedGeneric,
    /// A component of a key path.
    KeyPathComponent,
    /// The Nth conditional requirement in the parent locator's conformance.
    ConditionalRequirement,
    /// A single requirement placed on the type parameters.
    TypeParameterRequirement,
    /// Locator for a binding from an IUO disjunction choice.
    ImplicitlyUnwrappedDisjunctionChoice,
    /// A result of an expression involving dynamic lookup.
    DynamicLookupResult,
    /// The desired contextual type passed in to the constraint system.
    ContextualType,
    /// The missing argument synthesized by the solver.
    SynthesizedArgument,
    /// The member looked up via keypath based dynamic lookup.
    KeyPathDynamicMember,
    /// The type of the key path expression
    KeyPathType,
    /// The root of a key path
    KeyPathRoot,
    /// The value of a key path
    KeyPathValue,
    /// The result type of a key path component. Not used for subscripts.
    KeyPathComponentResult,
  };

  /// Determine the number of numeric values used for the given path
  /// element kind.
  static unsigned numNumericValuesInPathElement(PathElementKind kind) {
    switch (kind) {
    case ApplyArgument:
    case ApplyFunction:
    case GenericParameter:
    case FunctionArgument:
    case DefaultArgument:
    case FunctionResult:
    case OptionalPayload:
    case Member:
    case MemberRefBase:
    case UnresolvedMember:
    case SubscriptMember:
    case ConstructorMember:
    case LValueConversion:
    case RValueAdjustment:
    case ClosureResult:
    case ParentType:
    case InstanceType:
    case ExistentialSuperclassType:
    case SequenceElementType:
    case AutoclosureResult:
    case Requirement:
    case Witness:
    case ImplicitlyUnwrappedDisjunctionChoice:
    case DynamicLookupResult:
    case KeyPathType:
    case KeyPathRoot:
    case KeyPathValue:
    case KeyPathComponentResult:
      return 0;

    case ContextualType:
    case OpenedGeneric:
    case GenericArgument:
    case NamedTupleElement:
    case TupleElement:
    case KeyPathComponent:
    case SynthesizedArgument:
    case KeyPathDynamicMember:
      return 1;

    case TypeParameterRequirement:
    case ConditionalRequirement:
    case ApplyArgToParam:
      return 2;
    }

    llvm_unreachable("Unhandled PathElementKind in switch.");
  }

  /// Flags for efficiently recording certain information about a path.
  /// All of this information should be re-derivable from the path.
  ///
  /// Values are chosen so that an empty path has value 0 and the
  /// flags for a concatenated paths is simply the bitwise-or of the
  /// flags of the component paths.
  enum Flag : unsigned {
    /// Does this path involve a function conversion, i.e. a
    /// FunctionArgument or FunctionResult node?
    IsFunctionConversion = 0x1,
  };

  static unsigned getSummaryFlagsForPathElement(PathElementKind kind) {
    switch (kind) {
    case ApplyArgument:
    case ApplyFunction:
    case ApplyArgToParam:
    case SequenceElementType:
    case ClosureResult:
    case ConstructorMember:
    case InstanceType:
    case AutoclosureResult:
    case OptionalPayload:
    case Member:
    case MemberRefBase:
    case UnresolvedMember:
    case ParentType:
    case ExistentialSuperclassType:
    case LValueConversion:
    case RValueAdjustment:
    case SubscriptMember:
    case OpenedGeneric:
    case GenericParameter:
    case GenericArgument:
    case NamedTupleElement:
    case TupleElement:
    case Requirement:
    case Witness:
    case KeyPathComponent:
    case ConditionalRequirement:
    case TypeParameterRequirement:
    case ImplicitlyUnwrappedDisjunctionChoice:
    case DynamicLookupResult:
    case ContextualType:
    case SynthesizedArgument:
    case KeyPathDynamicMember:
    case KeyPathType:
    case KeyPathRoot:
    case KeyPathValue:
    case KeyPathComponentResult:
      return 0;

    case FunctionArgument:
    case DefaultArgument:
    case FunctionResult:
      return IsFunctionConversion;
    }

    llvm_unreachable("Unhandled PathElementKind in switch.");
  }

  /// One element in the path of a locator, which can include both
  /// a kind (PathElementKind) and a value used to describe specific
  /// kinds further (e.g., the position of a tuple element).
  class PathElement {
    /// Describes the kind of data stored here.
    enum StoredKind : unsigned char {
      StoredGenericParameter,
      StoredRequirement,
      StoredWitness,
      StoredGenericSignature,
      StoredKeyPathDynamicMemberBase,
      StoredKindAndValue
    };

    /// The actual storage for the path element, which involves both a
    /// kind and (potentially) a value.
    ///
    /// The current storage involves a two-bit "storage kind", which selects
    /// among the possible value stores. The value stores can either be an
    /// archetype (for archetype path elements) or an unsigned value that
    /// stores both the specific kind and the (optional) numeric value of that
    /// kind. Use \c encodeStorage and \c decodeStorage to work with this value.
    ///
    /// \note The "storage kind" is stored in the  \c storedKind field.
    uint64_t storage : 61;

    /// The kind of value stored in \c storage. Valid values are those
    /// from the StoredKind enum.
    uint64_t storedKind : 3;

    /// Encode a path element kind and a value into the storage format.
    static uint64_t encodeStorage(PathElementKind kind, unsigned value) {
      return ((uint64_t)value << 8) | kind;
    }

    /// Decode a storage value into path element kind and value.
    static std::pair<PathElementKind, unsigned>
    decodeStorage(uint64_t storage) {
      return { (PathElementKind)((unsigned)storage & 0xFF), storage >> 8 };
    }

    PathElement(PathElementKind kind, unsigned value)
      : storage(encodeStorage(kind, value)), storedKind(StoredKindAndValue)
    {
      assert(numNumericValuesInPathElement(kind) == 1 &&
             "Path element kind does not require 1 value");
    }

    PathElement(PathElementKind kind, unsigned value1, unsigned value2)
      : storage(encodeStorage(kind, value1 << 16 | value2)),
        storedKind(StoredKindAndValue)
    {
      assert(numNumericValuesInPathElement(kind) == 2 &&
             "Path element kind does not require 2 values");
    }

    PathElement(GenericSignature *sig)
        : storage((reinterpret_cast<uintptr_t>(sig) >> 3)),
          storedKind(StoredGenericSignature) {}

    PathElement(const NominalTypeDecl *keyPath)
        : storage((reinterpret_cast<uintptr_t>(keyPath) >> 3)),
          storedKind(StoredKeyPathDynamicMemberBase) {}

    friend class ConstraintLocator;

  public:
    PathElement(PathElementKind kind)
      : storage(encodeStorage(kind, 0)), storedKind(StoredKindAndValue)
    {
      assert(numNumericValuesInPathElement(kind) == 0 &&
             "Path element requires value");
    }

    PathElement(GenericTypeParamType *type)
      : storage((reinterpret_cast<uintptr_t>(type) >> 3)),
        storedKind(StoredGenericParameter)
    {
      static_assert(alignof(GenericTypeParamType) >= 4,
                    "archetypes insufficiently aligned");
      assert(getGenericParameter() == type);
    }

    PathElement(PathElementKind kind, ValueDecl *decl)
      : storage((reinterpret_cast<uintptr_t>(decl) >> 3)),
        storedKind(kind == Witness ? StoredWitness : StoredRequirement)
    {
      assert((kind == Witness || kind == Requirement) &&
             "Not a witness element");
      assert(((kind == Requirement && getRequirement() == decl) ||
              (kind == Witness && getWitness() == decl)));
    }

    /// Retrieve a path element for a tuple element referred to by
    /// its position.
    static PathElement getTupleElement(unsigned position) {
      return PathElement(TupleElement, position);
    }

    /// Retrieve a path element for a tuple element referred to by
    /// its name.
    static PathElement getNamedTupleElement(unsigned position) {
      return PathElement(NamedTupleElement, position);
    }

    /// Retrieve a path element for an argument/parameter comparison in a
    /// function application.
    static PathElement getApplyArgToParam(unsigned argIdx, unsigned paramIdx) {
      return PathElement(ApplyArgToParam, argIdx, paramIdx);
    }

    /// Retrieve a path element for a generic argument referred to by
    /// its position.
    static PathElement getGenericArgument(unsigned position) {
      return PathElement(GenericArgument, position);
    }
    
    /// Get a path element for a key path component.
    static PathElement getKeyPathComponent(unsigned position) {
      return PathElement(KeyPathComponent, position);
    }

    static PathElement getOpenedGeneric(GenericSignature *sig) {
      return PathElement(sig);
    }

    /// Get a path element for a conditional requirement.
    static PathElement
    getConditionalRequirementComponent(unsigned index, RequirementKind kind) {
      return PathElement(ConditionalRequirement, index,
                         static_cast<unsigned>(kind));
    }

    static PathElement getTypeRequirementComponent(unsigned index,
                                                   RequirementKind kind) {
      return PathElement(TypeParameterRequirement, index,
                         static_cast<unsigned>(kind));
    }

    static PathElement getSynthesizedArgument(unsigned position) {
      return PathElement(SynthesizedArgument, position);
    }

    static PathElement getKeyPathDynamicMember(const NominalTypeDecl *base) {
      return PathElement(base);
    }

    static PathElement getContextualType(bool isForSingleExprFunction = false) {
      return PathElement(ContextualType, isForSingleExprFunction);
    }

    /// Retrieve the kind of path element.
    PathElementKind getKind() const {
      switch (static_cast<StoredKind>(storedKind)) {
      case StoredGenericParameter:
        return GenericParameter;

      case StoredRequirement:
        return Requirement;

      case StoredWitness:
        return Witness;

      case StoredGenericSignature:
        return OpenedGeneric;

      case StoredKeyPathDynamicMemberBase:
        return KeyPathDynamicMember;

      case StoredKindAndValue:
        return decodeStorage(storage).first;
      }

      llvm_unreachable("Unhandled StoredKind in switch.");
    }

    /// Retrieve the value associated with this path element,
    /// if it has one.
    unsigned getValue() const {
      unsigned numValues = numNumericValuesInPathElement(getKind());
      assert(numValues > 0 && "No value in path element!");

      auto value = decodeStorage(storage).second;
      if (numValues == 1) {
        return value;
      }

      return value >> 16;
    }

    /// Retrieve the second value associated with this path element,
    /// if it has one.
    unsigned getValue2() const {
      unsigned numValues = numNumericValuesInPathElement(getKind());
      (void)numValues;
      assert(numValues == 2 && "No second value in path element!");

      auto value = decodeStorage(storage).second;
      return value & 0x00FFFF;
    }

    /// Retrieve the declaration for a witness path element.
    ValueDecl *getWitness() const {
      assert(getKind() == Witness && "Is not a witness");
      return reinterpret_cast<ValueDecl *>(storage << 3);
    }

    /// Retrieve the actual archetype for a generic parameter path
    /// element.
    GenericTypeParamType *getGenericParameter() const {
      assert(getKind() == GenericParameter &&
             "Not a generic parameter path element");
      return reinterpret_cast<GenericTypeParamType *>(storage << 3);
    }

    /// Retrieve the declaration for a requirement path element.
    ValueDecl *getRequirement() const {
      assert((static_cast<StoredKind>(storedKind) == StoredRequirement) &&
             "Is not a requirement");
      return reinterpret_cast<ValueDecl *>(storage << 3);
    }

    GenericSignature *getGenericSignature() const {
      assert((static_cast<StoredKind>(storedKind) == StoredGenericSignature) &&
             "Is not an opened generic");
      return reinterpret_cast<GenericSignature *>(storage << 3);
    }

    NominalTypeDecl *getKeyPath() const {
      assert((static_cast<StoredKind>(storedKind) ==
              StoredKeyPathDynamicMemberBase) &&
             "Is not a keypath dynamic member");
      return reinterpret_cast<NominalTypeDecl *>(storage << 3);
    }

    /// Return the summary flags for this particular element.
    unsigned getNewSummaryFlags() const {
      return getSummaryFlagsForPathElement(getKind());
    }

    bool isTypeParameterRequirement() const {
      return getKind() == PathElementKind::TypeParameterRequirement;
    }

    bool isConditionalRequirement() const {
      return getKind() == PathElementKind::ConditionalRequirement;
    }

    bool isSynthesizedArgument() const {
      return getKind() == PathElementKind::SynthesizedArgument;
    }

    bool isKeyPathDynamicMember() const {
      return getKind() == PathElementKind::KeyPathDynamicMember;
    }

    bool isKeyPathComponent() const {
      return getKind() == PathElementKind::KeyPathComponent;
    }

    bool isClosureResult() const {
      return getKind() == PathElementKind::ClosureResult;
    }

    /// Determine whether this element points to the contextual type
    /// associated with result of a single expression function.
    bool isResultOfSingleExprFunction() const {
      return getKind() == PathElementKind::ContextualType ? bool(getValue())
                                                          : false;
    }
  };

  /// Return the summary flags for an entire path.
  static unsigned getSummaryFlagsForPath(ArrayRef<PathElement> path) {
    unsigned flags = 0;
    for (auto &elt : path) flags |= elt.getNewSummaryFlags();
    return flags;
  }

  /// Retrieve the expression that anchors this locator.
  Expr *getAnchor() const { return anchor; }
  
  /// Retrieve the path that extends from the anchor to a specific
  /// subcomponent.
  ArrayRef<PathElement> getPath() const {
    // FIXME: Alignment.
    return llvm::makeArrayRef(reinterpret_cast<const PathElement *>(this + 1),
                              numPathElements);
  }

  unsigned getSummaryFlags() const { return summaryFlags; }

  /// Determines whether this locator is part of a function
  /// conversion.
  bool isFunctionConversion() const {
    return (getSummaryFlags() & IsFunctionConversion);
  }

  /// Determine whether given locator points to the subscript reference
  /// e.g. `foo[0]` or `\Foo.[0]`
  bool isSubscriptMemberRef() const;

  /// Determine whether give locator points to the type of the
  /// key path expression.
  bool isKeyPathType() const;

  /// Determine whether given locator points to the keypath root
  bool isKeyPathRoot() const;

  /// Determine whether given locator points to the keypath value
  bool isKeyPathValue() const;
  
  /// Determine whether given locator points to the choice picked as
  /// as result of the key path dynamic member lookup operation.
  bool isResultOfKeyPathDynamicMemberLookup() const;

  /// Determine whether this locator points to a subscript component
  /// of the key path at some index.
  bool isKeyPathSubscriptComponent() const;

  /// Determine whether this locator points to the member found
  /// via key path dynamic member lookup.
  bool isForKeyPathDynamicMemberLookup() const;

  /// Determine whether this locator points to one of the key path
  /// components.
  bool isForKeyPathComponent() const;

  /// Determine whether this locator points to the generic parameter.
  bool isForGenericParameter() const;

  /// Determine whether this locator points to the element type of a
  /// sequence in a for ... in ... loop.
  bool isForSequenceElementType() const;

  /// Determine whether this locator points to the contextual type.
  bool isForContextualType() const;

  /// Check whether the last element in the path of this locator
  /// is of a given kind.
  bool isLastElement(ConstraintLocator::PathElementKind kind) const;

  /// If this locator points to generic parameter return its type.
  GenericTypeParamType *getGenericParameter() const;

  /// Produce a profile of this locator, for use in a folding set.
  static void Profile(llvm::FoldingSetNodeID &id, Expr *anchor,
                      ArrayRef<PathElement> path);
  
  /// Produce a profile of this locator, for use in a folding set.
  void Profile(llvm::FoldingSetNodeID &id) {
    Profile(id, anchor, getPath());
  }
  
  /// Produce a debugging dump of this locator.
  LLVM_ATTRIBUTE_DEPRECATED(
      void dump(SourceManager *SM) LLVM_ATTRIBUTE_USED,
      "only for use within the debugger");
  LLVM_ATTRIBUTE_DEPRECATED(
      void dump(ConstraintSystem *CS) LLVM_ATTRIBUTE_USED,
      "only for use within the debugger");

  void dump(SourceManager *SM, raw_ostream &OS) LLVM_ATTRIBUTE_USED;

private:
  /// Initialize a constraint locator with an anchor and a path.
  ConstraintLocator(Expr *anchor, ArrayRef<PathElement> path,
                    unsigned flags)
    : anchor(anchor), numPathElements(path.size()), summaryFlags(flags)
  {
    // FIXME: Alignment.
    std::copy(path.begin(), path.end(),
              reinterpret_cast<PathElement *>(this + 1));
  }

  /// Create a new locator from an anchor and an array of path
  /// elements.
  ///
  /// Note that this routine only handles the allocation and initialization
  /// of the locator. The ConstraintSystem object is responsible for
  /// uniquing via the FoldingSet.
  static ConstraintLocator *create(llvm::BumpPtrAllocator &allocator,
                                   Expr *anchor,
                                   ArrayRef<PathElement> path,
                                   unsigned flags) {
    // FIXME: Alignment.
    unsigned size = sizeof(ConstraintLocator)
                  + path.size() * sizeof(PathElement);
    void *mem = allocator.Allocate(size, alignof(ConstraintLocator));
    return new (mem) ConstraintLocator(anchor, path, flags);
  }

  /// The expression at which this locator is anchored.
  Expr *anchor;

  /// The number of path elements in this locator.
  ///
  /// The actual path elements are stored after the locator.
  unsigned numPathElements : 24;

  /// A set of flags summarizing interesting properties of the path.
  unsigned summaryFlags : 7;
  
  friend class ConstraintSystem;
};

using LocatorPathElt = ConstraintLocator::PathElement;

/// A simple stack-only builder object that constructs a
/// constraint locator without allocating memory.
///
/// Use this object to build a path when passing components down the
/// stack, e.g., when recursively breaking apart types as in \c matchTypes().
class ConstraintLocatorBuilder {
  /// The constraint locator that this builder extends or the
  /// previous builder in the chain.
  llvm::PointerUnion<ConstraintLocator *, ConstraintLocatorBuilder *>
    previous;

  /// The current path element, if there is one.
  Optional<LocatorPathElt> element;

  /// The current set of flags.
  unsigned summaryFlags;

  ConstraintLocatorBuilder(llvm::PointerUnion<ConstraintLocator *,
                                              ConstraintLocatorBuilder *>
                             previous,
                           LocatorPathElt element,
                           unsigned flags)
    : previous(previous), element(element), summaryFlags(flags) { }

public:
  ConstraintLocatorBuilder(ConstraintLocator *locator)
    : previous(locator), element(),
      summaryFlags(locator ? locator->getSummaryFlags() : 0) { }

  /// Retrieve a new path with the given path element added to it.
  ConstraintLocatorBuilder withPathElement(LocatorPathElt newElt) {
    unsigned newFlags = summaryFlags | newElt.getNewSummaryFlags();
    if (!element)
      return ConstraintLocatorBuilder(previous, newElt, newFlags);

    return ConstraintLocatorBuilder(this, newElt, newFlags);
  }

  /// Determine whether this builder has an empty path.
  bool hasEmptyPath() const {
    return !element;
  }

  /// Return the set of flags that summarize this path.
  unsigned getSummaryFlags() const {
    return summaryFlags;
  }

  bool isFunctionConversion() const {
    return (getSummaryFlags() & ConstraintLocator::IsFunctionConversion);
  }

  /// Retrieve the base constraint locator, on which this builder's
  /// path is based.
  ConstraintLocator *getBaseLocator() const {
    for (auto prev = this;
         prev;
         prev = prev->previous.dyn_cast<ConstraintLocatorBuilder *>()) {
      if (auto locator = prev->previous.dyn_cast<ConstraintLocator *>())
        return locator;
    }

    return nullptr;
  }

  /// Get anchor expression associated with this locator builder.
  Expr *getAnchor() const {
    for (auto prev = this; prev;
         prev = prev->previous.dyn_cast<ConstraintLocatorBuilder *>()) {
      if (auto *locator = prev->previous.dyn_cast<ConstraintLocator *>())
        return locator->getAnchor();
    }

    return nullptr;
  }

  /// Retrieve the components of the complete locator, which includes
  /// the anchor expression and the path.
  Expr *getLocatorParts(SmallVectorImpl<LocatorPathElt> &path) const {
    for (auto prev = this;
         prev;
         prev = prev->previous.dyn_cast<ConstraintLocatorBuilder *>()) {
      // If there is an element at this level, add it.
      if (prev->element)
        path.push_back(*prev->element);

      if (auto locator = prev->previous.dyn_cast<ConstraintLocator *>()) {
        // We found the end of the chain. Reverse the path we've built up,
        // then prepend the locator's path.
        std::reverse(path.begin(), path.end());
        path.insert(path.begin(),
                    locator->getPath().begin(),
                    locator->getPath().end());
        return locator->getAnchor();
      }
    }

    // There was no locator. Just reverse the path.
    std::reverse(path.begin(), path.end());
    return nullptr;
  }

  /// Attempt to simplify this locator to a single expression.
  Expr *trySimplifyToExpr() const;

  /// Retrieve the last element in the path, if there is one.
  Optional<LocatorPathElt> last() const {
    // If we stored a path element here, grab it.
    if (element) return *element;

    // Otherwise, look in the previous builder if there is one.
    if (auto prevBuilder = previous.dyn_cast<ConstraintLocatorBuilder *>())
      return prevBuilder->last();

    // Next, check the constraint locator itself.
    if (auto locator = previous.dyn_cast<ConstraintLocator *>()) {
      auto path = locator->getPath();
      if (path.empty()) return None;
      return path.back();
    }

    return None;
  }
};

} // end namespace constraints
} // end namespace swift

#endif // LLVM_SWIFT_SEMA_CONSTRAINTLOCATOR_H
