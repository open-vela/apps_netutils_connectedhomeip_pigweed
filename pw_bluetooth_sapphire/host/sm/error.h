// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_ERROR_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_ERROR_H_

#include <lib/fit/function.h>

#include "src/connectivity/bluetooth/core/bt-host/common/error.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"

namespace bt {
namespace sm {

using Error = Error<bt::sm::ErrorCode>;

template <typename... V>
using Result = fitx::result<bt::sm::Error, V...>;

template <typename... V>
using ResultFunction = fit::function<void(bt::sm::Result<V...> result)>;

template <typename... V>
using ResultCallback = fit::callback<void(bt::sm::Result<V...> result)>;

}  // namespace sm

template <>
struct ProtocolErrorTraits<sm::ErrorCode> {
  static std::string ToString(sm::ErrorCode ecode);

  static constexpr bool is_success(sm::ErrorCode ecode) { return false; }
};

}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_ERROR_H_
