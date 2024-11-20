// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTION_REQUEST_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTION_REQUEST_H_

#include <lib/fit/function.h>
#include <lib/fit/result.h>
#include <lib/sys/inspect/cpp/component.h>

#include "low_energy_connection_handle.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/common/inspectable.h"
#include "src/connectivity/bluetooth/core/bt-host/common/status.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_discovery_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/types.h"

namespace bt::gap {

// Connection options for a LowEnergyConnectionRequest.
// TODO(fxbug.dev/66696): Move back into LowEnergyConnectionManager after dependency removed from
// LowEnergyConnection.
struct LowEnergyConnectionOptions {
  // The sm::BondableMode to connect with.
  sm::BondableMode bondable_mode = sm::BondableMode::Bondable;

  // When present, service discovery performed following the connection is restricted to primary
  // services that match this field. Otherwise, by default all available services are discovered.
  std::optional<UUID> service_uuid = std::nullopt;

  // When true, skip scanning before connecting. This should only be true when the connection is
  // initiated as a result of a directed advertisement.
  bool auto_connect = false;
};

namespace internal {
// LowEnergyConnectionRequest is used to model queued outbound connection and interrogation requests
// in both LowEnergyConnectionManager and LowEnergyConnection. Duplicate connection request
// callbacks are added with |AddCallback|, and |NotifyCallbacks| is called when the request is
// completed.
class LowEnergyConnectionRequest final {
 public:
  using ConnectionResult = fit::result<std::unique_ptr<LowEnergyConnectionHandle>, HostError>;
  using ConnectionResultCallback = fit::function<void(ConnectionResult)>;

  LowEnergyConnectionRequest(PeerId peer_id, ConnectionResultCallback first_callback,
                             LowEnergyConnectionOptions connection_options);
  ~LowEnergyConnectionRequest() = default;

  LowEnergyConnectionRequest(LowEnergyConnectionRequest&&) = default;
  LowEnergyConnectionRequest& operator=(LowEnergyConnectionRequest&&) = default;

  void AddCallback(ConnectionResultCallback cb) { callbacks_.Mutable()->push_back(std::move(cb)); }

  // Notifies all elements in |callbacks| with |status| and the result of
  // |func|.
  using RefFunc = fit::function<std::unique_ptr<LowEnergyConnectionHandle>()>;
  void NotifyCallbacks(fit::result<RefFunc, HostError> result);

  // Attach request inspect node as a child node of |parent| with the name |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  PeerId peer_id() const { return *peer_id_; }

  LowEnergyConnectionOptions connection_options() const { return connection_options_; }

  void set_discovery_session(LowEnergyDiscoverySessionPtr session) {
    session_ = std::move(session);
  }

  LowEnergyDiscoverySession* discovery_session() { return session_.get(); }

 private:
  StringInspectable<PeerId> peer_id_;
  IntInspectable<std::list<ConnectionResultCallback>> callbacks_;
  LowEnergyConnectionOptions connection_options_;
  LowEnergyDiscoverySessionPtr session_;
  inspect::Node inspect_node_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnectionRequest);
};

}  // namespace internal
}  // namespace bt::gap

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTION_REQUEST_H_
