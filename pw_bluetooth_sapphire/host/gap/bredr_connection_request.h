// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_BREDR_CONNECTION_REQUEST_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_BREDR_CONNECTION_REQUEST_H_

#include <lib/fit/function.h>

#include <list>

#include "fbl/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/common/identifier.h"
#include "src/connectivity/bluetooth/core/bt-host/common/inspectable.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/status.h"

namespace bt::gap {

class BrEdrConnection;

// A |BrEdrConnectionRequest| represents a request for the GAP to connect to a given
// |DeviceAddress| by one or more clients. BrEdrConnectionManager
// is responsible for tracking ConnectionRequests and passing them to the
// Connector when ready.
//
// There is at most One BrEdrConnectionRequest per address at any given time; if
// multiple clients wish to connect, they each append a callback to the list in
// the ConnectionRequest for the device they are interested in.
//
// If a remote peer makes an incoming request for a connection, we track that
// here also - whether an incoming request is pending is indicated by
// HasIncoming()
class BrEdrConnectionRequest final {
 public:
  using OnComplete = fit::function<void(hci::Status, BrEdrConnection*)>;
  using RefFactory = fit::function<BrEdrConnection*()>;

  // Construct without a callback. Can be used for incoming only requests
  BrEdrConnectionRequest(const DeviceAddress& addr, PeerId peer_id);

  BrEdrConnectionRequest(const DeviceAddress& addr, PeerId peer_id, OnComplete&& callback);

  BrEdrConnectionRequest(BrEdrConnectionRequest&&) = default;
  BrEdrConnectionRequest& operator=(BrEdrConnectionRequest&&) = default;

  void AddCallback(OnComplete cb) { callbacks_.Mutable()->push_back(std::move(cb)); }

  // Notifies all elements in |callbacks| with |status| and the result of
  // |generate_ref|. Called by the appropriate manager once a connection request
  // has completed, successfully or otherwise
  void NotifyCallbacks(hci::Status status, const RefFactory& generate_ref);

  void BeginIncoming() { has_incoming_.Set(true); }
  void CompleteIncoming() { has_incoming_.Set(false); }
  bool HasIncoming() { return *has_incoming_; }
  bool AwaitingOutgoing() { return !callbacks_->empty(); }

  // Attach request inspect node as a child of |parent| named |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  DeviceAddress address() const { return address_; }

  // If a role change occurs while this request is still pending, set it here so that the correct
  // role is used when connection establishment completes.
  void set_role_change(hci_spec::ConnectionRole role) { role_change_ = role; }

  // If the default role of the requested connection is changed during connection establishment, the
  // new role will be returned.
  const std::optional<hci_spec::ConnectionRole>& role_change() const { return role_change_; }

 private:
  PeerId peer_id_;
  DeviceAddress address_;
  UintInspectable<std::list<OnComplete>> callbacks_;
  BoolInspectable<bool> has_incoming_;
  std::optional<hci_spec::ConnectionRole> role_change_;

  inspect::StringProperty peer_id_property_;
  inspect::Node inspect_node_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BrEdrConnectionRequest);
};

}  // namespace bt::gap

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_BREDR_CONNECTION_REQUEST_H_
