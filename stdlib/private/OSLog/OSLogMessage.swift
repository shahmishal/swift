//===----------------- OSLogMessage.swift ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// This file contains data structures and helper functions that are used by
// the new OS log APIs. These are prototype implementations and should not be
// used outside of tests.

/// Formatting options supported by the logging APIs for logging integers.
/// These can be specified in the string interpolation passed to the log APIs.
/// For Example,
///     log.info("Writing to file with permissions: \(perm, format: .octal)")
///
/// See `OSLogInterpolation.appendInterpolation` definitions for default options
/// for integer types.
public enum IntFormat {
  case decimal
  case hex
  case octal
}

/// Privacy qualifiers for indicating the privacy level of the logged data
/// to the logging system. These can be specified in the string interpolation
/// passed to the log APIs.
/// For Example,
///     log.info("Login request from user id \(userid, privacy: .private)")
///
/// See `OSLogInterpolation.appendInterpolation` definitions for default options
/// for each supported type.
public enum Privacy {
  case `private`
  case `public`
}

/// Maximum number of arguments i.e., interpolated expressions that can
/// be used in the string interpolations passed to the log APIs.
/// This limit is imposed by the ABI of os_log.
@_transparent
public var maxOSLogArgumentCount: UInt8 { return 48 }

@usableFromInline
@_transparent
internal var logBitsPerByte: Int { return 3 }

/// Represents a string interpolation passed to the log APIs.
///
/// This type converts (through its methods) the given string interpolation into
/// a C-style format string and a sequence of arguments, which is represented
/// by the type `OSLogArguments`.
///
/// Do not create an instance of this type directly. It is used by the compiler
/// when you pass a string interpolation to the log APIs.
/// Extend this type with more `appendInterpolation` overloads to enable
/// interpolating additional types.
@frozen
public struct OSLogInterpolation : StringInterpolationProtocol {
  /// A format string constructed from the given string interpolation to be
  /// passed to the os_log ABI.
  @usableFromInline
  internal var formatString: String

  /// A representation of a sequence of arguments that must be serialized
  /// to a byte buffer and passed to the os_log ABI. Each argument, which is
  /// an (autoclosured) expressions that is interpolated, is prepended with a
  /// two byte header. The first header byte consists of a four bit flag and
  /// a four bit type. The second header byte has the size of the argument in
  /// bytes. This is schematically illustrated below.
  ///                 ----------------------------
  ///                 | 4-bit type  | 4-bit flag  |
  ///                 ----------------------------
  ///                 | 1st argument size in bytes|
  ///                 ----------------------------
  ///                 |     1st argument bytes    |
  ///                 ----------------------------
  ///                 | 4-bit type  | 4-bit flag  |
  ///                 -----------------------------
  ///                 | 2nd argument size in bytes|
  ///                 ----------------------------
  ///                 |     2nd argument bytes    |
  ///                 ----------------------------
  ///                         ...
  @usableFromInline
  internal var arguments: OSLogArguments

  /// The possible values for the argument flag, as defined by the os_log ABI,
  /// which occupies four least significant bits of the first byte of the
  /// argument header. The first two bits are used to indicate privacy and
  /// the other two are reserved.
  @usableFromInline
  @_frozen
  internal enum ArgumentFlag {
    case privateFlag
    case publicFlag

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .privateFlag:
        return 0x1
      case .publicFlag:
        return 0x2
      }
    }
  }

  /// The possible values for the argument type, as defined by the os_log ABI,
  /// which occupies four most significant bits of the first byte of the
  /// argument header.
  @usableFromInline
  @_frozen
  internal enum ArgumentType {
    case scalar
    // TODO: more types will be added here.

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .scalar:
        return 0
      }
    }
  }

  /// The first summary byte in the byte buffer passed to the os_log ABI that
  /// summarizes the privacy and nature of the arguments.
  @usableFromInline
  internal var preamble: UInt8

  /// Bit mask for setting bits in the peamble. The bits denoted by the bit
  /// mask indicate whether there is an argument that is private, and whether
  /// there is an argument that is non-scalar: String, NSObject or Pointer.
  @usableFromInline
  @_frozen
  internal enum PreambleBitMask {
    case privateBitMask
    case nonScalarBitMask

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .privateBitMask:
        return 0x1
      case .nonScalarBitMask:
        return 0x2
      }
    }
  }

  /// The second summary byte that denotes the number of arguments, which is
  /// also the number of interpolated expressions. This will be determined
  /// on the fly in order to support concatenation and interpolation of
  /// instances of `OSLogMessage`.
  @usableFromInline
  internal var argumentCount: UInt8

  /// Sum total of all the bytes (including header bytes) needed for
  /// serializing the arguments.
  @usableFromInline
  internal var totalBytesForSerializingArguments: Int

  // Some methods defined below are marked @_optimize(none) to prevent inlining
  // of string internals (such as String._StringGuts) which will interfere with
  // constant evaluation and folding. Note that these methods will be inlined,
  // constant evaluated/folded and optimized in the context of a caller.

  @_transparent
  @_optimize(none)
  public init(literalCapacity: Int, interpolationCount: Int) {
    // Since the format string is fully constructed at compile time,
    // the parameter `literalCapacity` is ignored.
    formatString = ""
    arguments = OSLogArguments(capacity: interpolationCount)
    preamble = 0
    argumentCount = 0
    totalBytesForSerializingArguments = 0
  }

  /// An internal initializer that should be used only when there are no
  /// interpolated expressions. This function must be constant evaluable.
  @inlinable
  @_semantics("oslog.interpolation.init")
  @_optimize(none)
  internal init() {
    formatString = ""
    arguments = OSLogArguments()
    preamble = 0
    argumentCount = 0
    totalBytesForSerializingArguments = 0
  }

  @_transparent
  @_optimize(none)
  public mutating func appendLiteral(_ literal: String) {
    formatString += literal.percentEscapedString
  }

  /// Define interpolation for expressions of type Int. This definition enables
  /// passing a formatting option and a privacy qualifier along with the
  /// interpolated expression as shown below:
  ///
  ///         "\(x, format: .hex, privacy: .private\)"
  ///
  /// - Parameters:
  ///  - number: the interpolated expression of type Int, which is autoclosured.
  ///  - format: a formatting option available for Int types, defined by the
  ///    enum `IntFormat`.
  ///  - privacy: a privacy qualifier which is either private or public.
  ///    The default is public.
  @_transparent
  @_optimize(none)
  public mutating func appendInterpolation(
    _ number: @autoclosure @escaping () -> Int,
    format: IntFormat = .decimal,
    privacy: Privacy = .public
  ) {
    guard argumentCount < maxOSLogArgumentCount else { return }

    addIntHeadersAndFormatSpecifier(
      format,
      isPrivate: isPrivate(privacy),
      bitWidth: Int.bitWidth,
      isSigned: true)
    arguments.append(number)
    argumentCount += 1
  }

  /// Construct/update format string and headers from the parameters of the
  /// interpolation.
  @_transparent
  @_optimize(none)
  public mutating func addIntHeadersAndFormatSpecifier(
    _ format: IntFormat,
    isPrivate: Bool,
    bitWidth: Int,
    isSigned: Bool
  ) {
    formatString += getIntegerFormatSpecifier(
      format,
      isPrivate: isPrivate,
      bitWidth: bitWidth,
      isSigned: isSigned)

    // Append argument header.
    let argumentHeader =
      getArgumentHeader(isPrivate: isPrivate, bitWidth: bitWidth, type: .scalar)
    arguments.append(argumentHeader)

    // Append number of bytes needed to serialize the argument.
    let argumentByteCount = OSLogSerializationInfo.sizeForEncoding(Int.self)
    arguments.append(UInt8(argumentByteCount))

    // Increment total byte size by the number of bytes needed for this
    // argument, which is the sum of the byte size of the argument and
    // two bytes needed for the headers.
    totalBytesForSerializingArguments += argumentByteCount + 2

    preamble = getUpdatedPreamble(isPrivate: isPrivate)
  }

  /// Return true if and only if the parameter is .private.
  /// This function must be constant evaluable.
  @inlinable
  @_semantics("oslog.interpolation.isPrivate")
  @_effects(readonly)
  @_optimize(none)
  internal func isPrivate(_ privacy: Privacy) -> Bool {
    // Do not use equality comparisons on enums as it is not supported by
    // the constant evaluator.
    if case .private = privacy {
      return true
    }
    return false
  }

  /// compute a byte-sized argument header consisting of flag and type.
  /// Flag and type take up the least and most significant four bits
  /// of the header byte, respectively.
  /// This function should be constant evaluable.
  @inlinable
  @_semantics("oslog.interpolation.getArgumentHeader")
  @_effects(readonly)
  @_optimize(none)
  internal func getArgumentHeader(
    isPrivate: Bool,
    bitWidth: Int,
    type: ArgumentType
  ) -> UInt8 {
    let flag: ArgumentFlag = isPrivate ? .privateFlag : .publicFlag
    let flagAndType: UInt8 = (type.rawValue &<< 4) | flag.rawValue
    return flagAndType
  }

  /// Compute the new preamble based whether the current argument is private
  /// or not. This function must be constant evaluable.
  @inlinable
  @_semantics("oslog.interpolation.getUpdatedPreamble")
  @_effects(readonly)
  @_optimize(none)
  internal func getUpdatedPreamble(isPrivate: Bool) -> UInt8 {
    if isPrivate {
      return preamble | PreambleBitMask.privateBitMask.rawValue
    }
    return preamble
  }

  /// Construct an os_log format specifier from the given parameters.
  /// This function must be constant evaluable and all its arguments
  /// must be known at compile time.
  @inlinable
  @_semantics("oslog.interpolation.getFormatSpecifier")
  @_effects(readonly)
  @_optimize(none)
  internal func getIntegerFormatSpecifier(
    _ format: IntFormat,
    isPrivate: Bool,
    bitWidth: Int,
    isSigned: Bool
  ) -> String {
    var formatSpecifier: String = isPrivate ? "%{private}" : "%{public}"

    // Add a length modifier, if needed, to the specifier
    // TODO: more length modifiers will be added.
    if (bitWidth == CLongLong.bitWidth) {
      formatSpecifier += "ll"
    }

    // TODO: more format specifiers will be added.
    switch (format) {
    case .hex:
      formatSpecifier += "x"
    case .octal:
      formatSpecifier += "o"
    default:
      formatSpecifier += isSigned ? "d" : "u"
    }
    return formatSpecifier
  }
}

extension String {
  /// Replace all percents "%" in the string by "%%" so that the string can be
  /// interpreted as a C format string.
  public var percentEscapedString: String {
    @_semantics("string.escapePercent.get")
    @_effects(readonly)
    @_optimize(none)
    get {
      return self
        .split(separator: "%", omittingEmptySubsequences: false)
        .joined(separator: "%%")
    }
  }
}

@frozen
public struct OSLogMessage :
  ExpressibleByStringInterpolation, ExpressibleByStringLiteral
{
  public let interpolation: OSLogInterpolation

  /// Initializer for accepting string interpolations. This function must be
  /// constant evaluable.
  @inlinable
  @_optimize(none)
  @_semantics("oslog.message.init_interpolation")
  public init(stringInterpolation: OSLogInterpolation) {
    self.interpolation = stringInterpolation
  }

  /// Initializer for accepting string literals. This function must be
  /// constant evaluable.
  @inlinable
  @_optimize(none)
  @_semantics("oslog.message.init_stringliteral")
  public init(stringLiteral value: String) {
    var s = OSLogInterpolation()
    s.appendLiteral(value)
    self.interpolation = s
  }

  /// The byte size of the buffer that will be passed to the C os_log ABI.
  /// It will contain the elements of `interpolation.arguments` and the two
  /// summary bytes: preamble and argument count.
  @_transparent
  @_optimize(none)
  public var bufferSize: Int {
    return interpolation.totalBytesForSerializingArguments + 2
  }
}


/// A representation of a sequence of arguments and headers (of possibly
/// different types) that have to be serialized to a byte buffer. The arguments
/// are captured within closures and stored in an array. The closures accept an
/// instance of `OSLogByteBufferBuilder`, and when invoked, serialize the
/// argument using the passed `OSLogByteBufferBuilder` instance.
@frozen
@usableFromInline
internal struct OSLogArguments {
  /// An array of closures that captures arguments of possibly different types.
  @usableFromInline
  internal var argumentClosures: [(inout OSLogByteBufferBuilder) -> ()]?

  /// This function must be constant evaluable.
  @inlinable
  @_semantics("oslog.arguments.init_empty")
  @_optimize(none)
  internal init() {
    argumentClosures = nil
  }

  @usableFromInline
  internal init(capacity: Int) {
    argumentClosures = []
    argumentClosures!.reserveCapacity(capacity)
  }

  /// Append a byte-sized header, constructed by
  /// `OSLogMessage.appendInterpolation`, to the tracked array of closures.
  @usableFromInline
  internal mutating func append(_ header: UInt8) {
    argumentClosures!.append({ $0.serialize(header) })
  }

  /// Append an (autoclosured) interpolated expression of type Int, passed to
  /// `OSLogMessage.appendInterpolation`, to the tracked array of closures.
  @usableFromInline
  internal mutating func append(_ value: @escaping () -> Int) {
    argumentClosures!.append({ $0.serialize(value()) })
  }

  @usableFromInline
  internal func serialize(into bufferBuilder: inout OSLogByteBufferBuilder) {
    argumentClosures?.forEach { $0(&bufferBuilder) }
  }
}

/// A struct that provides information regarding the serialization of types
/// to a byte buffer as specified by the C os_log ABIs.
@usableFromInline
internal struct OSLogSerializationInfo {
  /// Return the number of bytes needed for serializing an UInt8 value.
  @usableFromInline
  @_transparent
  internal static func sizeForEncoding(_ type: UInt8.Type) -> Int {
    return 1
  }

  /// Return the number of bytes needed for serializing an Int value.
  @usableFromInline
  @_transparent
  internal static func sizeForEncoding(_ type: Int.Type) -> Int {
    return Int.bitWidth &>> logBitsPerByte
  }
}

/// A struct that manages serialization of instances of specific types to a
/// byte buffer. The byte buffer is provided as an argument to the initializer
/// so that its lifetime can be managed by the caller.
@usableFromInline
internal struct OSLogByteBufferBuilder {
  internal var position: UnsafeMutablePointer<UInt8>

  /// Initializer that accepts a pointer to a preexisting buffer.
  /// - Parameter bufferStart: the starting pointer to a byte buffer
  ///   that must contain the serialized bytes.
  @usableFromInline
  internal init(_ bufferStart: UnsafeMutablePointer<UInt8>) {
    position = bufferStart
  }

  /// Serialize a UInt8 value at the buffer location pointed to by `position`.
  @usableFromInline
  internal mutating func serialize(_ value: UInt8) {
    position[0] = value
    position += 1
  }

  /// Serialize an Int at the buffer location pointed to by `position`.
  @usableFromInline
  internal mutating func serialize(_ value: Int) {
    let byteCount = OSLogSerializationInfo.sizeForEncoding(Int.self)
    let dest = UnsafeMutableRawBufferPointer(start: position, count: byteCount)
    withUnsafeBytes(of: value) { dest.copyMemory(from: $0) }
    position += byteCount
  }
}
