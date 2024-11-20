// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_STATUS_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_STATUS_H_

#include <stdarg.h>
#include <zircon/assert.h>

#include <cstdint>
#include <string>

#include "src/connectivity/bluetooth/core/bt-host/common/error.h"
#include "src/connectivity/bluetooth/core/bt-host/common/host_error.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/lib/cpp-string/string_printf.h"

namespace bt {

template <typename ProtocolErrorCode>
class Status {
  using ProtoTraits = ProtocolErrorTraits<ProtocolErrorCode>;

 public:
  // The result will carry a protocol error code.
  constexpr explicit Status(ProtocolErrorCode proto_code)
      : error_(HostError::kProtocolError), protocol_error_(proto_code) {}

  // The result will carry a host error. Constructs a success result by default.
  constexpr explicit Status(HostError ecode = HostError::kNoError) : error_(ecode) {
    ZX_DEBUG_ASSERT_MSG(ecode != HostError::kProtocolError,
                        "HostError::kProtocolError not allowed in HostError constructor");
  }

  // Returns true if this is a success result.
  bool is_success() const { return error_ == HostError::kNoError; }

  // Returns the host error code.
  HostError error() const { return error_; }

  // Returns true if this result is a protocol error.
  bool is_protocol_error() const { return error_ == HostError::kProtocolError; }

  // Returns the protocol error code. This value is undefined if error() is
  // not equal to HostError::kProtocolError.
  ProtocolErrorCode protocol_error() const { return protocol_error_; }

  // Returns true if this is a success result.
  explicit operator bool() const { return is_success(); }

  template <typename T>
  constexpr bool operator==(const Status<T>& other) const {
    // Do not disqualify incomparable Status types with SFINAE; instead, disqualify them with this
    // static_assert in order to provide a more useful error message.
    static_assert(std::is_same_v<ProtocolErrorCode, T>,
                  "Can not compare Statuses of different protocols");
    if (error() == HostError::kProtocolError && other.error() == HostError::kProtocolError) {
      return protocol_error() == other.protocol_error();
    }
    return error() == other.error();
  }

  template <typename T>
  constexpr bool operator!=(const Status<T>& other) const {
    return !(*this == other);
  }

  // Returns a string representation.
  inline std::string ToString() const {
    return bt_lib_cpp_string::StringPrintf(
        "[status: %s]",
        (is_protocol_error() ? ProtoTraits::ToString(protocol_error()) : HostErrorToString(error()))
            .c_str());
  }

  // Helper that returns true if this status represents an error and prints a message containing a
  // string representation of the status.
  bool TestForErrorAndLog(LogSeverity severity, const char* tag, const char* file, int line,
                          const char* fmt, va_list v_args) const {
    bool is_error = !is_success();
    if (!(is_error && IsLogLevelEnabled(severity))) {
      return is_error;
    }
    std::string msg = bt_lib_cpp_string::StringVPrintf(fmt, v_args);
    LogMessage(file, line, severity, tag, "%s: %s", msg.c_str(), ToString().c_str());
    return true;
  }

 private:
  HostError error_;
  ProtocolErrorCode protocol_error_;
};

// TODO(fxbug.dev/86900): Remove this alongside bt::Status
template <typename ProtocolErrorCode>
constexpr fitx::result<Error<ProtocolErrorCode>> ToResult(const Status<ProtocolErrorCode>& status) {
  if (status.is_success()) {
    return fitx::success();
  }
  if (status.is_protocol_error()) {
    return ToResult(status.protocol_error());
  }
  return ToResult<ProtocolErrorCode>(status.error());
}

namespace internal {

template <typename ProtocolErrorCode>
[[gnu::format(printf, 6, 7)]] bool TestForErrorAndLog(const Status<ProtocolErrorCode>& status,
                                                      LogSeverity severity, const char* tag,
                                                      const char* file, int line, const char* fmt,
                                                      ...) {
  va_list args;
  va_start(args, fmt);
  const bool is_error = status.TestForErrorAndLog(severity, tag, file, line, fmt, args);
  va_end(args);
  return is_error;
}

}  // namespace internal
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_STATUS_H_
