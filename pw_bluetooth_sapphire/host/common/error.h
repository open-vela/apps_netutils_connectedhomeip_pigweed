// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_ERROR_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_ERROR_H_

#include <lib/fitx/result.h>

#include <type_traits>
#include <variant>

#include "src/connectivity/bluetooth/core/bt-host/common/status.h"

namespace bt {

// Type used to hold either a HostError or a ProtocolErrorCode, a protocol-defined code. This can
// not be constructed in such a way to represent a success or to contain the product of a successful
// operation, but to be used as the error type parameter of a generic result type like
// fitx::result<Error<…>> or fitx::result<Error<…>, V>.
//
// As such, Errors can only be constructed indirectly through the ToResult function.
template <typename ProtocolErrorCode>
class Error;

// Create a fitx::result<Error<…>> from a HostError. The template parameter is used to specify the
// kind of protocol error that the Error could hold instead of the HostError provided.
template <typename ProtocolErrorCode>
[[nodiscard]] constexpr fitx::result<Error<ProtocolErrorCode>> ToResult(HostError host_error) {
  // TODO(fxbug.dev/86900): Remove this enum value alongside bt::Status
  if (host_error == HostError::kNoError) {
    return fitx::success();
  }
  return fitx::error(Error<ProtocolErrorCode>(host_error));
}

// Create a fitx::result<Error<…>> from a protocol error.
// This overload doesn't collide with the above when instantiated with <HostError>, because this
// would try to construct an invalid Error<HostError>.
template <typename ProtocolErrorCode>
[[nodiscard]] constexpr fitx::result<Error<ProtocolErrorCode>> ToResult(
    ProtocolErrorCode proto_error) {
  if (ProtocolErrorTraits<ProtocolErrorCode>::is_success(proto_error)) {
    return fitx::success();
  }
  return fitx::error(Error(proto_error));
}

template <typename ProtocolErrorCode>
class [[nodiscard]] Error {
  static_assert(!std::is_same_v<HostError, ProtocolErrorCode>,
                "HostError can not be a protocol error");

 public:
  Error() = delete;
  ~Error() = default;
  constexpr Error(const Error&) = default;
  constexpr Error(Error&&) noexcept = default;
  constexpr Error& operator=(const Error&) = default;
  constexpr Error& operator=(Error&&) noexcept = default;

  // Evaluates to true if and only if both Errors hold the same kind of error. Errors with different
  // ProtocolErrorCodes are intentionally not defined, because it's likely an antipattern and the
  // client can always define comparisons between specific pairs of protocol errors as needed.
  constexpr bool operator==(const Error& rhs) const {
    if (error_.index() != rhs.error_.index()) {
      return false;
    }
    return Visit(
        [&rhs](HostError held) { return held == std::get<HostError>(rhs.error_); },
        [&rhs](ProtocolErrorCode held) { return held == std::get<ProtocolErrorCode>(rhs.error_); });
  }

  // This is required to take precedence over the two generic operator!= overloads that would match
  // Error != Error expressions
  constexpr bool operator!=(const Error& rhs) const { return !(*this == rhs); }

  [[nodiscard]] constexpr bool is_host_error() const {
    return std::holds_alternative<HostError>(error_);
  }

  [[nodiscard]] constexpr bool is_protocol_error() const {
    return std::holds_alternative<ProtocolErrorCode>(error_);
  }

  [[nodiscard]] constexpr HostError host_error() const {
    ZX_ASSERT_MSG(is_host_error(), "Does not hold HostError");
    return std::get<HostError>(error_);
  }

  [[nodiscard]] constexpr ProtocolErrorCode protocol_error() const {
    ZX_ASSERT_MSG(is_protocol_error(), "Does not hold protocol error");
    return std::get<ProtocolErrorCode>(error_);
  }

  [[nodiscard]] constexpr bool is(ProtocolErrorCode proto_error) const {
    return Visit([](HostError) { return false; },
                 [proto_error](ProtocolErrorCode held) { return held == proto_error; });
  }

  [[nodiscard]] constexpr bool is(HostError host_error) const {
    return Visit([host_error](HostError held) { return held == host_error; },
                 [](ProtocolErrorCode) { return false; });
  }

  // Given two "visitors" (callable objects that accept HostError and ProtocolErrorCode), invoke the
  // one that corresponds to the error held in storage, but not the other.
  // This pattern allows the code within the visitors to statically presume the type of the error
  // code that they work with. Example:
  //
  //   int ConvertToInt(Error<FooError> error) {
  //     return Visit(
  //         [](HostError held) { return static_cast<int>(held); },
  //         [](ProtocolErrorCode held) { return static_cast<int>(held); });
  //     );
  //   }
  //
  // Unlike std::visit, the two visitors do not need to be differentiated from each other through
  // overload resolution rules: the argument order to invoking Visit(…) is what determines which
  // visitor gets called.
  //
  // Returns the return value of the visitor that was called (which may return void).
  template <typename HostVisitor, typename ProtoVisitor>
  [[nodiscard]] constexpr auto Visit(HostVisitor host_error_visitor,
                                     ProtoVisitor proto_error_visitor) const {
    // This doesn't just check that the return types match but also that the visitors can be invoked
    // with the appropriate types.
    static_assert(std::is_same_v<std::invoke_result_t<HostVisitor, HostError>,
                                 std::invoke_result_t<ProtoVisitor, ProtocolErrorCode>>,
                  "Return types of both visitors must match");
    if (is_host_error()) {
      return host_error_visitor(host_error());
    }
    return proto_error_visitor(protocol_error());
  }

 private:
  // Factory functions
  friend constexpr fitx::result<Error<ProtocolErrorCode>> ToResult<ProtocolErrorCode>(
      ProtocolErrorCode);
  friend constexpr fitx::result<Error<ProtocolErrorCode>> ToResult<ProtocolErrorCode>(HostError);

  constexpr explicit Error(ProtocolErrorCode proto_error) : error_(proto_error) {
    ZX_ASSERT(!ProtocolErrorTraits<ProtocolErrorCode>::is_success(proto_error));
  }

  constexpr explicit Error(HostError host_error) : error_(host_error) {
    // TODO(fxbug.dev/86900): Make this ctor public after these enums are removed
    ZX_ASSERT(host_error != HostError::kNoError);
    ZX_ASSERT(host_error != HostError::kProtocolError);
  }

  std::variant<HostError, ProtocolErrorCode> error_;
};

// Shorthand for commutativity
// The enable_if check ensures that this is sufficiently narrow to never call itself
template <typename ProtocolErrorCode, typename T>
constexpr std::enable_if_t<!std::is_convertible_v<T, Error<ProtocolErrorCode>>, bool> operator==(
    const Error<ProtocolErrorCode>& lhs, const T& rhs) {
  return rhs == lhs;
}

template <typename ProtocolErrorCode, typename T>
constexpr std::enable_if_t<!std::is_convertible_v<T, Error<ProtocolErrorCode>>, bool> operator!=(
    const Error<ProtocolErrorCode>& lhs, const T& rhs) {
  return rhs != lhs;
}

// Shorthand to generate operator!= from any defined operator==
template <typename ProtocolErrorCode, typename T>
constexpr bool operator!=(const T& lhs, const Error<ProtocolErrorCode>& rhs) {
  return !(lhs == rhs);
}

// Comparisons to ProtocolErrorCode
template <typename ProtocolErrorCode>
constexpr bool operator==(const ProtocolErrorCode& lhs, const Error<ProtocolErrorCode>& rhs) {
  return rhs.is(lhs);
}

// Comparisons to HostError
template <typename ProtocolErrorCode>
constexpr bool operator==(const HostError& lhs, const Error<ProtocolErrorCode>& rhs) {
  return rhs.is(lhs);
}

// Comparisons to fitx::result<Error<ProtocolErrorCode>>
template <typename ProtocolErrorCode, typename... Ts>
constexpr bool operator==(const fitx::result<Error<ProtocolErrorCode>, Ts...>& lhs,
                          const Error<ProtocolErrorCode>& rhs) {
  static_assert(std::conjunction_v<std::negation<std::is_same<Ts, Error<ProtocolErrorCode>>>...>,
                "fitx::result should not contain Error as a success value");
  return lhs.is_error() && (lhs.error_value() == rhs);
}

// Comparisons between fitx::result<Error<…>> objects
// Note that this is not standard fitx::result relation behavior which normally compares all error
// results to be equal. These are preferred in overload resolution because they are more specific
// templates than the ones provided by fitx.
template <typename ProtocolErrorCode, typename OtherProtoErrCode, typename T>
constexpr bool operator==(const fitx::result<Error<ProtocolErrorCode>, T>& lhs,
                          const fitx::result<Error<OtherProtoErrCode>, T>& rhs) {
  static_assert(!std::is_same_v<T, Error<ProtocolErrorCode>>,
                "fitx::result should not contain Error as a success value");
  if (lhs.is_ok() != rhs.is_ok()) {
    return false;
  }
  if (lhs.is_ok()) {
    return lhs.value() == rhs.value();
  }
  return lhs.error_value() == rhs.error_value();
}

template <typename ProtocolErrorCode, typename OtherProtoErrCode, typename T>
constexpr bool operator!=(const fitx::result<Error<ProtocolErrorCode>, T>& lhs,
                          const fitx::result<Error<OtherProtoErrCode>, T>& rhs) {
  return !(lhs == rhs);
}

template <typename ProtocolErrorCode, typename OtherProtoErrCode>
constexpr bool operator==(const fitx::result<Error<ProtocolErrorCode>>& lhs,
                          const fitx::result<Error<OtherProtoErrCode>>& rhs) {
  if (lhs.is_ok() != rhs.is_ok()) {
    return false;
  }
  if (lhs.is_ok()) {
    return true;
  }
  return lhs.error_value() == rhs.error_value();
}

template <typename ProtocolErrorCode, typename OtherProtoErrCode>
constexpr bool operator!=(const fitx::result<Error<ProtocolErrorCode>>& lhs,
                          const fitx::result<Error<OtherProtoErrCode>>& rhs) {
  return !(lhs == rhs);
}

}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_ERROR_H_
