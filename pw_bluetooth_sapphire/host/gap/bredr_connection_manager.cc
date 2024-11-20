// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bredr_connection_manager.h"

#include <zircon/assert.h>

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/status.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_connection.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer_cache.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci_constants.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/sequential_command_runner.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/status.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/types.h"

namespace bt::gap {

using std::unique_ptr;
using ConnectionState = Peer::ConnectionState;

namespace {

std::string ReasonAsString(DisconnectReason reason) {
  switch (reason) {
    case DisconnectReason::kApiRequest:
      return "ApiRequest";
    case DisconnectReason::kInterrogationFailed:
      return "InterrogationFailed";
    case DisconnectReason::kPairingFailed:
      return "PairingFailed";
    case DisconnectReason::kAclLinkError:
      return "AclLinkError";
    default:
      return "<Unknown Reason>";
  }
}

// This procedure can continue to operate independently of the existence of an
// BrEdrConnectionManager instance, which will begin to disable Page Scan as it shuts down.
void SetPageScanEnabled(bool enabled, fxl::WeakPtr<hci::Transport> hci,
                        async_dispatcher_t* dispatcher, hci::StatusCallback cb) {
  ZX_DEBUG_ASSERT(cb);
  auto read_enable = hci::CommandPacket::New(hci::kReadScanEnable);
  auto finish_enable_cb = [enabled, hci, finish_cb = std::move(cb)](
                              auto, const hci::EventPacket& event) mutable {
    if (hci_is_error(event, WARN, "gap-bredr", "read scan enable failed")) {
      finish_cb(event.ToStatus());
      return;
    }

    auto params = event.return_params<hci::ReadScanEnableReturnParams>();
    uint8_t scan_type = params->scan_enable;
    if (enabled) {
      scan_type |= static_cast<uint8_t>(hci::ScanEnableBit::kPage);
    } else {
      scan_type &= ~static_cast<uint8_t>(hci::ScanEnableBit::kPage);
    }
    auto write_enable =
        hci::CommandPacket::New(hci::kWriteScanEnable, sizeof(hci::WriteScanEnableCommandParams));
    write_enable->mutable_payload<hci::WriteScanEnableCommandParams>()->scan_enable = scan_type;
    hci->command_channel()->SendCommand(
        std::move(write_enable),
        [cb = std::move(finish_cb)](auto, const hci::EventPacket& event) { cb(event.ToStatus()); });
  };
  hci->command_channel()->SendCommand(std::move(read_enable), std::move(finish_enable_cb));
}

}  // namespace

// An event signifying that a connection was completed by the controller
BrEdrConnectionManager::ConnectionComplete::ConnectionComplete(const hci::EventPacket& event) {
  ZX_ASSERT(event.event_code() == hci::kConnectionCompleteEventCode);
  const auto& params = event.params<hci::ConnectionCompleteEventParams>();
  handle = letoh16(params.connection_handle);
  addr = DeviceAddress(DeviceAddress::Type::kBREDR, params.bd_addr);
  status = hci::Status(params.status);
  link_type = params.link_type;
}

// An event signifying that an incoming connection is being requested by a peer
BrEdrConnectionManager::ConnectionRequestEvent::ConnectionRequestEvent(
    const hci::EventPacket& event) {
  ZX_ASSERT(event.event_code() == hci::kConnectionRequestEventCode);
  const auto& params = event.params<hci::ConnectionRequestEventParams>();
  addr = DeviceAddress(DeviceAddress::Type::kBREDR, params.bd_addr);
  link_type = params.link_type;
  class_of_device = params.class_of_device;
}

hci::CommandChannel::EventHandlerId BrEdrConnectionManager::AddEventHandler(
    const hci::EventCode& code, hci::CommandChannel::EventCallback cb) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto event_id = hci_->command_channel()->AddEventHandler(
      code, [self, callback = std::move(cb)](const auto& event) {
        if (self) {
          return callback(event);
        }
        return hci::CommandChannel::EventCallbackResult::kRemove;
      });
  ZX_DEBUG_ASSERT(event_id);
  event_handler_ids_.push_back(event_id);
  return event_id;
}

BrEdrConnectionManager::BrEdrConnectionManager(fxl::WeakPtr<hci::Transport> hci,
                                               PeerCache* peer_cache, DeviceAddress local_address,
                                               fbl::RefPtr<l2cap::L2cap> l2cap,
                                               bool use_interlaced_scan)
    : hci_(std::move(hci)),
      cache_(peer_cache),
      local_address_(local_address),
      l2cap_(l2cap),
      interrogator_(cache_, hci_),
      page_scan_interval_(0),
      page_scan_window_(0),
      use_interlaced_scan_(use_interlaced_scan),
      request_timeout_(kBrEdrCreateConnectionTimeout),
      dispatcher_(async_get_default_dispatcher()),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(hci_);
  ZX_DEBUG_ASSERT(cache_);
  ZX_DEBUG_ASSERT(l2cap_);
  ZX_DEBUG_ASSERT(dispatcher_);

  hci_cmd_runner_ = std::make_unique<hci::SequentialCommandRunner>(dispatcher_, hci_);

  // Register event handlers
  AddEventHandler(hci::kAuthenticationCompleteEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnAuthenticationComplete));
  AddEventHandler(hci::kConnectionCompleteEventCode, [this](const hci::EventPacket& event) {
    OnConnectionComplete(ConnectionComplete(event));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  });
  AddEventHandler(hci::kConnectionRequestEventCode, [this](const hci::EventPacket& event) {
    OnConnectionRequest(ConnectionRequestEvent(event));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  });
  AddEventHandler(hci::kIOCapabilityRequestEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnIoCapabilityRequest));
  AddEventHandler(hci::kIOCapabilityResponseEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnIoCapabilityResponse));
  AddEventHandler(hci::kLinkKeyRequestEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnLinkKeyRequest));
  AddEventHandler(hci::kLinkKeyNotificationEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnLinkKeyNotification));
  AddEventHandler(hci::kSimplePairingCompleteEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnSimplePairingComplete));
  AddEventHandler(hci::kUserConfirmationRequestEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnUserConfirmationRequest));
  AddEventHandler(hci::kUserPasskeyRequestEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnUserPasskeyRequest));
  AddEventHandler(hci::kUserPasskeyNotificationEventCode,
                  fit::bind_member(this, &BrEdrConnectionManager::OnUserPasskeyNotification));
}

BrEdrConnectionManager::~BrEdrConnectionManager() {
  if (pending_request_ && pending_request_->Cancel())
    SendCreateConnectionCancelCommand(pending_request_->peer_address());

  // Disconnect any connections that we're holding.
  connections_.clear();
  // Become unconnectable
  SetPageScanEnabled(/*enabled=*/false, hci_, dispatcher_, [](const auto) {});
  // Remove all event handlers
  for (auto handler_id : event_handler_ids_) {
    hci_->command_channel()->RemoveEventHandler(handler_id);
  }
}

void BrEdrConnectionManager::SetConnectable(bool connectable, hci::StatusCallback status_cb) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  if (!connectable) {
    auto not_connectable_cb = [self, cb = std::move(status_cb)](const auto& status) {
      if (self) {
        self->page_scan_interval_ = 0;
        self->page_scan_window_ = 0;
      } else if (status) {
        cb(hci::Status(HostError::kFailed));
        return;
      }
      cb(status);
    };
    SetPageScanEnabled(/*enabled=*/false, hci_, dispatcher_, std::move(not_connectable_cb));
    return;
  }

  WritePageScanSettings(
      hci::kPageScanR1Interval, hci::kPageScanR1Window, use_interlaced_scan_,
      [self, cb = std::move(status_cb)](const auto& status) mutable {
        if (bt_is_error(status, WARN, "gap-bredr", "Write Page Scan Settings failed")) {
          cb(status);
          return;
        }
        if (!self) {
          cb(hci::Status(HostError::kFailed));
          return;
        }
        SetPageScanEnabled(/*enabled=*/true, self->hci_, self->dispatcher_, std::move(cb));
      });
}

void BrEdrConnectionManager::SetPairingDelegate(fxl::WeakPtr<PairingDelegate> delegate) {
  pairing_delegate_ = std::move(delegate);
  for (auto& [handle, connection] : connections_) {
    connection.pairing_state().SetPairingDelegate(pairing_delegate_);
  }
}

PeerId BrEdrConnectionManager::GetPeerId(hci::ConnectionHandle handle) const {
  auto it = connections_.find(handle);
  if (it == connections_.end()) {
    return kInvalidPeerId;
  }

  auto* peer = cache_->FindByAddress(it->second.link().peer_address());
  ZX_DEBUG_ASSERT_MSG(peer, "Couldn't find peer for handle %#.4x", handle);
  return peer->identifier();
}

void BrEdrConnectionManager::Pair(PeerId peer_id, BrEdrSecurityRequirements security,
                                  hci::StatusCallback callback) {
  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "can't pair to peer_id %s: connection not found", bt_str(peer_id));
    callback(hci::Status(HostError::kNotFound));
    return;
  }
  auto& [handle, connection] = *conn_pair;
  auto pairing_callback = [pair_callback = std::move(callback)](auto, hci::Status status) {
    pair_callback(status);
  };
  connection->pairing_state().InitiatePairing(security, std::move(pairing_callback));
}

void BrEdrConnectionManager::OpenL2capChannel(PeerId peer_id, l2cap::PSM psm,
                                              BrEdrSecurityRequirements security_reqs,
                                              l2cap::ChannelParameters params,
                                              l2cap::ChannelCallback cb) {
  auto pairing_cb = [self = weak_ptr_factory_.GetWeakPtr(), peer_id, psm, params,
                     cb = std::move(cb)](auto status) mutable {
    bt_log(TRACE, "gap-bredr", "got pairing status %s, %sreturning socket to %s", bt_str(status),
           status ? "" : "not ", bt_str(peer_id));
    if (!status || !self) {
      // Report the failure to the user with a null channel.
      cb(nullptr);
      return;
    }

    auto conn_pair = self->FindConnectionById(peer_id);
    if (!conn_pair) {
      bt_log(INFO, "gap-bredr", "can't open l2cap channel: connection not found (peer: %s)",
             bt_str(peer_id));
      cb(nullptr);
      return;
    }
    auto& [handle, connection] = *conn_pair;

    connection->OpenL2capChannel(psm, params,
                                 [cb = std::move(cb)](auto chan) { cb(std::move(chan)); });
  };

  Pair(peer_id, security_reqs, std::move(pairing_cb));
}

BrEdrConnectionManager::SearchId BrEdrConnectionManager::AddServiceSearch(
    const UUID& uuid, std::unordered_set<sdp::AttributeId> attributes,
    BrEdrConnectionManager::SearchCallback callback) {
  auto on_service_discovered = [self = weak_ptr_factory_.GetWeakPtr(), uuid,
                                client_cb = std::move(callback)](PeerId peer_id, auto& attributes) {
    if (self) {
      Peer* const peer = self->cache_->FindById(peer_id);
      ZX_ASSERT(peer);
      peer->MutBrEdr().AddService(uuid);
    }
    client_cb(peer_id, attributes);
  };
  return discoverer_.AddSearch(uuid, std::move(attributes), std::move(on_service_discovered));
}

bool BrEdrConnectionManager::RemoveServiceSearch(SearchId id) {
  return discoverer_.RemoveSearch(id);
}

std::optional<BrEdrConnectionManager::ScoRequestHandle> BrEdrConnectionManager::OpenScoConnection(
    PeerId peer_id, bool initiator, hci::SynchronousConnectionParameters parameters,
    ScoConnectionCallback callback) {
  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "Can't open SCO connection to unconnected peer (peer: %s)",
           bt_str(peer_id));
    callback(fit::error(HostError::kNotFound));
    return std::nullopt;
  };

  return conn_pair->second->OpenScoConnection(initiator, parameters, std::move(callback));
}

bool BrEdrConnectionManager::Disconnect(PeerId peer_id, DisconnectReason reason) {
  bt_log(INFO, "gap-bredr", "Disconnect Requested (peer %s, reason %hhu - %s)", bt_str(peer_id),
         reason, ReasonAsString(reason).c_str());

  // TODO(fxbug.dev/65157) - If a disconnect request is received when we have a pending connection,
  // we should instead abort the connection, by either:
  //   * removing the request if it has not yet been processed
  //   * sending a cancel command to the controller and waiting for it to be processed
  //   * sending a cancel command, and if we already complete, then beginning a disconnect procedure
  if (connection_requests_.find(peer_id) != connection_requests_.end()) {
    bt_log(WARN, "gap-bredr", "Can't disconnect peer %s because it's being connected to",
           bt_str(peer_id));
    return false;
  }

  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(INFO, "gap-bredr", "No need to disconnect peer (id: %s): It is not connected",
           bt_str(peer_id));
    return true;
  }

  auto [handle, connection] = *conn_pair;
  CleanUpConnection(handle, std::move(connections_.extract(handle).mapped()));
  return true;
}

void BrEdrConnectionManager::WritePageScanSettings(uint16_t interval, uint16_t window,
                                                   bool interlaced, hci::StatusCallback cb) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  if (!hci_cmd_runner_->IsReady()) {
    // TODO(jamuraa): could run the three "settings" commands in parallel and
    // remove the sequence runner.
    cb(hci::Status(HostError::kInProgress));
    return;
  }

  auto write_activity = hci::CommandPacket::New(hci::kWritePageScanActivity,
                                                sizeof(hci::WritePageScanActivityCommandParams));
  auto* activity_params =
      write_activity->mutable_payload<hci::WritePageScanActivityCommandParams>();
  activity_params->page_scan_interval = htole16(interval);
  activity_params->page_scan_window = htole16(window);

  hci_cmd_runner_->QueueCommand(
      std::move(write_activity), [self, interval, window](const hci::EventPacket& event) {
        if (!self || hci_is_error(event, WARN, "gap-bredr", "write page scan activity failed")) {
          return;
        }

        self->page_scan_interval_ = interval;
        self->page_scan_window_ = window;

        bt_log(TRACE, "gap-bredr", "page scan activity updated");
      });

  auto write_type =
      hci::CommandPacket::New(hci::kWritePageScanType, sizeof(hci::WritePageScanTypeCommandParams));
  auto* type_params = write_type->mutable_payload<hci::WritePageScanTypeCommandParams>();
  type_params->page_scan_type =
      (interlaced ? hci::PageScanType::kInterlacedScan : hci::PageScanType::kStandardScan);

  hci_cmd_runner_->QueueCommand(
      std::move(write_type), [self, interlaced](const hci::EventPacket& event) {
        if (!self || hci_is_error(event, WARN, "gap-bredr", "write page scan type failed")) {
          return;
        }

        self->page_scan_type_ =
            (interlaced ? hci::PageScanType::kInterlacedScan : hci::PageScanType::kStandardScan);

        bt_log(TRACE, "gap-bredr", "page scan type updated");
      });

  hci_cmd_runner_->RunCommands(std::move(cb));
}

std::optional<std::pair<hci::ConnectionHandle, BrEdrConnection*>>
BrEdrConnectionManager::FindConnectionById(PeerId peer_id) {
  auto it = std::find_if(connections_.begin(), connections_.end(),
                         [peer_id](const auto& c) { return c.second.peer_id() == peer_id; });

  if (it == connections_.end()) {
    return std::nullopt;
  }

  auto& [handle, conn] = *it;
  ZX_ASSERT(conn.link().ll_type() != hci::Connection::LinkType::kLE);

  return std::pair(handle, &conn);
}

std::optional<std::pair<hci::ConnectionHandle, BrEdrConnection*>>
BrEdrConnectionManager::FindConnectionByAddress(const DeviceAddressBytes& bd_addr) {
  auto* const peer = cache_->FindByAddress(DeviceAddress(DeviceAddress::Type::kBREDR, bd_addr));
  if (!peer) {
    return std::nullopt;
  }

  return FindConnectionById(peer->identifier());
}

Peer* BrEdrConnectionManager::FindOrInitPeer(DeviceAddress addr) {
  Peer* peer = cache_->FindByAddress(addr);
  if (!peer) {
    peer = cache_->NewPeer(addr, /*connectable*/ true);
  }
  return peer;
}

// Build connection state for a new connection and begin interrogation. L2CAP is not enabled for
// this link but pairing is allowed before interrogation completes.
void BrEdrConnectionManager::InitializeConnection(DeviceAddress addr,
                                                  hci::ConnectionHandle connection_handle) {
  // TODO(fxbug.dev/881): support non-master connections.
  auto link = hci::Connection::CreateACL(connection_handle, hci::Connection::Role::kMaster,
                                         local_address_, addr, hci_);
  Peer* const peer = FindOrInitPeer(addr);
  auto peer_id = peer->identifier();
  bt_log(INFO, "gap-bredr", "Beginning interrogation for peer %s", bt_str(peer_id));

  // We should never have more than one link to a given peer
  ZX_DEBUG_ASSERT(!FindConnectionById(peer_id));
  peer->MutBrEdr().SetConnectionState(ConnectionState::kInitializing);

  // The controller has completed the HCI connection procedure, so the connection request can no
  // longer be failed by a lower layer error. Now tie error reporting of the request to the lifetime
  // of the connection state object (BrEdrConnection RAII).
  auto node = connection_requests_.extract(peer_id);
  auto request = node ? std::optional(std::move(node.mapped())) : std::nullopt;

  const hci::ConnectionHandle handle = link->handle();
  auto send_auth_request_cb = [this, handle]() {
    this->SendAuthenticationRequested(handle, [handle](auto status) {
      bt_is_error(status, WARN, "gap-bredr", "authentication requested command failed for %#.4x",
                  handle);
    });
  };
  auto disconnect_cb = [this, peer_id] { Disconnect(peer_id, DisconnectReason::kPairingFailed); };
  auto on_peer_disconnect_cb =
      std::bind(&BrEdrConnectionManager::OnPeerDisconnect, this, link.get());
  auto [conn_iter, success] = connections_.try_emplace(
      handle, peer_id, std::move(link), std::move(send_auth_request_cb), std::move(disconnect_cb),
      std::move(on_peer_disconnect_cb), cache_, l2cap_, hci_, std::move(request));
  ZX_ASSERT(success);

  BrEdrConnection& connection = conn_iter->second;
  connection.pairing_state().SetPairingDelegate(pairing_delegate_);

  // Interrogate this peer to find out its version/capabilities.
  auto self = weak_ptr_factory_.GetWeakPtr();
  interrogator_.Start(peer->identifier(), handle, [peer, self, handle](auto status) {
    if (!self) {
      return;
    }
    bt_log_scope("peer: %s, handle: %#.4x", bt_str(peer->identifier()), handle);
    if (bt_is_error(status, WARN, "gap-bredr", "interrogation failed, dropping connection")) {
      // If this connection was locally requested, requester(s) are notified by the disconnection.
      self->Disconnect(peer->identifier(), DisconnectReason::kInterrogationFailed);
      return;
    }
    bt_log(INFO, "gap-bredr", "interrogation complete");
    self->CompleteConnectionSetup(peer, handle);
  });

  // If this was our in-flight request, close it
  if (pending_request_.has_value() && addr == pending_request_->peer_address()) {
    pending_request_.reset();
  }

  TryCreateNextConnection();
}

// Finish connection setup after a successful interrogation.
void BrEdrConnectionManager::CompleteConnectionSetup(Peer* peer, hci::ConnectionHandle handle) {
  auto self = weak_ptr_factory_.GetWeakPtr();

  auto connections_iter = connections_.find(handle);
  if (connections_iter == connections_.end()) {
    bt_log(WARN, "gap-bredr", "Connection to complete not found, handle: %#.4x", handle);
    return;
  }
  BrEdrConnection& conn_state = connections_iter->second;
  if (conn_state.peer_id() != peer->identifier()) {
    bt_log(WARN, "gap-bredr",
           "Connection %#.4x is no longer to peer %s (now to %s), ignoring interrogation result",
           handle, bt_str(peer->identifier()), bt_str(conn_state.peer_id()));
    return;
  }
  hci::Connection* const connection = &conn_state.link();

  auto error_handler = [self, peer_id = peer->identifier(), connection = connection->WeakPtr()] {
    if (!self || !connection)
      return;
    bt_log(WARN, "gap-bredr", "Link error received, closing connection (peer: %s, handle: %#.4x)",
           bt_str(peer_id), connection->handle());

    self->Disconnect(peer_id, DisconnectReason::kAclLinkError);
  };

  // TODO(fxbug.dev/37650): Implement this callback as a call to InitiatePairing().
  auto security_callback = [](hci::ConnectionHandle handle, sm::SecurityLevel level, auto cb) {
    bt_log(INFO, "gap-bredr", "Ignoring security upgrade request; not implemented");
    cb(sm::Status(HostError::kNotSupported));
  };

  // Register with L2CAP to handle services on the ACL signaling channel.
  l2cap_->AddACLConnection(handle, connection->role(), error_handler, std::move(security_callback));

  peer->MutBrEdr().SetConnectionState(ConnectionState::kConnected);

  if (discoverer_.search_count()) {
    l2cap_->OpenL2capChannel(handle, l2cap::kSDP, l2cap::ChannelParameters(),
                             [self, peer_id = peer->identifier()](auto channel) {
                               if (!self)
                                 return;

                               if (!channel) {
                                 bt_log(ERROR, "gap",
                                        "failed to create l2cap channel for SDP (peer id: %s)",
                                        bt_str(peer_id));
                                 return;
                               }

                               auto client = sdp::Client::Create(std::move(channel));
                               self->discoverer_.StartServiceDiscovery(peer_id, std::move(client));
                             });
  }

  conn_state.Start();
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnAuthenticationComplete(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kAuthenticationCompleteEventCode);
  const auto& params = event.params<hci::AuthenticationCompleteEventParams>();

  auto iter = connections_.find(params.connection_handle);
  if (iter == connections_.end()) {
    bt_log(INFO, "gap-bredr",
           "ignoring authentication complete for unknown connection handle %#.04x",
           params.connection_handle);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  hci::StatusCode status_code;
  event.ToStatusCode(&status_code);
  iter->second.pairing_state().OnAuthenticationComplete(status_code);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

bool BrEdrConnectionManager::ExistsIncomingRequest(PeerId id) {
  auto request = connection_requests_.find(id);
  return (request != connection_requests_.end() && request->second.HasIncoming());
}

void BrEdrConnectionManager::OnConnectionRequest(ConnectionRequestEvent event) {
  // Initialize the peer if it doesn't exist, to ensure we have allocated a PeerId
  auto peer = FindOrInitPeer(event.addr);
  auto peer_id = peer->identifier();
  bt_log_scope("peer: %s, addr: %s, link_type: %s, class: %s", bt_str(peer_id), bt_str(event.addr),
               LinkTypeToString(event.link_type).c_str(), bt_str(event.class_of_device));

  // In case of concurrent incoming requests from the same peer, reject all but the first
  if (ExistsIncomingRequest(peer_id)) {
    bt_log(WARN, "gap-bredr", "rejecting duplicate incoming connection request");
    SendRejectConnectionRequest(event.addr, hci::StatusCode::kConnectionRejectedBadBdAddr);
    return;
  }

  if (event.link_type == hci::LinkType::kACL) {
    // If we happen to be already connected (for example, if our outgoing raced, or we received
    // duplicate requests), we reject the request with 'ConnectionAlreadyExists'
    if (FindConnectionById(peer_id)) {
      bt_log(WARN, "gap-bredr", "rejecting incoming connection request; already connected");
      SendRejectConnectionRequest(event.addr, hci::StatusCode::kConnectionAlreadyExists);
      return;
    }

    // Accept the connection, performing a role switch. We receive a Connection Complete event
    // when the connection is complete, and finish the link then.
    bt_log(INFO, "gap-bredr", "accepting incoming connection");

    // Register that we're in the middle of an incoming request for this peer - create a new
    // request if one doesn't already exist
    auto [request, _ignore] = connection_requests_.try_emplace(peer_id, event.addr);
    request->second.BeginIncoming();

    SendAcceptConnectionRequest(
        event.addr.value(),
        [addr = event.addr, self = weak_ptr_factory_.GetWeakPtr(), peer_id](auto status) {
          if (self && !status)
            self->CompleteRequest(peer_id, addr, status, /*handle=*/0);
        });

    return;
  }

  if (event.link_type == hci::LinkType::kSCO || event.link_type == hci::LinkType::kExtendedSCO) {
    auto conn_pair = FindConnectionByAddress(event.addr.value());
    if (conn_pair) {
      // The ScoConnectionManager owned by the BrEdrConnection will respond.
      bt_log(INFO, "gap-bredr", "delegating incoming SCO connection to ScoConnectionManager");
      return;
    }
    bt_log(WARN, "gap-bredr", "rejecting (e)SCO connection request for peer that is not connected");
    SendRejectSynchronousRequest(event.addr, hci::StatusCode::kUnacceptableConnectionParameters);
  } else {
    auto link_type = static_cast<unsigned int>(event.link_type);
    bt_log(WARN, "gap-bredr", "reject unsupported connection type %u", link_type);
    SendRejectConnectionRequest(event.addr, hci::StatusCode::kUnsupportedFeatureOrParameter);
  }
}

void BrEdrConnectionManager::OnConnectionComplete(ConnectionComplete event) {
  if (event.link_type != hci::LinkType::kACL) {
    // Only ACL links are processed
    return;
  }

  // Initialize the peer if it doesn't exist, to ensure we have allocated a PeerId (we should
  // usually have a peer by this point)
  auto peer = FindOrInitPeer(event.addr);

  CompleteRequest(peer->identifier(), event.addr, event.status, event.handle);
}

// A request for a connection - from an upstream client _or_ a remote peer - completed, successfully
// or not. This may be due to a ConnectionComplete event being received, or due to a CommandStatus
// response being received in response to a CreateConnection command
void BrEdrConnectionManager::CompleteRequest(PeerId peer_id, DeviceAddress address,
                                             hci::Status status, hci::ConnectionHandle handle) {
  bt_log_scope("peer: %s, addr: %s, handle: %#.4x", bt_str(peer_id), bt_str(address), handle);

  auto req_iter = connection_requests_.find(peer_id);
  if (req_iter == connection_requests_.end()) {
    // This could potentially happen if the peer expired from the peer cache during the connection
    // procedure
    bt_log(INFO, "gap-bredr",
           "ConnectionComplete received for address with no known request (status: %s)",
           bt_str(status));
    return;
  }
  auto& request = req_iter->second;

  bool completed_request_was_outgoing =
      pending_request_ && pending_request_->peer_address() == address;
  bool failed = !status.is_success();

  const char* direction = completed_request_was_outgoing ? "outgoing" : "incoming";
  const char* result = status ? "complete" : "error";
  bt_log(INFO, "gap-bredr", "%s connection %s (status: %s)", direction, result, bt_str(status));

  if (completed_request_was_outgoing) {
    // Determine the modified status in case of cancellation or timeout
    status = pending_request_->CompleteRequest(status);
    pending_request_.reset();
  } else {
    // If this was an incoming attempt, clear it
    request.CompleteIncoming();
  }

  if (failed) {
    if (request.HasIncoming() || (!completed_request_was_outgoing && request.AwaitingOutgoing())) {
      // This request failed, but we're still waiting on either:
      // * an in-progress incoming request or
      // * to attempt our own outgoing request
      // Therefore we don't notify yet - instead take no action, and wait until we finish those
      // steps before completing the request and notifying callbacks
      TryCreateNextConnection();
      return;
    }
    request.NotifyCallbacks(status, [] { return nullptr; });
    connection_requests_.erase(req_iter);

    // The peer may no longer be in the cache by the time this function is called
    // TODO(fxbug.dev/70878): What if this request failed but a previous one succeeded? This is a
    // potential race condition in tracking peer state
    Peer* peer = cache_->FindByAddress(address);
    if (peer) {
      peer->MutBrEdr().SetConnectionState(ConnectionState::kNotConnected);
    }
  } else {
    // Callbacks will be notified when interrogation completes
    InitializeConnection(address, handle);
  }

  TryCreateNextConnection();
}

void BrEdrConnectionManager::OnPeerDisconnect(const hci::Connection* connection) {
  auto handle = connection->handle();

  auto it = connections_.find(handle);
  if (it == connections_.end()) {
    bt_log(WARN, "gap-bredr", "disconnect from unknown connection handle %#.4x", handle);
    return;
  }

  auto conn = std::move(it->second);
  connections_.erase(it);

  bt_log(INFO, "gap-bredr", "peer disconnected (peer: %s, handle: %#.4x)", bt_str(conn.peer_id()),
         handle);
  CleanUpConnection(handle, std::move(conn));
}

void BrEdrConnectionManager::CleanUpConnection(hci::ConnectionHandle handle, BrEdrConnection conn) {
  auto* peer = cache_->FindByAddress(conn.link().peer_address());
  ZX_DEBUG_ASSERT_MSG(peer, "Couldn't find peer for handle: %#.4x", handle);
  peer->MutBrEdr().SetConnectionState(ConnectionState::kNotConnected);

  l2cap_->RemoveConnection(handle);

  // |conn| is destroyed when it goes out of scope.
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnIoCapabilityRequest(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kIOCapabilityRequestEventCode);
  const auto& params = event.params<hci::IOCapabilityRequestEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(ERROR, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    SendIoCapabilityRequestNegativeReply(params.bd_addr, hci::StatusCode::kPairingNotAllowed);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  auto [handle, conn_ptr] = *conn_pair;
  auto reply = conn_ptr->pairing_state().OnIoCapabilityRequest();

  if (!reply) {
    SendIoCapabilityRequestNegativeReply(params.bd_addr, hci::StatusCode::kPairingNotAllowed);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  const hci::IOCapability io_capability = *reply;

  // TODO(fxbug.dev/601): Add OOB status from PeerCache.
  const uint8_t oob_data_present = 0x00;  // None present.

  // TODO(fxbug.dev/1249): Determine this based on the service requirements.
  const hci::AuthRequirements auth_requirements =
      io_capability == hci::IOCapability::kNoInputNoOutput
          ? hci::AuthRequirements::kGeneralBonding
          : hci::AuthRequirements::kMITMGeneralBonding;

  SendIoCapabilityRequestReply(params.bd_addr, io_capability, oob_data_present, auth_requirements);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnIoCapabilityResponse(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kIOCapabilityResponseEventCode);
  const auto& params = event.params<hci::IOCapabilityResponseEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(INFO, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  conn_pair->second->pairing_state().OnIoCapabilityResponse(params.io_capability);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnLinkKeyRequest(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kLinkKeyRequestEventCode);
  const auto& params = event.params<hci::LinkKeyRequestParams>();

  DeviceAddress addr(DeviceAddress::Type::kBREDR, params.bd_addr);
  auto* peer = cache_->FindByAddress(addr);
  if (!peer) {
    bt_log(WARN, "gap-bredr", "no peer with address %s found", bt_str(addr));
    SendLinkKeyRequestNegativeReply(params.bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto peer_id = peer->identifier();
  auto conn_pair = FindConnectionById(peer_id);

  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "can't find connection for ltk (id: %s)", bt_str(peer_id));
    SendLinkKeyRequestNegativeReply(params.bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  auto& [handle, conn] = *conn_pair;

  auto link_key = conn->pairing_state().OnLinkKeyRequest(addr);
  if (!link_key.has_value()) {
    SendLinkKeyRequestNegativeReply(params.bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  SendLinkKeyRequestReply(params.bd_addr, link_key.value());
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnLinkKeyNotification(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kLinkKeyNotificationEventCode);
  const auto& params = event.params<hci::LinkKeyNotificationEventParams>();

  DeviceAddress addr(DeviceAddress::Type::kBREDR, params.bd_addr);

  auto* peer = cache_->FindByAddress(addr);
  if (!peer) {
    bt_log(WARN, "gap-bredr",
           "no known peer with address %s found; link key not stored (key type: %u)", bt_str(addr),
           params.key_type);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  bt_log(INFO, "gap-bredr", "got link key notification (key type: %u, peer: %s)", params.key_type,
         bt_str(peer->identifier()));

  auto key_type = hci::LinkKeyType{params.key_type};
  sm::SecurityProperties sec_props;
  if (key_type == hci::LinkKeyType::kChangedCombination) {
    if (!peer->bredr() || !peer->bredr()->bonded()) {
      bt_log(WARN, "gap-bredr", "can't update link key of unbonded peer %s",
             bt_str(peer->identifier()));
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }

    // Reuse current properties
    ZX_DEBUG_ASSERT(peer->bredr()->link_key());
    sec_props = peer->bredr()->link_key()->security();
    key_type = *sec_props.GetLinkKeyType();
  } else {
    sec_props = sm::SecurityProperties(key_type);
  }

  auto peer_id = peer->identifier();

  if (sec_props.level() == sm::SecurityLevel::kNoSecurity) {
    bt_log(WARN, "gap-bredr", "link key for peer %s has insufficient security; not stored",
           bt_str(peer_id));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  UInt128 key_value;
  std::copy(params.link_key, &params.link_key[key_value.size()], key_value.begin());
  hci::LinkKey hci_key(key_value, 0, 0);
  sm::LTK key(sec_props, hci_key);

  auto handle = FindConnectionById(peer_id);
  if (!handle) {
    bt_log(WARN, "gap-bredr", "can't find current connection for ltk (peer: %s)", bt_str(peer_id));
  } else {
    handle->second->link().set_bredr_link_key(hci_key, key_type);
    handle->second->pairing_state().OnLinkKeyNotification(key_value, key_type);
  }

  if (!cache_->StoreBrEdrBond(addr, key)) {
    bt_log(ERROR, "gap-bredr", "failed to cache bonding data (peer: %s)", bt_str(peer_id));
  }
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnSimplePairingComplete(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kSimplePairingCompleteEventCode);
  const auto& params = event.params<hci::SimplePairingCompleteEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  conn_pair->second->pairing_state().OnSimplePairingComplete(params.status);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnUserConfirmationRequest(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kUserConfirmationRequestEventCode);
  const auto& params = event.params<hci::UserConfirmationRequestEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    SendUserConfirmationRequestNegativeReply(params.bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto confirm_cb = [this, self = weak_ptr_factory_.GetWeakPtr(),
                     bd_addr = params.bd_addr](bool confirm) {
    if (!self) {
      return;
    }

    if (confirm) {
      SendUserConfirmationRequestReply(bd_addr);
    } else {
      SendUserConfirmationRequestNegativeReply(bd_addr);
    }
  };
  conn_pair->second->pairing_state().OnUserConfirmationRequest(letoh32(params.numeric_value),
                                                               std::move(confirm_cb));
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnUserPasskeyRequest(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kUserPasskeyRequestEventCode);
  const auto& params = event.params<hci::UserPasskeyRequestEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    SendUserPasskeyRequestNegativeReply(params.bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto passkey_cb = [this, self = weak_ptr_factory_.GetWeakPtr(),
                     bd_addr = params.bd_addr](std::optional<uint32_t> passkey) {
    if (!self) {
      return;
    }

    if (passkey) {
      SendUserPasskeyRequestReply(bd_addr, *passkey);
    } else {
      SendUserPasskeyRequestNegativeReply(bd_addr);
    }
  };
  conn_pair->second->pairing_state().OnUserPasskeyRequest(std::move(passkey_cb));
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnUserPasskeyNotification(
    const hci::EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci::kUserPasskeyNotificationEventCode);
  const auto& params = event.params<hci::UserPasskeyNotificationEventParams>();

  auto conn_pair = FindConnectionByAddress(params.bd_addr);
  if (!conn_pair) {
    bt_log(WARN, "gap-bredr", "got %s for unconnected addr %s", __func__, bt_str(params.bd_addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  conn_pair->second->pairing_state().OnUserPasskeyNotification(letoh32(params.numeric_value));
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

bool BrEdrConnectionManager::Connect(PeerId peer_id, ConnectResultCallback on_connection_result) {
  Peer* peer = cache_->FindById(peer_id);
  if (!peer) {
    bt_log(WARN, "gap-bredr", "%s: peer not found (peer: %s)", __func__, bt_str(peer_id));
    return false;
  }

  if (peer->technology() == TechnologyType::kLowEnergy) {
    bt_log(ERROR, "gap-bredr", "peer does not support BrEdr: %s", bt_str(*peer));
    return false;
  }

  // Br/Edr peers should always be connectable by definition
  ZX_ASSERT(peer->connectable());

  // Succeed immediately or after interrogation if there is already an active connection.
  auto conn = FindConnectionById(peer_id);
  if (conn) {
    conn->second->AddRequestCallback(std::move(on_connection_result));
    return true;
  }

  // If we are already waiting to connect to |peer_id| then we store
  // |on_connection_result| to be processed after the connection attempt
  // completes (in either success of failure).
  auto pending_iter = connection_requests_.find(peer_id);
  if (pending_iter != connection_requests_.end()) {
    pending_iter->second.AddCallback(std::move(on_connection_result));
    return true;
  }
  // If we are not already connected or pending, initiate a new connection
  peer->MutBrEdr().SetConnectionState(ConnectionState::kInitializing);
  connection_requests_.try_emplace(peer_id, peer->address(), std::move(on_connection_result));

  TryCreateNextConnection();

  return true;
}

std::optional<BrEdrConnectionManager::CreateConnectionParams>
BrEdrConnectionManager::NextCreateConnectionParams() {
  if (connection_requests_.empty()) {
    bt_log(TRACE, "gap-bredr", "no pending requests remaining");
    return std::nullopt;
  }

  Peer* peer = nullptr;
  // We use a rough heuristic of ordering likely connection requests by presence in the peer cache.
  // If a peer is still in the cache, that implies it was seen more recently which is likely to
  // correlate with being physically close and therefore still in range when we attempt to connect.
  //
  // So first try a request for which we have a peer struct:
  for (auto& [identifier, request] : connection_requests_) {
    const auto& addr = request.address();
    peer = cache_->FindByAddress(addr);
    if (peer && peer->bredr() && !request.HasIncoming())
      return std::optional(CreateConnectionParams{peer->identifier(), addr,
                                                  peer->bredr()->clock_offset(),
                                                  peer->bredr()->page_scan_repetition_mode()});
  }

  // Otherwise, fall back to any other requests - it is entirely possible that while a connection is
  // pending, discovery has ended and the peer which was intended to be connected to has timed out
  // of the peer cache, but may still be in range and connectable.
  for (auto& [identifier, request] : connection_requests_) {
    if (!request.HasIncoming()) {
      return std::optional(
          CreateConnectionParams{identifier, request.address(), std::nullopt, std::nullopt});
    }
  }
  // Finally, if we didn't find a connection request we could process at this time:
  return std::nullopt;
}

void BrEdrConnectionManager::TryCreateNextConnection() {
  // There can only be one outstanding BrEdr CreateConnection request at a time
  if (pending_request_) {
    return;
  }

  auto next = NextCreateConnectionParams();
  if (next) {
    InitiatePendingConnection(*next);
  }
}

void BrEdrConnectionManager::InitiatePendingConnection(CreateConnectionParams params) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto on_failure = [self, addr = params.addr](hci::Status status, auto peer_id) {
    if (self && !status)
      self->CompleteRequest(peer_id, addr, status, /*handle=*/0);
  };
  auto on_timeout = [self] {
    if (self)
      self->OnRequestTimeout();
  };
  pending_request_.emplace(params.peer_id, params.addr, on_timeout);
  pending_request_->CreateConnection(hci_->command_channel(), dispatcher_, params.clock_offset,
                                     params.page_scan_repetition_mode, request_timeout_,
                                     on_failure);
}

void BrEdrConnectionManager::OnRequestTimeout() {
  if (pending_request_) {
    pending_request_->Timeout();
    SendCreateConnectionCancelCommand(pending_request_->peer_address());
  }
}

void BrEdrConnectionManager::SendCreateConnectionCancelCommand(DeviceAddress addr) {
  auto cancel = hci::CommandPacket::New(hci::kCreateConnectionCancel,
                                        sizeof(hci::CreateConnectionCancelCommandParams));
  auto params = cancel->mutable_payload<hci::CreateConnectionCancelCommandParams>();
  params->bd_addr = addr.value();
  hci_->command_channel()->SendCommand(std::move(cancel), [](auto, const hci::EventPacket& event) {
    hci_is_error(event, WARN, "hci-bredr", "failed to cancel connection request");
  });
}

void BrEdrConnectionManager::SendAuthenticationRequested(hci::ConnectionHandle handle,
                                                         hci::StatusCallback cb) {
  auto auth_request = hci::CommandPacket::New(hci::kAuthenticationRequested,
                                              sizeof(hci::AuthenticationRequestedCommandParams));
  auth_request->mutable_payload<hci::AuthenticationRequestedCommandParams>()->connection_handle =
      htole16(handle);

  // Complete on command status because Authentication Complete Event is already registered.
  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [cb = std::move(cb)](auto, const hci::EventPacket& event) {
      cb(event.ToStatus());
    };
  }
  hci_->command_channel()->SendCommand(std::move(auth_request), std::move(command_cb),
                                       hci::kCommandStatusEventCode);
}

void BrEdrConnectionManager::SendIoCapabilityRequestReply(DeviceAddressBytes bd_addr,
                                                          hci::IOCapability io_capability,
                                                          uint8_t oob_data_present,
                                                          hci::AuthRequirements auth_requirements,
                                                          hci::StatusCallback cb) {
  auto packet = hci::CommandPacket::New(hci::kIOCapabilityRequestReply,
                                        sizeof(hci::IOCapabilityRequestReplyCommandParams));
  auto params = packet->mutable_payload<hci::IOCapabilityRequestReplyCommandParams>();
  params->bd_addr = bd_addr;
  params->io_capability = io_capability;
  params->oob_data_present = oob_data_present;
  params->auth_requirements = auth_requirements;
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendIoCapabilityRequestNegativeReply(DeviceAddressBytes bd_addr,
                                                                  hci::StatusCode reason,
                                                                  hci::StatusCallback cb) {
  auto packet = hci::CommandPacket::New(hci::kIOCapabilityRequestNegativeReply,
                                        sizeof(hci::IOCapabilityRequestNegativeReplyCommandParams));
  auto params = packet->mutable_payload<hci::IOCapabilityRequestNegativeReplyCommandParams>();
  params->bd_addr = bd_addr;
  params->reason = reason;
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserConfirmationRequestReply(DeviceAddressBytes bd_addr,
                                                              hci::StatusCallback cb) {
  auto packet = hci::CommandPacket::New(hci::kUserConfirmationRequestReply,
                                        sizeof(hci::UserConfirmationRequestReplyCommandParams));
  packet->mutable_payload<hci::UserConfirmationRequestReplyCommandParams>()->bd_addr = bd_addr;
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserConfirmationRequestNegativeReply(DeviceAddressBytes bd_addr,
                                                                      hci::StatusCallback cb) {
  auto packet =
      hci::CommandPacket::New(hci::kUserConfirmationRequestNegativeReply,
                              sizeof(hci::UserConfirmationRequestNegativeReplyCommandParams));
  packet->mutable_payload<hci::UserConfirmationRequestNegativeReplyCommandParams>()->bd_addr =
      bd_addr;
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserPasskeyRequestReply(DeviceAddressBytes bd_addr,
                                                         uint32_t numeric_value,
                                                         hci::StatusCallback cb) {
  auto packet = hci::CommandPacket::New(hci::kUserPasskeyRequestReply,
                                        sizeof(hci::UserPasskeyRequestReplyCommandParams));
  auto params = packet->mutable_payload<hci::UserPasskeyRequestReplyCommandParams>();
  params->bd_addr = bd_addr;
  params->numeric_value = htole32(numeric_value);
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserPasskeyRequestNegativeReply(DeviceAddressBytes bd_addr,
                                                                 hci::StatusCallback cb) {
  auto packet = hci::CommandPacket::New(hci::kUserPasskeyRequestNegativeReply,
                                        sizeof(hci::UserPasskeyRequestNegativeReplyCommandParams));
  packet->mutable_payload<hci::UserPasskeyRequestNegativeReplyCommandParams>()->bd_addr = bd_addr;
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendLinkKeyRequestNegativeReply(DeviceAddressBytes bd_addr,
                                                             hci::StatusCallback cb) {
  auto negative_reply = hci::CommandPacket::New(
      hci::kLinkKeyRequestNegativeReply, sizeof(hci::LinkKeyRequestNegativeReplyCommandParams));
  auto negative_reply_params =
      negative_reply->mutable_payload<hci::LinkKeyRequestNegativeReplyCommandParams>();
  negative_reply_params->bd_addr = bd_addr;
  SendCommandWithStatusCallback(std::move(negative_reply), std::move(cb));
}

void BrEdrConnectionManager::SendLinkKeyRequestReply(DeviceAddressBytes bd_addr,
                                                     hci::LinkKey link_key,
                                                     hci::StatusCallback cb) {
  auto reply = hci::CommandPacket::New(hci::kLinkKeyRequestReply,
                                       sizeof(hci::LinkKeyRequestReplyCommandParams));
  auto reply_params = reply->mutable_payload<hci::LinkKeyRequestReplyCommandParams>();
  reply_params->bd_addr = bd_addr;
  const auto& key_value = link_key.value();
  std::copy(key_value.begin(), key_value.end(), reply_params->link_key);
  SendCommandWithStatusCallback(std::move(reply), std::move(cb));
}

void BrEdrConnectionManager::SendCommandWithStatusCallback(
    std::unique_ptr<hci::CommandPacket> command_packet, hci::StatusCallback cb) {
  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [cb = std::move(cb)](auto, const hci::EventPacket& event) {
      cb(event.ToStatus());
    };
  }
  hci_->command_channel()->SendCommand(std::move(command_packet), std::move(command_cb));
}

void BrEdrConnectionManager::SendAcceptConnectionRequest(DeviceAddressBytes addr,
                                                         hci::StatusCallback cb) {
  auto accept = hci::CommandPacket::New(hci::kAcceptConnectionRequest,
                                        sizeof(hci::AcceptConnectionRequestCommandParams));
  auto accept_params = accept->mutable_payload<hci::AcceptConnectionRequestCommandParams>();
  accept_params->bd_addr = addr;
  accept_params->role = hci::ConnectionRole::kMaster;

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [cb = std::move(cb)](auto, const hci::EventPacket& event) {
      cb(event.ToStatus());
    };
  }

  hci_->command_channel()->SendCommand(std::move(accept), std::move(command_cb),
                                       hci::kCommandStatusEventCode);
}

void BrEdrConnectionManager::SendRejectConnectionRequest(DeviceAddress addr, hci::StatusCode reason,
                                                         hci::StatusCallback cb) {
  auto reject = hci::CommandPacket::New(hci::kRejectConnectionRequest,
                                        sizeof(hci::RejectConnectionRequestCommandParams));
  auto reject_params = reject->mutable_payload<hci::RejectConnectionRequestCommandParams>();
  reject_params->bd_addr = addr.value();
  reject_params->reason = reason;

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [cb = std::move(cb)](auto, const hci::EventPacket& event) {
      cb(event.ToStatus());
    };
  }

  hci_->command_channel()->SendCommand(std::move(reject), std::move(command_cb),
                                       hci::kCommandStatusEventCode);
}

void BrEdrConnectionManager::SendRejectSynchronousRequest(DeviceAddress addr,
                                                          hci::StatusCode reason,
                                                          hci::StatusCallback cb) {
  auto reject =
      hci::CommandPacket::New(hci::kRejectSynchronousConnectionRequest,
                              sizeof(hci::RejectSynchronousConnectionRequestCommandParams));
  auto reject_params =
      reject->mutable_payload<hci::RejectSynchronousConnectionRequestCommandParams>();
  reject_params->bd_addr = addr.value();
  reject_params->reason = reason;

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [cb = std::move(cb)](auto, const hci::EventPacket& event) {
      cb(event.ToStatus());
    };
  }

  hci_->command_channel()->SendCommand(std::move(reject), std::move(command_cb),
                                       hci::kCommandStatusEventCode);
}

}  // namespace bt::gap
