// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "host_server.h"

#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fit/result.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/assert.h>

#include "helpers.h"
#include "low_energy_central_server.h"
#include "low_energy_peripheral_server.h"
#include "profile_server.h"
#include "src/connectivity/bluetooth/core/bt-host/common/identifier.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/adapter.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bonding_data.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_connection_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_discovery_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/gap.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_address_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_discovery_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt_host.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/types.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/util.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_number_conversions.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace bthost {

namespace fbt = fuchsia::bluetooth;
namespace fsys = fuchsia::bluetooth::sys;

using bt::PeerId;
using bt::gap::LeSecurityModeToString;
using bt::sm::IOCapability;
using fidl_helpers::HostErrorToFidl;
using fidl_helpers::LeSecurityModeFromFidl;
using fidl_helpers::NewFidlError;
using fidl_helpers::PeerIdFromString;
using fidl_helpers::SecurityLevelFromFidl;
using fidl_helpers::StatusToFidl;

std::pair<PeerTracker::Updated, PeerTracker::Removed> PeerTracker::ToFidl(
    const bt::gap::PeerCache* peer_cache) {
  PeerTracker::Updated updated_fidl;
  for (auto& id : updated_) {
    auto* peer = peer_cache->FindById(id);

    // All ids in |updated_| are assumed to be valid as they would otherwise be in |removed_|.
    ZX_ASSERT(peer);

    updated_fidl.push_back(fidl_helpers::PeerToFidl(*peer));
  }

  PeerTracker::Removed removed_fidl;
  for (auto& id : removed_) {
    removed_fidl.push_back(fbt::PeerId{id.value()});
  }

  return std::make_pair(std::move(updated_fidl), std::move(removed_fidl));
}

void PeerTracker::Update(bt::PeerId id) {
  updated_.insert(id);
  removed_.erase(id);
}

void PeerTracker::Remove(bt::PeerId id) {
  updated_.erase(id);
  removed_.insert(id);
}

WatchPeersGetter::WatchPeersGetter(bt::gap::PeerCache* peer_cache) : peer_cache_(peer_cache) {
  ZX_DEBUG_ASSERT(peer_cache_);
}

void WatchPeersGetter::Notify(std::queue<Callback> callbacks, PeerTracker peers) {
  auto [updated, removed] = peers.ToFidl(peer_cache_);
  while (!callbacks.empty()) {
    auto f = std::move(callbacks.front());
    callbacks.pop();
    f(fidl::Clone(updated), fidl::Clone(removed));
  }
}

HostServer::HostServer(zx::channel channel, fxl::WeakPtr<bt::gap::Adapter> adapter,
                       fxl::WeakPtr<GattHost> gatt_host)
    : AdapterServerBase(adapter, this, std::move(channel)),
      pairing_delegate_(nullptr),
      gatt_host_(gatt_host),
      requesting_discovery_(false),
      requesting_background_scan_(false),
      requesting_discoverable_(false),
      io_capability_(IOCapability::kNoInputNoOutput),
      watch_peers_getter_(adapter->peer_cache()),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(gatt_host_);

  auto self = weak_ptr_factory_.GetWeakPtr();
  adapter->peer_cache()->set_peer_updated_callback([self](const auto& peer) {
    if (self) {
      self->OnPeerUpdated(peer);
    }
  });
  adapter->peer_cache()->set_peer_removed_callback([self](const auto& identifier) {
    if (self) {
      self->OnPeerRemoved(identifier);
    }
  });
  adapter->peer_cache()->set_peer_bonded_callback([self](const auto& peer) {
    if (self) {
      self->OnPeerBonded(peer);
    }
  });
  adapter->set_auto_connect_callback([self](auto conn_ref) {
    if (self) {
      self->RegisterLowEnergyConnection(std::move(conn_ref), true);
    }
  });

  // Initialize the HostInfo getter with the initial state.
  NotifyInfoChange();

  // Initialize the peer watcher with all known connectable peers that are in the cache.
  adapter->peer_cache()->ForEach([this](const bt::gap::Peer& peer) { OnPeerUpdated(peer); });
}

HostServer::~HostServer() { Close(); }

void HostServer::WatchState(WatchStateCallback callback) {
  info_getter_.Watch(std::move(callback));
}

void HostServer::SetLocalData(fsys::HostData host_data) {
  if (host_data.has_irk()) {
    bt_log(DEBUG, "bt-host", "assign IRK");
    if (adapter()->le()) {
      adapter()->le()->set_irk(host_data.irk().value);
    }
  }
}

void HostServer::WatchPeers(WatchPeersCallback callback) {
  watch_peers_getter_.Watch(std::move(callback));
}

// TODO(fxbug.dev/35008): Add a unit test for this method.
void HostServer::SetLocalName(::std::string local_name, SetLocalNameCallback callback) {
  ZX_DEBUG_ASSERT(!local_name.empty());
  // Make a copy of |local_name| to move separately into the lambda.
  std::string name_copy(local_name);
  adapter()->SetLocalName(std::move(local_name),
                          [self = weak_ptr_factory_.GetWeakPtr(), local_name = std::move(name_copy),
                           callback = std::move(callback)](auto status) {
                            // Send adapter state update on success and if the connection is still
                            // open.
                            if (status && self) {
                              self->NotifyInfoChange();
                            }
                            callback(StatusToFidl(status));
                          });
}

// TODO(fxbug.dev/35008): Add a unit test for this method.
void HostServer::SetDeviceClass(fbt::DeviceClass device_class, SetDeviceClassCallback callback) {
  // Device Class values must only contain data in the lower 3 bytes.
  if (device_class.value >= 1 << 24) {
    callback(fit::error(fsys::Error::INVALID_ARGUMENTS));
    return;
  }
  bt::DeviceClass dev_class(device_class.value);
  adapter()->SetDeviceClass(
      dev_class, [callback = std::move(callback)](auto status) { callback(StatusToFidl(status)); });
}

void HostServer::StartLEDiscovery(StartDiscoveryCallback callback) {
  if (!adapter()->le()) {
    callback(fit::error(fsys::Error::FAILED));
    return;
  }
  adapter()->le()->StartDiscovery(/*active=*/true, [self = weak_ptr_factory_.GetWeakPtr(),
                                                    callback = std::move(callback)](auto session) {
    // End the new session if this AdapterServer got destroyed in the
    // meantime (e.g. because the client disconnected).
    if (!self) {
      callback(fit::error(fsys::Error::FAILED));
      return;
    }

    if (!self->requesting_discovery_) {
      callback(fit::error(fsys::Error::CANCELED));
      return;
    }

    if (!session) {
      bt_log(DEBUG, "bt-host", "failed to start LE discovery session");
      callback(fit::error(fsys::Error::FAILED));
      self->bredr_discovery_session_ = nullptr;
      self->requesting_discovery_ = false;
      return;
    }

    // Set up a general-discovery filter for connectable devices.
    // NOTE(armansito): This currently has no effect since peer updates
    // are driven by PeerCache events. |session|'s "result callback" is unused.
    session->filter()->set_connectable(true);
    session->filter()->SetGeneralDiscoveryFlags();

    self->le_discovery_session_ = std::move(session);
    self->requesting_discovery_ = false;

    // Send the adapter state update.
    self->NotifyInfoChange();

    callback(fit::ok());
  });
}

void HostServer::StartDiscovery(StartDiscoveryCallback callback) {
  bt_log(DEBUG, "bt-host", "StartDiscovery()");
  ZX_DEBUG_ASSERT(adapter());

  if (le_discovery_session_ || requesting_discovery_) {
    bt_log(DEBUG, "bt-host", "discovery already in progress");
    callback(fit::error(fsys::Error::IN_PROGRESS));
    return;
  }

  requesting_discovery_ = true;
  if (!adapter()->bredr()) {
    StartLEDiscovery(std::move(callback));
    return;
  }
  // TODO(jamuraa): start these in parallel instead of sequence
  adapter()->bredr()->RequestDiscovery(
      [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](
          bt::hci::Status status, auto session) mutable {
        if (!self) {
          callback(fit::error(fsys::Error::FAILED));
          return;
        }

        if (!self->requesting_discovery_) {
          callback(fit::error(fsys::Error::CANCELED));
          return;
        }

        if (!status || !session) {
          bt_log(DEBUG, "bt-host", "failed to start BR/EDR discovery session");

          fit::result<void, fsys::Error> result;
          if (!status) {
            result = StatusToFidl(status);
          } else {
            result = fit::error(fsys::Error::FAILED);
          }
          self->requesting_discovery_ = false;
          callback(std::move(result));
          return;
        }

        self->bredr_discovery_session_ = std::move(session);
        self->StartLEDiscovery(std::move(callback));
      });
}

void HostServer::StopDiscovery() {
  bt_log(DEBUG, "bt-host", "StopDiscovery()");

  bool discovering = le_discovery_session_ || bredr_discovery_session_;
  bredr_discovery_session_ = nullptr;
  le_discovery_session_ = nullptr;

  if (discovering) {
    NotifyInfoChange();
  } else {
    bt_log(DEBUG, "bt-host", "no active discovery session");
  }
}

void HostServer::SetConnectable(bool connectable, SetConnectableCallback callback) {
  bt_log(DEBUG, "bt-host", "SetConnectable(%s)", connectable ? "true" : "false");

  auto classic = adapter()->bredr();
  if (!classic) {
    callback(fit::error(fsys::Error::NOT_SUPPORTED));
    return;
  }
  classic->SetConnectable(connectable, [callback = std::move(callback)](const auto& status) {
    callback(StatusToFidl(status));
  });
}

void HostServer::RestoreBonds(::std::vector<fsys::BondingData> bonds,
                              RestoreBondsCallback callback) {
  bt_log(DEBUG, "bt-host", "RestoreBonds");

  std::vector<fsys::BondingData> errors;

  if (bonds.empty()) {
    // Nothing to do. Reply with an empty list.
    callback(std::move(errors));
    return;
  }

  for (auto& bond : bonds) {
    // This method is only accessible by bt-gap, so we can be confident no clients will use the
    // deprecated `le` sys/LeData or `bredr` sys/BredrData fields.
    ZX_ASSERT_MSG(!bond.has_le(), "Cannot restore bond with deprecated LeData field");
    ZX_ASSERT_MSG(!bond.has_bredr(), "Cannot restore bond with deprecated BredrData field");

    if (!bond.has_identifier() || !bond.has_address() ||
        !(bond.has_le_bond() || bond.has_bredr_bond())) {
      bt_log(ERROR, "bt-host", "BondingData mandatory fields missing!");
      errors.push_back(std::move(bond));
      continue;
    }

    auto address = fidl_helpers::AddressFromFidlBondingData(bond);
    if (!address) {
      errors.push_back(std::move(bond));
      continue;
    }

    bt::gap::BondingData bd;
    bd.identifier = bt::PeerId{bond.identifier().value};
    bd.address = *address;
    if (bond.has_name()) {
      bd.name = {bond.name()};
    }

    if (bond.has_le_bond()) {
      bd.le_pairing_data = fidl_helpers::LePairingDataFromFidl(bond.le_bond());
    }
    if (bond.has_bredr_bond()) {
      bd.bredr_link_key = fidl_helpers::BredrKeyFromFidl(bond.bredr_bond());
      bd.bredr_services = fidl_helpers::BredrServicesFromFidl(bond.bredr_bond());
    }

    // TODO(fxbug.dev/59645): Convert bond.bredr.services to BondingData::bredr_services
    if (!adapter()->AddBondedPeer(bd)) {
      bt_log(ERROR, "bt-host", "failed to load bonding data entry");
      errors.push_back(std::move(bond));
    }
  }

  callback(std::move(errors));
}

void HostServer::OnPeerBonded(const bt::gap::Peer& peer) {
  bt_log(DEBUG, "bt-host", "OnPeerBonded()");
  binding()->events().OnNewBondingData(fidl_helpers::PeerToFidlBondingData(*adapter(), peer));
}

void HostServer::RegisterLowEnergyConnection(bt::gap::LowEnergyConnectionRefPtr conn_ref,
                                             bool auto_connect) {
  ZX_DEBUG_ASSERT(conn_ref);

  bt::PeerId id = conn_ref->peer_identifier();
  auto iter = le_connections_.find(id);
  if (iter != le_connections_.end()) {
    bt_log(TRACE, "bt-host", "peer already connected; reference dropped");
    return;
  }

  bt_log(DEBUG, "bt-host", "LE peer connected (%s): %s ", (auto_connect ? "auto" : "direct"),
         bt_str(id));
  conn_ref->set_closed_callback([self = weak_ptr_factory_.GetWeakPtr(), id] {
    if (self)
      self->le_connections_.erase(id);
  });
  le_connections_[id] = std::move(conn_ref);
}

void HostServer::SetDiscoverable(bool discoverable, SetDiscoverableCallback callback) {
  bt_log(DEBUG, "bt-host", "SetDiscoverable(%s)", discoverable ? "true" : "false");
  // TODO(fxbug.dev/955): advertise LE here
  if (!discoverable) {
    bredr_discoverable_session_ = nullptr;
    NotifyInfoChange();
    callback(fit::ok());
    return;
  }
  if (discoverable && requesting_discoverable_) {
    bt_log(DEBUG, "bt-host", "SetDiscoverable already in progress");
    callback(fit::error(fsys::Error::IN_PROGRESS));
    return;
  }
  requesting_discoverable_ = true;
  if (!adapter()->bredr()) {
    callback(fit::error(fsys::Error::FAILED));
    return;
  }
  adapter()->bredr()->RequestDiscoverable(
      [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](
          bt::hci::Status status, auto session) {
        if (!self) {
          callback(fit::error(fsys::Error::FAILED));
          return;
        }

        if (!self->requesting_discoverable_) {
          callback(fit::error(fsys::Error::CANCELED));
          return;
        }

        if (!status || !session) {
          bt_log(DEBUG, "bt-host", "failed to set discoverable");
          fit::result<void, fsys::Error> result;
          if (!status) {
            result = StatusToFidl(status);
          } else {
            result = fit::error(fsys::Error::FAILED);
          }
          self->requesting_discoverable_ = false;
          callback(std::move(result));
          return;
        }

        self->bredr_discoverable_session_ = std::move(session);
        self->requesting_discoverable_ = false;
        self->NotifyInfoChange();
        callback(fit::ok());
      });
}

void HostServer::EnableBackgroundScan(bool enabled) {
  bt_log(DEBUG, "bt-host", "%s background scan", (enabled ? "enable" : "disable"));
  if (!adapter()->le()) {
    return;
  }

  if (!enabled) {
    requesting_background_scan_ = false;
    le_background_scan_ = nullptr;
    return;
  }

  // If a scan is already starting or is in progress, there is nothing to do to enable the scan.
  if (requesting_background_scan_ || le_background_scan_) {
    return;
  }

  requesting_background_scan_ = true;
  adapter()->le()->StartDiscovery(
      /*active=*/false, [self = weak_ptr_factory_.GetWeakPtr()](auto session) {
        if (!self) {
          return;
        }

        // Background scan may have been disabled while discovery was starting.
        if (!self->requesting_background_scan_) {
          return;
        }

        if (!session) {
          bt_log(DEBUG, "bt-host", "failed to start LE background scan");
          self->le_background_scan_ = nullptr;
          self->requesting_background_scan_ = false;
          return;
        }

        self->le_background_scan_ = std::move(session);
        self->requesting_background_scan_ = false;
      });
}

void HostServer::EnablePrivacy(bool enabled) {
  bt_log(DEBUG, "bt-host", "%s LE privacy", (enabled ? "enable" : "disable"));
  if (adapter()->le()) {
    adapter()->le()->EnablePrivacy(enabled);
  }
}

void HostServer::SetLeSecurityMode(fsys::LeSecurityMode mode) {
  bt::gap::LeSecurityMode gap_mode = LeSecurityModeFromFidl(mode);
  bt_log(INFO, "bt-host", "Setting LE Security Mode: %s", LeSecurityModeToString(gap_mode));
  if (adapter()->le()) {
    adapter()->le()->SetSecurityMode(gap_mode);
  }
}

void HostServer::SetPairingDelegate(fsys::InputCapability input, fsys::OutputCapability output,
                                    ::fidl::InterfaceHandle<fsys::PairingDelegate> delegate) {
  bool cleared = !delegate;
  pairing_delegate_.Bind(std::move(delegate));

  if (cleared) {
    bt_log(DEBUG, "bt-host", "PairingDelegate cleared");
    ResetPairingDelegate();
    return;
  }

  io_capability_ = fidl_helpers::IoCapabilityFromFidl(input, output);
  bt_log(DEBUG, "bt-host", "PairingDelegate assigned (I/O capability: %s)",
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());

  auto self = weak_ptr_factory_.GetWeakPtr();
  adapter()->SetPairingDelegate(self);
  pairing_delegate_.set_error_handler([self](zx_status_t status) {
    bt_log(DEBUG, "bt-host", "PairingDelegate disconnected");
    if (self) {
      self->ResetPairingDelegate();
    }
  });
}

// Attempt to connect to peer identified by |peer_id|. The peer must be
// in our peer cache. We will attempt to connect technologies (LowEnergy,
// Classic or Dual-Mode) as the peer claims to support when discovered
void HostServer::Connect(fbt::PeerId peer_id, ConnectCallback callback) {
  bt::PeerId id{peer_id.value};
  auto peer = adapter()->peer_cache()->FindById(id);
  if (!peer) {
    // We don't support connecting to peers that are not in our cache
    callback(fit::error(fsys::Error::PEER_NOT_FOUND));
    return;
  }

  // TODO(fxbug.dev/1242): Dual-mode currently not supported; if the peer supports
  // BR/EDR we prefer BR/EDR. If a dual-mode peer, we should attempt to connect
  // both protocols.
  if (peer->bredr()) {
    ConnectBrEdr(id, std::move(callback));
    return;
  }

  ConnectLowEnergy(id, std::move(callback));
}

// Attempt to disconnect the peer identified by |peer_id| from all transports.
// If the peer is already not connected, return success. If the peer is
// disconnected succesfully, return success.
void HostServer::Disconnect(fbt::PeerId peer_id, DisconnectCallback callback) {
  bt::PeerId id{peer_id.value};
  bool le_disc = adapter()->le() ? adapter()->le()->Disconnect(id) : true;
  bool bredr_disc = adapter()->bredr()
                        ? adapter()->bredr()->Disconnect(id, bt::gap::DisconnectReason::kApiRequest)
                        : true;
  if (le_disc && bredr_disc) {
    callback(fit::ok());
  } else {
    callback(fit::error(fsys::Error::FAILED));
  }
}

void HostServer::ConnectLowEnergy(PeerId peer_id, ConnectCallback callback) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto on_complete = [self, callback = std::move(callback), peer_id](auto result) {
    if (result.is_error()) {
      bt_log(DEBUG, "bt-host", "failed to connect LE transport to peer (id %s)", bt_str(peer_id));
      callback(fit::error(HostErrorToFidl(result.error())));
      return;
    }

    // We must be connected and to the right peer
    auto connection = result.take_value();
    ZX_ASSERT(connection);
    ZX_ASSERT(peer_id == connection->peer_identifier());

    callback(fit::ok());

    if (self)
      self->RegisterLowEnergyConnection(std::move(connection), false);
  };

  adapter()->le()->Connect(peer_id, std::move(on_complete),
                           bt::gap::Adapter::LowEnergy::ConnectionOptions());
}

// Initiate an outgoing Br/Edr connection, unless already connected
// Br/Edr connections are host-wide, and stored in BrEdrConnectionManager
void HostServer::ConnectBrEdr(PeerId peer_id, ConnectCallback callback) {
  auto on_complete = [callback = std::move(callback), peer_id](auto status, auto connection) {
    if (!status) {
      ZX_ASSERT(!connection);
      bt_log(DEBUG, "bt-host", "failed to connect BR/EDR transport to peer (id %s)",
             bt_str(peer_id));
      callback(fit::error(HostErrorToFidl(status.error())));
      return;
    }

    // We must be connected and to the right peer
    ZX_ASSERT(connection);
    ZX_ASSERT(peer_id == connection->peer_id());

    callback(fit::ok());
  };

  if (!adapter()->bredr()->Connect(peer_id, std::move(on_complete))) {
    callback(fit::error(fsys::Error::FAILED));
  }
}

void HostServer::Forget(fbt::PeerId peer_id, ForgetCallback callback) {
  bt::PeerId id{peer_id.value};
  auto peer = adapter()->peer_cache()->FindById(id);
  if (!peer) {
    bt_log(DEBUG, "bt-host", "peer %s to forget wasn't found", bt_str(id));
    callback(fit::ok());
    return;
  }

  const bool le_disconnected = adapter()->le() ? adapter()->le()->Disconnect(id) : true;
  const bool bredr_disconnected =
      adapter()->bredr()
          ? adapter()->bredr()->Disconnect(id, bt::gap::DisconnectReason::kApiRequest)
          : true;
  const bool peer_removed = adapter()->peer_cache()->RemoveDisconnectedPeer(id);

  if (!le_disconnected || !bredr_disconnected) {
    const auto message =
        fxl::StringPrintf("link(s) failed to close:%s%s", le_disconnected ? "" : " LE",
                          bredr_disconnected ? "" : " BR/EDR");
    callback(fit::error(fsys::Error::FAILED));
  } else {
    ZX_ASSERT(peer_removed);
    callback(fit::ok());
  }
}

void HostServer::Pair(fbt::PeerId id, fsys::PairingOptions options, PairCallback callback) {
  auto peer_id = bt::PeerId(id.value);
  auto peer = adapter()->peer_cache()->FindById(peer_id);
  if (!peer) {
    // We don't support pairing to peers that are not in our cache
    callback(fit::error(fsys::Error::PEER_NOT_FOUND));
    return;
  }
  // If options specifies a transport preference for LE or BR/EDR, we use that. Otherwise, we use
  // whatever transport exists, defaulting to LE for dual-mode connections.
  bool pair_bredr = !peer->le();
  if (options.has_transport() && options.transport() != fsys::TechnologyType::DUAL_MODE) {
    pair_bredr = (options.transport() == fsys::TechnologyType::CLASSIC);
  }
  if (pair_bredr) {
    PairBrEdr(peer_id, std::move(callback));
    return;
  }
  PairLowEnergy(peer_id, std::move(options), std::move(callback));
}

void HostServer::PairLowEnergy(PeerId peer_id, fsys::PairingOptions options,
                               PairCallback callback) {
  std::optional<bt::sm::SecurityLevel> security_level;
  if (options.has_le_security_level()) {
    security_level = SecurityLevelFromFidl(options.le_security_level());
    if (!security_level.has_value()) {
      callback(fit::error(fsys::Error::INVALID_ARGUMENTS));
      return;
    }
  } else {
    security_level = bt::sm::SecurityLevel::kAuthenticated;
  }
  bt::sm::BondableMode bondable_mode = bt::sm::BondableMode::Bondable;
  if (options.has_bondable_mode() && options.bondable_mode() == fsys::BondableMode::NON_BONDABLE) {
    bondable_mode = bt::sm::BondableMode::NonBondable;
  }
  auto on_complete = [peer_id, callback = std::move(callback)](bt::sm::Status status) {
    if (!status) {
      bt_log(WARN, "bt-host", "failed to pair to peer (id %s)", bt_str(peer_id));
      callback(fit::error(HostErrorToFidl(status.error())));
    } else {
      callback(fit::ok());
    }
  };
  ZX_ASSERT(adapter()->le());
  adapter()->le()->Pair(peer_id, *security_level, bondable_mode, std::move(on_complete));
}

void HostServer::PairBrEdr(PeerId peer_id, PairCallback callback) {
  auto on_complete = [peer_id, callback = std::move(callback)](bt::hci::Status status) {
    if (!status) {
      bt_log(WARN, "bt-host", "failed to pair to peer (id %s)", bt_str(peer_id));
      callback(fit::error(HostErrorToFidl(status.error())));
    } else {
      callback(fit::ok());
    }
  };
  // TODO(fxbug.dev/57991): Add security parameter to Pair and use that here instead of hardcoding
  // default.
  bt::gap::BrEdrSecurityRequirements security{.authentication = false, .secure_connections = false};
  ZX_ASSERT(adapter()->bredr());
  adapter()->bredr()->Pair(peer_id, security, std::move(on_complete));
}

void HostServer::RequestLowEnergyCentral(
    fidl::InterfaceRequest<fuchsia::bluetooth::le::Central> request) {
  BindServer<LowEnergyCentralServer>(std::move(request), gatt_host_);
}

void HostServer::RequestLowEnergyPeripheral(
    fidl::InterfaceRequest<fuchsia::bluetooth::le::Peripheral> request) {
  BindServer<LowEnergyPeripheralServer>(std::move(request));
}

void HostServer::RequestGattServer(
    fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Server> request) {
  // GATT FIDL requests are handled by GattHost.
  gatt_host_->BindGattServer(std::move(request));
}

void HostServer::RequestProfile(
    fidl::InterfaceRequest<fuchsia::bluetooth::bredr::Profile> request) {
  BindServer<ProfileServer>(std::move(request));
}

void HostServer::Close() {
  bt_log(DEBUG, "bt-host", "closing FIDL handles");

  // Invalidate all weak pointers. This will guarantee that all pending tasks
  // that reference this HostServer will return early if they run in the future.
  weak_ptr_factory_.InvalidateWeakPtrs();

  // Destroy all FIDL bindings.
  servers_.clear();
  gatt_host_->CloseServers();

  // Cancel pending requests.
  requesting_discovery_ = false;
  requesting_discoverable_ = false;
  requesting_background_scan_ = false;

  le_discovery_session_ = nullptr;
  le_background_scan_ = nullptr;
  bredr_discovery_session_ = nullptr;
  bredr_discoverable_session_ = nullptr;

  // Drop all connections that are attached to this HostServer.
  le_connections_.clear();

  if (adapter()->le()) {
    // Stop background scan if enabled.
    adapter()->le()->EnablePrivacy(false);
    adapter()->le()->set_irk(std::nullopt);
  }

  // Disallow future pairing.
  pairing_delegate_ = nullptr;
  ResetPairingDelegate();

  // Send adapter state change.
  if (binding()->is_bound()) {
    NotifyInfoChange();
  }
}

bt::sm::IOCapability HostServer::io_capability() const {
  bt_log(DEBUG, "bt-host", "I/O capability: %s",
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());
  return io_capability_;
}

void HostServer::CompletePairing(PeerId id, bt::sm::Status status) {
  bt_log(DEBUG, "bt-host", "pairing complete for peer: %s, status: %s", bt_str(id), bt_str(status));
  ZX_DEBUG_ASSERT(pairing_delegate_);
  pairing_delegate_->OnPairingComplete(fbt::PeerId{id.value()}, status.is_success());
}

void HostServer::ConfirmPairing(PeerId id, ConfirmCallback confirm) {
  bt_log(DEBUG, "bt-host", "pairing confirmation request for peer: %s", bt_str(id));
  DisplayPairingRequest(id, std::nullopt, fsys::PairingMethod::CONSENT, std::move(confirm));
}

void HostServer::DisplayPasskey(PeerId id, uint32_t passkey, DisplayMethod method,
                                ConfirmCallback confirm) {
  auto fidl_method = fsys::PairingMethod::PASSKEY_DISPLAY;
  if (method == DisplayMethod::kComparison) {
    bt_log(DEBUG, "bt-host", "compare passkey %06u on peer: %s", passkey, bt_str(id));
    fidl_method = fsys::PairingMethod::PASSKEY_COMPARISON;
  } else {
    bt_log(DEBUG, "bt-host", "enter passkey %06u on peer: %s", passkey, bt_str(id));
  }
  DisplayPairingRequest(id, passkey, fidl_method, std::move(confirm));
}

void HostServer::RequestPasskey(PeerId id, PasskeyResponseCallback respond) {
  bt_log(DEBUG, "bt-host", "passkey request for peer: %s", bt_str(id));
  auto found_peer = adapter()->peer_cache()->FindById(id);
  ZX_ASSERT(found_peer);
  auto peer = fidl_helpers::PeerToFidl(*found_peer);

  ZX_ASSERT(pairing_delegate_);
  pairing_delegate_->OnPairingRequest(
      std::move(peer), fsys::PairingMethod::PASSKEY_ENTRY, 0u,
      [respond = std::move(respond)](const bool accept, uint32_t entered_passkey) mutable {
        if (!respond) {
          bt_log(WARN, "bt-host",
                 "The PairingDelegate invoked the Pairing Request callback more than once, which "
                 "should not happen");
          return;
        }
        bt_log(DEBUG, "bt-host", "got peer response: %s, \"%u\"", accept ? "accept" : "reject",
               entered_passkey);
        if (!accept) {
          respond(-1);
        } else {
          bt_log(TRACE, "bt-host", "got peer passkey: \"%u\"", entered_passkey);
          respond(entered_passkey);
        }
      });
}

void HostServer::DisplayPairingRequest(bt::PeerId id, std::optional<uint32_t> passkey,
                                       fsys::PairingMethod method, ConfirmCallback confirm) {
  auto found_peer = adapter()->peer_cache()->FindById(id);
  ZX_ASSERT(found_peer);
  auto peer = fidl_helpers::PeerToFidl(*found_peer);

  ZX_ASSERT(pairing_delegate_);
  uint32_t displayed_passkey = passkey ? *passkey : 0u;
  pairing_delegate_->OnPairingRequest(
      std::move(peer), method, displayed_passkey,
      [confirm = std::move(confirm)](const bool accept, uint32_t entered_passkey) mutable {
        if (!confirm) {
          bt_log(WARN, "bt-host",
                 "The PairingDelegate invoked the Pairing Request callback more than once, which "
                 "should not happen");
          return;
        }
        bt_log(DEBUG, "bt-host", "got peer response: %s, \"%u\"", accept ? "accept" : "reject",
               entered_passkey);
        confirm(accept);
      });
}

void HostServer::OnConnectionError(Server* server) {
  ZX_DEBUG_ASSERT(server);
  servers_.erase(server);
}

void HostServer::OnPeerUpdated(const bt::gap::Peer& peer) {
  if (!peer.connectable()) {
    return;
  }

  watch_peers_getter_.Transform([id = peer.identifier()](auto tracker) {
    tracker.Update(id);
    return tracker;
  });
}

void HostServer::OnPeerRemoved(bt::PeerId id) {
  // TODO(armansito): Notify only if the peer is connectable for symmetry with
  // OnPeerUpdated?
  watch_peers_getter_.Transform([id](auto tracker) {
    tracker.Remove(id);
    return tracker;
  });
}

void HostServer::ResetPairingDelegate() {
  io_capability_ = IOCapability::kNoInputNoOutput;
  adapter()->SetPairingDelegate(fxl::WeakPtr<HostServer>());
}

void HostServer::NotifyInfoChange() { info_getter_.Set(fidl_helpers::HostInfoToFidl(*adapter())); }

}  // namespace bthost
