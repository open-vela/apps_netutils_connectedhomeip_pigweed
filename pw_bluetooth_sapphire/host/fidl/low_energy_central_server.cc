// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "low_energy_central_server.h"

#include <zircon/assert.h>
#include <zircon/types.h>

#include <measure_tape/hlcpp/hlcpp_measure_tape_for_peer.h>

#include "gatt_client_server.h"
#include "helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/types.h"

using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Int8;
using fuchsia::bluetooth::Status;

using bt::sm::BondableMode;
using fuchsia::bluetooth::gatt::Client;
using fuchsia::bluetooth::le::ScanFilterPtr;
namespace fble = fuchsia::bluetooth::le;
namespace measure_fble = measure_tape::fuchsia::bluetooth::le;

namespace bthost {

LowEnergyCentralServer::LowEnergyCentralServer(fxl::WeakPtr<bt::gap::Adapter> adapter,
                                               fidl::InterfaceRequest<Central> request,
                                               fxl::WeakPtr<bt::gatt::GATT> gatt)
    : AdapterServerBase(adapter, this, std::move(request)),
      gatt_(gatt),
      requesting_scan_deprecated_(false),
      weak_ptr_factory_(this) {
  ZX_ASSERT(gatt_);
}

LowEnergyCentralServer::~LowEnergyCentralServer() {
  if (scan_instance_) {
    scan_instance_->Close(ZX_OK);
    scan_instance_.reset();
  }
}

std::optional<bt::gap::LowEnergyConnectionHandle*> LowEnergyCentralServer::FindConnectionForTesting(
    bt::PeerId identifier) {
  auto conn_iter = connections_.find(identifier);
  if (conn_iter != connections_.end()) {
    return conn_iter->second.get();
  }
  return std::nullopt;
}

LowEnergyCentralServer::ScanResultWatcherServer::ScanResultWatcherServer(
    fxl::WeakPtr<bt::gap::Adapter> adapter,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher> watcher,
    fit::callback<void()> error_cb)
    : ServerBase(this, std::move(watcher)),
      adapter_(std::move(adapter)),
      error_callback_(std::move(error_cb)) {
  set_error_handler([this](auto) {
    bt_log(DEBUG, "fidl", "ScanResultWatcher client closed, stopping scan");
    ZX_ASSERT(error_callback_);
    error_callback_();
  });
}

void LowEnergyCentralServer::ScanResultWatcherServer::Close(zx_status_t epitaph) {
  binding()->Close(epitaph);
}

void LowEnergyCentralServer::ScanResultWatcherServer::AddPeers(
    std::unordered_set<bt::PeerId> peers) {
  while (!peers.empty() && updated_peers_.size() < kMaxPendingScanResultWatcherPeers) {
    updated_peers_.insert(peers.extract(peers.begin()));
  }

  if (!peers.empty()) {
    bt_log(WARN, "fidl", "Maximum pending peers (%zu) reached, dropping %zu peers from results",
           kMaxPendingScanResultWatcherPeers, peers.size());
  }

  MaybeSendPeers();
}

void LowEnergyCentralServer::ScanResultWatcherServer::Watch(WatchCallback callback) {
  bt_log(TRACE, "fidl", "%s", __FUNCTION__);
  if (watch_callback_) {
    bt_log(WARN, "fidl", "%s: called before previous call completed", __FUNCTION__);
    Close(ZX_ERR_CANCELED);
    ZX_ASSERT(error_callback_);
    error_callback_();
    return;
  }
  watch_callback_ = std::move(callback);
  MaybeSendPeers();
}

void LowEnergyCentralServer::ScanResultWatcherServer::MaybeSendPeers() {
  if (updated_peers_.empty() || !watch_callback_) {
    return;
  }

  // Send as many peers as will fit in the channel.
  const size_t kVectorOverhead = sizeof(fidl_message_header_t) + sizeof(fidl_vector_t);
  const size_t kMaxBytes = ZX_CHANNEL_MAX_MSG_BYTES - kVectorOverhead;
  size_t bytes_used = 0;
  std::vector<fble::Peer> peers;
  while (!updated_peers_.empty()) {
    bt::PeerId peer_id = *updated_peers_.begin();
    bt::gap::Peer* peer = adapter_->peer_cache()->FindById(peer_id);
    if (!peer) {
      // The peer has been removed from the peer cache since it was queued, so the stale peer ID
      // should not be sent to the client.
      updated_peers_.erase(peer_id);
      continue;
    }

    fble::Peer fidl_peer = fidl_helpers::PeerToFidlLe(*peer);
    measure_fble::Size peer_size = measure_fble::Measure(fidl_peer);
    ZX_ASSERT_MSG(peer_size.num_handles == 0,
                  "Expected fuchsia.bluetooth.le/Peer to not have handles, but %zu handles found",
                  peer_size.num_handles);
    bytes_used += peer_size.num_bytes;
    if (bytes_used > kMaxBytes) {
      // Don't remove the peer that exceeded the size limit. It will be sent in the next batch.
      break;
    }

    updated_peers_.erase(peer_id);
    peers.emplace_back(std::move(fidl_peer));
  }

  // It is possible that all queued peers were stale, so there is nothing to send.
  if (peers.empty()) {
    return;
  }

  watch_callback_(std::move(peers));
}

LowEnergyCentralServer::ScanInstance::ScanInstance(
    fxl::WeakPtr<bt::gap::Adapter> adapter, LowEnergyCentralServer* central_server,
    std::vector<fuchsia::bluetooth::le::Filter> fidl_filters,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher> watcher, ScanCallback cb)
    : result_watcher_(adapter, std::move(watcher),
                      /*error_cb=*/
                      [this] {
                        Close(ZX_OK);
                        central_server_->ClearScan();
                      }),
      scan_complete_callback_(std::move(cb)),
      central_server_(central_server),
      adapter_(std::move(adapter)),
      weak_ptr_factory_(this) {
  std::transform(fidl_filters.begin(), fidl_filters.end(), std::back_inserter(filters_),
                 fidl_helpers::DiscoveryFilterFromFidl);

  // Send all current peers in peer cache that match filters.
  std::unordered_set<bt::PeerId> initial_peers;
  adapter_->peer_cache()->ForEach(
      [&](const bt::gap::Peer& peer) { initial_peers.emplace(peer.identifier()); });
  FilterAndAddPeers(std::move(initial_peers));

  // Subscribe to updated peers.
  peer_updated_callback_id_ = adapter_->peer_cache()->add_peer_updated_callback(
      [this](const bt::gap::Peer& peer) { FilterAndAddPeers({peer.identifier()}); });

  auto self = weak_ptr_factory_.GetWeakPtr();
  adapter_->le()->StartDiscovery(
      /*active=*/true, [self](auto session) {
        if (!self) {
          bt_log(TRACE, "fidl", "ignoring LE discovery session for canceled Scan");
          return;
        }

        if (!session) {
          bt_log(WARN, "fidl", "failed to start LE discovery session");
          self->Close(ZX_ERR_INTERNAL);
          self->central_server_->ClearScan();
          return;
        }

        session->set_error_callback([self] {
          if (!self) {
            bt_log(TRACE, "fidl", "ignoring LE discovery session error for canceled Scan");
            return;
          }

          bt_log(DEBUG, "fidl", "canceling Scan due to LE discovery session error");
          self->Close(ZX_ERR_INTERNAL);
          self->central_server_->ClearScan();
        });

        self->scan_session_ = std::move(session);
      });
}

LowEnergyCentralServer::ScanInstance::~ScanInstance() {
  // If this scan instance has not already been closed with a more specific status, close with an
  // error status.
  Close(ZX_ERR_INTERNAL);
}

void LowEnergyCentralServer::ScanInstance::Close(zx_status_t status) {
  if (scan_complete_callback_) {
    result_watcher_.Close(status);
    scan_complete_callback_();
  }
}

void LowEnergyCentralServer::ScanInstance::FilterAndAddPeers(std::unordered_set<bt::PeerId> peers) {
  // Remove peers that don't match any filters.
  for (auto peers_iter = peers.begin(); peers_iter != peers.end();) {
    bt::gap::Peer* peer = adapter_->peer_cache()->FindById(*peers_iter);
    if (!peer || !peer->le()) {
      peers_iter = peers.erase(peers_iter);
      continue;
    }
    bool matches_any = false;
    for (const bt::gap::DiscoveryFilter& filter : filters_) {
      // TODO(fxbug.dev/36373): Match peer names that are not in advertising data.
      // This might require implementing a new peer filtering class, as
      // DiscoveryFilter only filters advertising data.
      if (filter.MatchLowEnergyResult(peer->le()->advertising_data(), peer->connectable(),
                                      peer->rssi())) {
        matches_any = true;
        break;
      }
    }
    if (!matches_any) {
      peers_iter = peers.erase(peers_iter);
      continue;
    }
    peers_iter++;
  }

  result_watcher_.AddPeers(std::move(peers));
}

void LowEnergyCentralServer::Scan(
    fuchsia::bluetooth::le::ScanOptions options,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher> result_watcher,
    ScanCallback callback) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);

  if (scan_instance_ || requesting_scan_deprecated_ || scan_session_deprecated_) {
    bt_log(INFO, "fidl", "%s: scan already in progress", __FUNCTION__);
    result_watcher.Close(ZX_ERR_ALREADY_EXISTS);
    callback();
    return;
  }

  if (!options.has_filters() || options.filters().empty()) {
    bt_log(INFO, "fidl", "%s: no scan filters specified", __FUNCTION__);
    result_watcher.Close(ZX_ERR_INVALID_ARGS);
    callback();
    return;
  }

  scan_instance_ = std::make_unique<ScanInstance>(adapter()->AsWeakPtr(), this,
                                                  std::move(*options.mutable_filters()),
                                                  std::move(result_watcher), std::move(callback));
}

void LowEnergyCentralServer::GetPeripherals(::fidl::VectorPtr<::std::string> service_uuids,
                                            GetPeripheralsCallback callback) {
  // TODO:
  bt_log(ERROR, "fidl", "GetPeripherals() not implemented");
}

void LowEnergyCentralServer::GetPeripheral(::std::string identifier,
                                           GetPeripheralCallback callback) {
  // TODO:
  bt_log(ERROR, "fidl", "GetPeripheral() not implemented");
}

void LowEnergyCentralServer::StartScan(ScanFilterPtr filter, StartScanCallback callback) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);

  if (requesting_scan_deprecated_) {
    bt_log(DEBUG, "fidl", "%s: scan request already in progress", __FUNCTION__);
    callback(fidl_helpers::NewFidlError(ErrorCode::IN_PROGRESS, "Scan request in progress"));
    return;
  }

  if (filter && !fidl_helpers::IsScanFilterValid(*filter)) {
    bt_log(WARN, "fidl", "%s: invalid scan filter given", __FUNCTION__);
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS,
                                        "ScanFilter contains an invalid UUID"));
    return;
  }

  if (scan_session_deprecated_) {
    // A scan is already in progress. Update its filter and report success.
    scan_session_deprecated_->filter()->Reset();
    fidl_helpers::PopulateDiscoveryFilter(*filter, scan_session_deprecated_->filter());
    callback(Status());
    return;
  }

  requesting_scan_deprecated_ = true;
  adapter()->le()->StartDiscovery(/*active=*/true, [self = weak_ptr_factory_.GetWeakPtr(),
                                                    filter = std::move(filter),
                                                    callback = std::move(callback),
                                                    func = __FUNCTION__](auto session) {
    if (!self)
      return;

    self->requesting_scan_deprecated_ = false;

    if (!session) {
      bt_log(WARN, "fidl", "%s: failed to start LE discovery session", func);
      callback(fidl_helpers::NewFidlError(ErrorCode::FAILED, "Failed to start discovery session"));
      return;
    }

    // Assign the filter contents if a filter was provided.
    if (filter)
      fidl_helpers::PopulateDiscoveryFilter(*filter, session->filter());

    session->SetResultCallback([self](const auto& peer) {
      if (self)
        self->OnScanResult(peer);
    });

    session->set_error_callback([self] {
      if (self) {
        // Clean up the session and notify the delegate.
        self->StopScan();
      }
    });

    self->scan_session_deprecated_ = std::move(session);
    self->NotifyScanStateChanged(true);
    callback(Status());
  });
}

void LowEnergyCentralServer::StopScan() {
  bt_log(DEBUG, "fidl", "StopScan()");

  if (!scan_session_deprecated_) {
    bt_log(DEBUG, "fidl", "%s: no active discovery session; nothing to do", __FUNCTION__);
    return;
  }

  scan_session_deprecated_ = nullptr;
  NotifyScanStateChanged(false);
}

void LowEnergyCentralServer::ConnectPeripheral(
    ::std::string identifier, fuchsia::bluetooth::le::ConnectionOptions connection_options,
    ::fidl::InterfaceRequest<Client> client_request, ConnectPeripheralCallback callback) {
  bt_log(INFO, "fidl", "%s: (peer: %s)", __FUNCTION__, identifier.c_str());

  auto peer_id = fidl_helpers::PeerIdFromString(identifier);
  if (!peer_id.has_value()) {
    bt_log(WARN, "fidl", "%s: invalid peer id : %s", __FUNCTION__, identifier.c_str());
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS, "invalid peer ID"));
    return;
  }

  auto iter = connections_.find(*peer_id);
  if (iter != connections_.end()) {
    if (iter->second) {
      bt_log(INFO, "fidl", "%s: already connected to %s", __FUNCTION__, bt_str(*peer_id));
      callback(
          fidl_helpers::NewFidlError(ErrorCode::ALREADY, "Already connected to requested peer"));
    } else {
      bt_log(INFO, "fidl", "%s: connect request pending (peer: %s)", __FUNCTION__,
             bt_str(*peer_id));
      callback(fidl_helpers::NewFidlError(ErrorCode::IN_PROGRESS, "Connect request pending"));
    }
    return;
  }

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto conn_cb = [self, callback = callback.share(), peer_id = *peer_id,
                  request = std::move(client_request), func = __FUNCTION__](auto result) mutable {
    if (!self)
      return;

    auto iter = self->connections_.find(peer_id);
    if (iter == self->connections_.end()) {
      bt_log(INFO, "fidl", "%s: connect request canceled during connection procedure (peer: %s)",
             func, bt_str(peer_id));
      auto error = fidl_helpers::NewFidlError(ErrorCode::FAILED, "Connect request canceled");
      callback(std::move(error));
      return;
    }

    if (result.is_error()) {
      bt_log(INFO, "fidl", "%s: failed to connect to peer (peer: %s)", func, bt_str(peer_id));
      self->connections_.erase(peer_id);
      callback(fidl_helpers::StatusToFidlDeprecated(bt::hci::Status(result.error()),
                                                    "failed to connect"));
      return;
    }

    auto conn_ref = result.take_value();
    ZX_ASSERT(conn_ref);
    ZX_ASSERT(peer_id == conn_ref->peer_identifier());

    if (self->gatt_client_servers_.find(peer_id) != self->gatt_client_servers_.end()) {
      bt_log(WARN, "fidl", "only 1 gatt.Client FIDL handle allowed per peer (%s)", bt_str(peer_id));
      // The handle owned by |request| will be closed.
      return;
    }

    auto server = std::make_unique<GattClientServer>(peer_id, self->gatt_, std::move(request));
    server->set_error_handler([self, peer_id](zx_status_t status) {
      if (self) {
        bt_log(DEBUG, "bt-host", "GATT client disconnected");
        self->gatt_client_servers_.erase(peer_id);
      }
    });
    self->gatt_client_servers_.emplace(peer_id, std::move(server));

    conn_ref->set_closed_callback([self, peer_id] {
      if (self && self->connections_.erase(peer_id) != 0) {
        bt_log(INFO, "fidl", "peripheral connection closed (peer: %s)", bt_str(peer_id));
        self->gatt_client_servers_.erase(peer_id);
        self->NotifyPeripheralDisconnected(peer_id);
      }
    });

    ZX_ASSERT(!iter->second);
    iter->second = std::move(conn_ref);
    callback(Status());
  };
  BondableMode bondable_mode =
      (!connection_options.has_bondable_mode() || connection_options.bondable_mode())
          ? BondableMode::Bondable
          : BondableMode::NonBondable;
  std::optional<bt::UUID> service_uuid =
      connection_options.has_service_filter()
          ? std::optional(fidl_helpers::UuidFromFidl(connection_options.service_filter()))
          : std::nullopt;
  bt::gap::LowEnergyConnectionOptions mgr_connection_options{.bondable_mode = bondable_mode,
                                                             .service_uuid = service_uuid};

  // An entry for the connection must be created here so that a synchronous call to conn_cb below
  // does not cause conn_cb to treat the connection as cancelled.
  connections_[*peer_id] = nullptr;

  adapter()->le()->Connect(*peer_id, std::move(conn_cb), mgr_connection_options);
}

void LowEnergyCentralServer::DisconnectPeripheral(::std::string identifier,
                                                  DisconnectPeripheralCallback callback) {
  auto peer_id = fidl_helpers::PeerIdFromString(identifier);
  if (!peer_id.has_value()) {
    bt_log(WARN, "fidl", "%s: invalid peer id : %s", __FUNCTION__, identifier.c_str());
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS, "invalid peer ID"));
    return;
  }

  auto iter = connections_.find(*peer_id);
  if (iter == connections_.end()) {
    bt_log(INFO, "fidl", "%s: client not connected to peer (peer: %s)", __FUNCTION__,
           identifier.c_str());
    callback(Status());
    return;
  }

  // If a request to this peer is pending then the request will be canceled.
  bool was_pending = !iter->second;
  connections_.erase(iter);

  if (was_pending) {
    bt_log(INFO, "fidl", "%s: canceling connection request (peer: %s)", __FUNCTION__,
           bt_str(*peer_id));
  } else {
    gatt_client_servers_.erase(*peer_id);
    NotifyPeripheralDisconnected(*peer_id);
  }

  callback(Status());
}

void LowEnergyCentralServer::OnScanResult(const bt::gap::Peer& peer) {
  auto fidl_device = fidl_helpers::NewLERemoteDevice(peer);
  if (!fidl_device) {
    bt_log(WARN, "fidl", "%s: ignoring malformed scan result (peer: %s)", __FUNCTION__,
           bt_str(peer.identifier()));
    return;
  }

  if (peer.rssi() != bt::hci::kRSSIInvalid) {
    fidl_device->rssi = Int8::New();
    fidl_device->rssi->value = peer.rssi();
  }

  binding()->events().OnDeviceDiscovered(std::move(*fidl_device));
}

void LowEnergyCentralServer::NotifyScanStateChanged(bool scanning) {
  binding()->events().OnScanStateChanged(scanning);
}

void LowEnergyCentralServer::NotifyPeripheralDisconnected(bt::PeerId peer_id) {
  binding()->events().OnPeripheralDisconnected(peer_id.ToString());
}

}  // namespace bthost
