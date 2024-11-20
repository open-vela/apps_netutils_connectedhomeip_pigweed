// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTOR_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTOR_H_

#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_connection.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_connection_request.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_interrogator.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/low_energy_connector.h"

namespace bt::gap::internal {

class LowEnergyConnector final {
 public:
  using ResultCallback =
      fit::callback<void(fit::result<std::unique_ptr<LowEnergyConnection>, hci::Status>)>;

  // Initiate an outbound connection to |peer_id|. |cb| will be called with the result of the
  // procedure.
  static std::unique_ptr<LowEnergyConnector> CreateOutboundConnector(
      PeerId peer_id, LowEnergyConnectionOptions options, hci::LowEnergyConnector* connector,
      zx::duration request_timeout, fxl::WeakPtr<hci::Transport> transport, PeerCache* peer_cache,
      fxl::WeakPtr<LowEnergyDiscoveryManager> discovery_manager,
      fxl::WeakPtr<LowEnergyConnectionManager> conn_mgr, fbl::RefPtr<l2cap::L2cap> l2cap,
      fxl::WeakPtr<gatt::GATT> gatt, ResultCallback cb);

  // Start interrogating peer using an already established |connection|. |cb| will be called with
  // the result of the procedure.
  static std::unique_ptr<LowEnergyConnector> CreateInboundConnector(
      PeerId peer_id, std::unique_ptr<hci::Connection> connection,
      LowEnergyConnectionOptions options, fxl::WeakPtr<hci::Transport> transport,
      PeerCache* peer_cache, fxl::WeakPtr<LowEnergyConnectionManager> conn_mgr,
      fbl::RefPtr<l2cap::L2cap> l2cap, fxl::WeakPtr<gatt::GATT> gatt, ResultCallback cb);

  // Instances should only be destroyed after the result callback is called (except for stack tear
  // down). Due to the asynchronous nature of cancelling the connection process, it is NOT safe to
  // destroy a connector before the result callback has been called. The connector will be unable to
  // wait for the HCI connection cancellation to complete, which can lead to failure to connect in
  // later connectors (as the HCI connector is still pending).
  ~LowEnergyConnector();

  // Cancelling an already completed connector is a no-op.
  void Cancel();

 private:
  enum class State {
    kIdle,
    kStartingScanning,
    kScanning,
    kConnecting,
    kInterrogating,
    kAwaitingConnectionFailedToBeEstablishedDisconnect,
    kComplete,
    kFailed,
  };

  LowEnergyConnector(bool outbound, PeerId peer_id, std::unique_ptr<hci::Connection> connection,
                     LowEnergyConnectionOptions options, hci::LowEnergyConnector* connector,
                     zx::duration request_timeout, fxl::WeakPtr<hci::Transport> transport,
                     PeerCache* peer_cache, fxl::WeakPtr<LowEnergyConnectionManager> conn_mgr,
                     fxl::WeakPtr<LowEnergyDiscoveryManager> discovery_manager,
                     fbl::RefPtr<l2cap::L2cap> l2cap, fxl::WeakPtr<gatt::GATT> gatt,
                     ResultCallback cb);

  static const char* StateToString(State);

  // Initiate scanning for peer before connecting to ensure it is advertising.
  void StartScanningForPeer();
  void OnScanStart(LowEnergyDiscoverySessionPtr session);

  // Initiate HCI connection procedure.
  void RequestCreateConnection();
  void OnConnectResult(hci::Status status, hci::ConnectionPtr link);

  // Creates LowEnergyConnection and initializes fixed chanels & timers.
  void InitializeConnection(hci::ConnectionPtr link);

  void StartInterrogation();
  void OnInterrogationComplete(hci::Status status);

  // Handle a disconnect during kInterrogating or
  // kAwaitingConnectionFailedToBeEstablishedDisconnect.
  void OnPeerDisconnect(hci::StatusCode status);

  // Returns true if the connection is retried.
  //
  // The link layer only considers a connection established after a packet is received from the peer
  // before (6 * connInterval), even though it notifies the host immediately after sending a
  // CONNECT_IND pdu. See Core Spec v5.2, Vol 6, Part B, Sec 4.5 for details.
  //
  // In the field, we have noticed a substantial amount of 0x3e (Connection Failed to be
  // Established) HCI link errors occurring on links AFTER being notified of successful HCI-level
  // connection. To work around this issue, we perform link-layer interrogation on the peer before
  // returning gap::LowEnergyConnections to higher layer clients. If we receive the 0x3e error
  // during interrogation, we will retry the connection process a number of times.
  bool MaybeRetryConnection();

  void NotifySuccess();
  void NotifyFailure(hci::Status status = hci::Status(HostError::kFailed));

  State state_;

  PeerId peer_id_;
  DeviceAddress peer_address_;
  PeerCache* peer_cache_;

  // Layer pointers to be passed to LowEnergyConnection.
  fbl::RefPtr<l2cap::L2cap> l2cap_;
  fxl::WeakPtr<gatt::GATT> gatt_;

  // True if this connector is connecting an outbound connection, false if it is connecting an
  // inbound connection.
  bool is_outbound_;

  // Time after which an outbound HCI connection request is considered to have timed out. This
  // is configurable to allow unit tests to set a shorter value.
  zx::duration hci_request_timeout_;

  LowEnergyConnectionOptions options_;

  // Callback used to return the result of the connection procedure to the owning class.
  ResultCallback result_cb_;

  // Used to connect outbound connections during the kConnecting state.
  hci::LowEnergyConnector* hci_connector_;

  // The LowEnergyConnection to be passed to LowEnergyConnectionManager. Created during the
  // kConnecting state for outbound connections, or during construction for inbound connections.
  std::unique_ptr<internal::LowEnergyConnection> connection_;

  // For outbound connections, this is a 0-indexed counter of which connection attempt the connector
  // is on.
  int connection_attempt_;

  // Task called after the scan attempt times out.
  std::optional<async::TaskClosure> scan_timeout_task_;

  std::unique_ptr<LowEnergyDiscoverySession> discovery_session_;

  // Sends HCI commands that request version and feature support information from peer
  // controllers.
  LowEnergyInterrogator interrogator_;

  fxl::WeakPtr<LowEnergyDiscoveryManager> discovery_manager_;

  fxl::WeakPtr<hci::Transport> transport_;

  // Only used to construct a LowEnergyConnection.
  fxl::WeakPtr<LowEnergyConnectionManager> le_connection_manager_;

  fxl::WeakPtrFactory<LowEnergyConnector> weak_ptr_factory_;
};

}  // namespace bt::gap::internal

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GAP_LOW_ENERGY_CONNECTOR_H_
