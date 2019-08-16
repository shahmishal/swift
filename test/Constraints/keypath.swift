// RUN: %target-swift-frontend -typecheck -verify %S/Inputs/keypath.swift -primary-file %s

struct S {
  let i: Int

  init() {
    let _: WritableKeyPath<S, Int> = \.i // no error for Swift 3/4

    S()[keyPath: \.i] = 1
    // expected-error@-1 {{cannot assign through subscript: function call returns immutable value}}
  }
}

func test() {
  let _: WritableKeyPath<C, Int> = \.i // no error for Swift 3/4

  C()[keyPath: \.i] = 1   // warning on write with literal keypath
  // expected-warning@-1 {{forming a writable keypath to property}}

  let _ = C()[keyPath: \.i] // no warning for a read
}

// SR-7339
class Some<T, V> { // expected-note {{'V' declared as parameter to type 'Some'}}
  init(keyPath: KeyPath<T, ((V) -> Void)?>) {
  }
}

class Demo {
  var here: (() -> Void)?
}

let some = Some(keyPath: \Demo.here)
// expected-error@-1 {{cannot convert value of type 'KeyPath<Demo, (() -> Void)?>' to expected argument type 'KeyPath<Demo, ((Any) -> Void)?>'}}
// expected-note@-2 {{arguments to generic parameter 'Value' ('(() -> Void)?' and '((Any) -> Void)?') are expected to be equal}}
// expected-error@-3 {{generic parameter 'V' could not be inferred}}
// expected-note@-4 {{explicitly specify the generic arguments to fix this issue}}

// SE-0249
func testFunc() {
  let _: (S) -> Int = \.i
  _ = ([S]()).map(\.i)

  // FIXME: A terrible error, but the same as the pre-existing key path
  // error in the similar situation: 'let _ = \S.init'.
  _ = ([S]()).map(\.init)
  // expected-error@-1 {{type of expression is ambiguous without more context}}

  let kp = \S.i
  let _: KeyPath<S, Int> = kp // works, because type defaults to KeyPath nominal
  let f = \S.i
  let _: (S) -> Int = f // expected-error {{cannot convert value of type 'KeyPath<S, Int>' to specified type '(S) -> Int'}}
}
