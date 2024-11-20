// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_ACL_DATA_CHANNEL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_ACL_DATA_CHANNEL_H_

#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>
#include <lib/fit/thread_checker.h>
#include <lib/zx/channel.h>
#include <zircon/compiler.h>

#include <list>
#include <queue>
#include <unordered_map>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/acl_data_packet.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/command_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/control_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci_constants.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci_defs.h"

namespace bt::hci {

// Our ACL implementation allows specifying a Unique ChannelId for purposes of grouping packets so
// they can be dropped together when necessary. In practice, this channel id will always be equal
// to a given L2CAP ChannelId, as specified in the l2cap library
using UniqueChannelId = uint16_t;

class Transport;

// Represents the controller data buffer settings for the BR/EDR or LE
// transports.
class DataBufferInfo {
 public:
  // Initialize fields to non-zero values.
  DataBufferInfo(size_t max_data_length, size_t max_num_packets);

  // The default constructor sets all fields to zero. This can be used to
  // represent a data buffer that does not exist (e.g. the controller has a
  // single shared buffer and no dedicated LE buffer.
  DataBufferInfo();

  // The maximum length (in octets) of the data portion of each HCI ACL data
  // packet that the controller is able to accept.
  size_t max_data_length() const { return max_data_length_; }

  // Returns the total number of HCI ACL data packets that can be stored in the
  // data buffer represented by this object.
  size_t max_num_packets() const { return max_num_packets_; }

  // Returns true if both fields are set to zero.
  bool IsAvailable() const { return max_data_length_ && max_num_packets_; }

  // Comparison operators.
  bool operator==(const DataBufferInfo& other) const;
  bool operator!=(const DataBufferInfo& other) const { return !(*this == other); }

 private:
  size_t max_data_length_;
  size_t max_num_packets_;
};

// Represents the Bluetooth ACL Data channel and manages the Host<->Controller
// ACL data flow control.
//
// This currently only supports the Packet-based Data Flow Control as defined in
// Core Spec v5.0, Vol 2, Part E, Section 4.1.1.
class AclDataChannel {
 public:
  enum class PacketPriority { kHigh, kLow };

  using AclPacketPredicate =
      fit::function<bool(const ACLDataPacketPtr& packet, UniqueChannelId channel_id)>;

  static std::unique_ptr<AclDataChannel> Create(Transport* transport, zx::channel hci_acl_channel);

  virtual ~AclDataChannel() = default;

  // Starts listening on the HCI ACL data channel and starts handling data flow
  // control. |bredr_buffer_info| represents the controller's data buffering
  // capacity for the BR/EDR transport and the |le_buffer_info| represents Low
  // Energy buffers. At least one of these (BR/EDR vs LE) must contain non-zero
  // values per Core Spec v5.0 Vol 2, Part E, Sec 4.1.1:
  //
  //   - A LE only controller will have LE buffers only.
  //   - A BR/EDR-only controller will have BR/EDR buffers only.
  //   - A dual-mode controller will have BR/EDR buffers and MAY have LE buffers
  //     if the BR/EDR buffer is not shared between the transports.
  //
  // As this class is intended to support flow-control for both, this function
  // should be called based on what is reported by the controller.
  virtual void Initialize(const DataBufferInfo& bredr_buffer_info,
                          const DataBufferInfo& le_buffer_info) = 0;

  // Unregisters event handlers and cleans up.
  // NOTE: Initialize() and ShutDown() MUST be called on the same thread. These
  // methods are not thread-safe.
  virtual void ShutDown() = 0;

  // Assigns a handler callback for received ACL data packets. |rx_callback| will shall take
  // ownership of each packet received from the controller.
  virtual void SetDataRxHandler(ACLPacketHandler rx_callback) = 0;

  // Queues the given ACL data packet to be sent to the controller. Returns
  // false if the packet cannot be queued up, e.g. if the size of |data_packet|
  // exceeds the MTU for the link type set in RegisterLink().
  //
  // |data_packet| is passed by value, meaning that ACLDataChannel will take
  // ownership of it. |data_packet| must represent a valid ACL data packet.
  //
  // |channel_id| must match the l2cap channel that the packet is being sent to. It is needed to
  // determine what channel l2cap packet fragments are being sent to when revoking queued packets
  // for specific channels that have closed. If the packet does not contain a fragment of an l2cap
  // packet, |channel_id| should be set to |l2cap::kInvalidChannelId|.
  //
  // |priority| indicates the order this packet should be dispatched off of the queue relative to
  // packets of other priorities. Note that high priority packets may still wait behind low priority
  // packets that have already been sent to the controller.
  virtual bool SendPacket(ACLDataPacketPtr data_packet, UniqueChannelId channel_id,
                          PacketPriority priority) = 0;

  // Queues the given list of ACL data packets to be sent to the controller. The
  // behavior is identical to that of SendPacket() with the guarantee that all
  // packets that are in |packets| are queued atomically. If any packet's handle is not registered
  // in the allowlist, then none will be queued.
  //
  // Takes ownership of the contents of |packets|. Returns false if |packets|
  // contains an element that exceeds the MTU for its link type or |packets| is empty.
  //
  // |channel_id| must match the l2cap channel that all packets is being sent to. It is needed to
  // determine what channel l2cap packet fragments are being sent to when revoking queued packets
  // for channels that have closed. If the packets do not contain a fragment of an l2cap
  // packet, |channel_id| should be set to |l2cap::kInvalidChannelId|.
  //
  // |priority| indicates the order this packet should be dispatched off of the queue relative to
  // packets of other priorities. Note that high priority packets may still wait behind low priority
  // packets that have already been sent to the controller.
  virtual bool SendPackets(LinkedList<ACLDataPacket> packets, UniqueChannelId channel_id,
                           PacketPriority priority) = 0;

  // Allowlist packets destined for the link identified by |handle| (of link type |ll_type|) for
  // submission to the controller.
  //
  // Failure to register a link before sending packets will result in the packets
  // being dropped immediately. A handle must not be registered again until after UnregisterLink has
  // been called on that handle.
  virtual void RegisterLink(hci::ConnectionHandle handle, Connection::LinkType ll_type) = 0;

  // Cleans up all outgoing data buffering state related to the logical link
  // with the given |handle|. This must be called upon disconnection of a link
  // to ensure that stale outbound packets are filtered out of the send queue.
  // All future packets sent to this link will be dropped.
  //
  // |RegisterLink| must be called before |UnregisterLink| for the same handle.
  //
  // |UnregisterLink| does not clear the controller packet count, so |ClearControllerPacketCount|
  // must be called after |UnregisterLink| and the HCI Disconnection Complete event has been
  // received.
  virtual void UnregisterLink(hci::ConnectionHandle handle) = 0;

  // Removes all queued data packets for which |predicate| returns true.
  virtual void DropQueuedPackets(AclPacketPredicate predicate) = 0;

  // Resets controller packet count for |handle| so that controller buffer credits can be reused.
  // This must be called on the HCI_Disconnection_Complete event to notify ACLDataChannel that
  // packets in the controller's buffer for |handle| have been flushed. See Core Spec v5.1, Vol 2,
  // Part E, Section 4.3. This must be called after |UnregisterLink|.
  virtual void ClearControllerPacketCount(hci::ConnectionHandle handle) = 0;

  // Returns the BR/EDR buffer information that the channel was initialized
  // with.
  virtual const DataBufferInfo& GetBufferInfo() const = 0;

  // Returns the LE buffer information that the channel was initialized with.
  // This defaults to the BR/EDR buffers if the controller does not have a
  // dedicated LE buffer.
  virtual const DataBufferInfo& GetLeBufferInfo() const = 0;

  // Reads bytes from the channel and try to parse them as ACLDataPacket.
  // ZX_ERR_IO means error happens while reading from the channel.
  // ZX_ERR_INVALID_ARGS means the packet is malformed.
  // Otherwise, ZX_OK is returned.
  static zx_status_t ReadAclDataPacketFromChannel(const zx::channel& channel,
                                                  const ACLDataPacketPtr& packet);

  // Attempts to set the ACL |priority| of the connection indicated by |handle|. |callback| will be
  // called with the result of the request.
  virtual void RequestAclPriority(hci::AclPriority priority, hci::ConnectionHandle handle,
                                  fit::callback<void(fit::result<>)> callback) = 0;

  // Sets an automatic flush timeout with duration |flush_timeout| for the connection indicated by
  // |handle|. |callback| will be called with the result of the operation.
  // |handle| must correspond to a BR/EDR connection.
  // |flush_timeout| must be in the range [1ms - kMaxAutomaticFlushTimeoutDuration]. A flush timeout
  // of zx::duration::infinite() indicates an infinite flush timeout (no automatic flush), the
  // default. If an invalid value of |flush_timeout| is specified, an error will be returned to
  // |callback|.
  virtual void SetBrEdrAutomaticFlushTimeout(
      zx::duration flush_timeout, hci::ConnectionHandle handle,
      fit::callback<void(fit::result<void, StatusCode>)> callback) = 0;
};

}  // namespace bt::hci

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_ACL_DATA_CHANNEL_H_
