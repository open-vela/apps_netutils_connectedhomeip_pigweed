// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_

#include <lib/fit/function.h>
#include <zircon/assert.h>

#include <fbl/macros.h>

#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection_parameters.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/control_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/link_key.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/lib/fxl/synchronization/thread_checker.h"

namespace bt {
namespace hci {

class Transport;

// A Connection represents a logical link connection to a remote device. It
// maintains link-specific configuration parameters (such as the connection
// handle, role, and connection parameters) and state (e.g. kConnected/kDisconnected).
// Controller procedures that are related to managing a logical link are
// performed by a Connection, e.g. disconnecting the link and initiating link
// layer authentication.
//
// Connection instances are intended to be uniquely owned. The owner of an
// instance is also the owner of the underlying link and the lifetime of a
// Connection determines the lifetime of the link.
//
// Connection's public interface related to controller operations is abstract to
// enable the injection of a fake implementation for unit tests that don't need
// a real HCI transport. A production implementation can be obtained via static
// factory methods (see Create* below).
//
// It is possible for non-owning code to reference a Connection by a obtaining a
// WeakPtr.
//
// THREAD SAFETY:
//
// This class is not thread-safe. Instances should only be accessed on their
// creation thread.
class Connection {
 public:
  // This defines the various connection types. These do not exactly correspond
  // to the baseband logical/physical link types but instead provide a
  // high-level abstraction.
  enum class LinkType {
    // Represents a BR/EDR baseband link (ACL-U).
    kACL,

    // BR/EDR isochronous links (SCO-S, eSCO-S).
    kSCO,
    kESCO,

    // A LE logical link (LE-U).
    kLE,
  };

  // Role of the local device in the established connection.
  enum class Role {
    kMaster,
    kSlave,
  };

  enum class State {
    // Default state of a newly created Connection. This is the only connection state that is
    // considered "open".
    kConnected,

    // HCI Disconnect command has been sent, but HCI Disconnection Complete event has not yet been
    // received. This state is skipped when the disconnection is initiated by the peer.
    kWaitingForDisconnectionComplete,

    // HCI Disconnection Complete event has been received.
    kDisconnected
  };

  // Initializes this as a LE connection.
  static std::unique_ptr<Connection> CreateLE(ConnectionHandle handle, Role role,
                                              const DeviceAddress& local_address,
                                              const DeviceAddress& peer_address,
                                              const LEConnectionParameters& params, fxl::WeakPtr<Transport> hci);

  // Initializes this as a BR/EDR ACL connection.
  static std::unique_ptr<Connection> CreateACL(ConnectionHandle handle, Role role,
                                               const DeviceAddress& local_address,
                                               const DeviceAddress& peer_address, fxl::WeakPtr<Transport> hci);

  // The destructor closes this connection.
  virtual ~Connection() = default;

  // Returns a weak pointer to this Connection. This must be implemented by
  // subclasses.
  virtual fxl::WeakPtr<Connection> WeakPtr() = 0;

  // Returns a string representation.
  std::string ToString() const;

  // The type of the connection.
  LinkType ll_type() const { return ll_type_; }

  // Returns the 12-bit connection handle of this connection. This handle is
  // used to identify an individual logical link maintained by the controller.
  ConnectionHandle handle() const { return handle_; }

  // Returns the role of the local device in the established connection.
  Role role() const { return role_; }

  // The active LE connection parameters of this connection. Must only be called
  // on a Connection with the LE link type.
  const LEConnectionParameters& low_energy_parameters() const {
    ZX_DEBUG_ASSERT(ll_type_ == LinkType::kLE);
    return le_params_;
  }

  // Sets the active LE parameters of this connection. Must only be called on a
  // Connection with the LE link type.
  void set_low_energy_parameters(const LEConnectionParameters& params) {
    ZX_DEBUG_ASSERT(ll_type_ == LinkType::kLE);
    le_params_ = params;
  }

  // The local device address used while establishing the connection.
  const DeviceAddress& local_address() const { return local_address_; }

  // The peer address used while establishing the connection.
  const DeviceAddress& peer_address() const { return peer_address_; }

  // Assigns a long term key to this LE-U connection. This will be used for all future encryption
  // procedures.
  void set_le_ltk(const LinkKey& ltk) {
    ZX_ASSERT(ll_type_ == LinkType::kLE);
    ltk_ = ltk;
    ltk_type_ = std::nullopt;
  }

  // Assigns a link key with its corresponding HCI type to this BR/EDR connection. This will be
  // used for bonding procedures and determines the resulting security properties of the link.
  void set_bredr_link_key(const LinkKey& link_key, hci::LinkKeyType type) {
    ZX_ASSERT(ll_type_ != LinkType::kLE);
    ltk_ = link_key;
    ltk_type_ = type;
  }

  // The current long term key of the connection.
  const std::optional<LinkKey>& ltk() const { return ltk_; }

  // For BR/EDR, returns the HCI type value for the long term key, or "link key" per HCI
  // terminology. For LE, returns std::nullopt.
  const std::optional<hci::LinkKeyType>& ltk_type() const { return ltk_type_; }

  // Assigns a callback that will run when the encryption state of the
  // underlying link changes. The |enabled| parameter should be ignored if
  // |status| indicates an error.
  using EncryptionChangeCallback = fit::function<void(Status, bool enabled)>;
  void set_encryption_change_callback(EncryptionChangeCallback callback) {
    encryption_change_callback_ = std::move(callback);
  }

  // Assigns a callback that will be run when the peer disconnects. The callback will be called in
  // the creation thread.
  using PeerDisconnectCallback = fit::function<void(const Connection* connection)>;
  void set_peer_disconnect_callback(PeerDisconnectCallback callback) {
    peer_disconnect_callback_ = std::move(callback);
  }

  // Authenticate (i.e. encrypt) this connection using its current link key.
  // Returns false if the procedure cannot be initiated. The result of the
  // authentication procedure will be reported via the encryption change
  // callback.
  //
  // If called on a LE connection and the link layer procedure fails, the
  // connection will be disconnected. The encryption change callback will be
  // notified of the failure.
  virtual bool StartEncryption() = 0;

  // Send HCI Disconnect and set state to closed. Must not be called on an already disconnected
  // connection.
  virtual void Disconnect(StatusCode reason) = 0;

 protected:
  Connection(ConnectionHandle handle, LinkType ll_type, Role role,
             const DeviceAddress& local_address, const DeviceAddress& peer_address);

  const EncryptionChangeCallback& encryption_change_callback() const {
    return encryption_change_callback_;
  }

  const PeerDisconnectCallback& peer_disconnect_callback() const {
    return peer_disconnect_callback_;
  }

 private:
  LinkType ll_type_;
  ConnectionHandle handle_;
  Role role_;

  // Addresses used while creating the link.
  DeviceAddress local_address_;
  DeviceAddress peer_address_;

  // Connection parameters for a LE link. Not nullptr if the link type is LE.
  LEConnectionParameters le_params_;

  // This connection's current link key.
  std::optional<LinkKey> ltk_;

  // BR/EDR-specific type of the assigned link key.
  std::optional<hci::LinkKeyType> ltk_type_;

  EncryptionChangeCallback encryption_change_callback_;

  PeerDisconnectCallback peer_disconnect_callback_;

  // TODO(BT-715): Add a BREDRParameters struct.

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Connection);
};

using ConnectionPtr = std::unique_ptr<Connection>;

}  // namespace hci
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_CONNECTION_H_
