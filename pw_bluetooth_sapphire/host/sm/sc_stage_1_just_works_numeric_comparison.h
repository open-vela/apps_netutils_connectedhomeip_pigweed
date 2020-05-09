// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_SC_STAGE_1_JUST_WORKS_NUMERIC_COMPARISON_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_SC_STAGE_1_JUST_WORKS_NUMERIC_COMPARISON_H_

#include <lib/fit/function.h>

#include <cstdint>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/uint128.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/pairing_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/pairing_phase.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/sc_stage_1.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace bt {
namespace sm {

// ScStage1JustWorksNumericComparison encapsulates Stage 1 of LE Secure Connections Pairing Phase
// 2, which handles authentication using the Just Works or Numeric Comparison methods described in
// Spec V5.0 Vol. 3 Part H 2.3.5.6.2.
//
// This class is not thread safe and is meant to be accessed on the thread it was created on. All
// callbacks will be run by the default dispatcher of a Phase2SecureConnections's creation thread.
class ScStage1JustWorksNumericComparison final : public ScStage1 {
 public:
  ScStage1JustWorksNumericComparison(fxl::WeakPtr<PairingPhase::Listener> listener, Role role,
                                     UInt256 local_pub_key_x, UInt256 peer_pub_key_x,
                                     PairingMethod method,
                                     fit::function<void(ByteBufferPtr)> send_cb,
                                     Stage1CompleteCallback on_complete);
  void Run() override;
  void OnPairingConfirm(PairingConfirmValue confirm) override;
  void OnPairingRandom(PairingRandomValue rand) override;

 private:
  void SendPairingRandom();

  // Called after all crypto messages have been exchanged
  void CompleteStage1();

  fxl::WeakPtr<PairingPhase::Listener> listener_;
  Role role_;
  UInt256 local_public_key_x_;
  UInt256 peer_public_key_x_;
  PairingMethod method_;

  std::optional<PairingConfirmValue> confirm_;
  bool sent_pairing_confirm_;

  PairingRandomValue local_rand_;
  bool sent_local_rand_;
  std::optional<PairingRandomValue> peer_rand_;

  // Callback allowing this Stage 1 class to send messages to the L2CAP SM channel
  fit::function<void(ByteBufferPtr)> send_cb_;
  Stage1CompleteCallback on_complete_;
  fxl::WeakPtrFactory<ScStage1JustWorksNumericComparison> weak_ptr_factory_;
};
}  // namespace sm
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_SM_SC_STAGE_1_JUST_WORKS_NUMERIC_COMPARISON_H_
