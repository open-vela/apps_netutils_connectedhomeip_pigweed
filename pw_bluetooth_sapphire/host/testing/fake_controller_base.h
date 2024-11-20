// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TESTING_FAKE_CONTROLLER_BASE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TESTING_FAKE_CONTROLLER_BASE_H_

#include <lib/async/cpp/wait.h>
#include <lib/zx/channel.h>
#include <zircon/device/bt-hci.h>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/packet_view.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/lib/fxl/macros.h"

namespace bt {
namespace testing {

// Abstract base for implementing a fake HCI controller endpoint. This can
// directly send ACL data and event packets on request and forward outgoing ACL
// data packets to subclass implementations.
class FakeControllerBase {
 public:
  FakeControllerBase();
  virtual ~FakeControllerBase();

  // Stops the message loop and thread.
  void Stop();

  // Sends the given packet over this FakeController's command channel endpoint.
  // Retuns the result of the write operation on the command channel.
  zx_status_t SendCommandChannelPacket(const common::ByteBuffer& packet);

  // Sends the given packet over this FakeController's ACL data channel
  // endpoint.
  // Retuns the result of the write operation on the channel.
  zx_status_t SendACLDataChannelPacket(const common::ByteBuffer& packet);

  // Immediately closes the command channel endpoint.
  void CloseCommandChannel();

  // Immediately closes the ACL data channel endpoint.
  void CloseACLDataChannel();

  // Immediately closes the Snoop channel endpoint.
  void CloseSnoopChannel();

  // Starts listening for command/event packets on the given channel.
  // Returns false if already listening on a command channel
  bool StartCmdChannel(zx::channel chan);

  // Starts listening for acl packets on the given channel.
  // Returns false if already listening on a acl channel
  bool StartAclChannel(zx::channel chan);

  // Starts listening for snoop packets on the given channel.
  // Returns false if already listening on a snoop channel
  bool StartSnoopChannel(zx::channel chan);

 protected:
  // Getters for our channel endpoints.
  const zx::channel& command_channel() const { return cmd_channel_; }
  const zx::channel& acl_data_channel() const { return acl_channel_; }
  const zx::channel& snoop_channel() const { return snoop_channel_; }

  // Called when there is an incoming command packet.
  virtual void OnCommandPacketReceived(
      const common::PacketView<hci::CommandHeader>& command_packet) = 0;

  // Called when there is an outgoing ACL data packet.
  virtual void OnACLDataPacketReceived(
      const common::ByteBuffer& acl_data_packet) = 0;

 private:
  // Read and handle packets received over the channels.
  void HandleCommandPacket(async_dispatcher_t* dispatcher,
                           async::WaitBase* wait,
                           zx_status_t wait_status,
                           const zx_packet_signal_t* signal);
  void HandleACLPacket(async_dispatcher_t* dispatcher,
                       async::WaitBase* wait,
                       zx_status_t wait_status,
                       const zx_packet_signal_t* signal);

  // Sends the given packet over this FakeController's Snoop channel
  // endpoint.
  // Retuns the result of the write operation on the channel.
  void SendSnoopChannelPacket(const common::ByteBuffer& packet,
                              bt_hci_snoop_type_t packet_type,
                              bool is_received);

  zx::channel cmd_channel_;
  zx::channel acl_channel_;
  zx::channel snoop_channel_;

  async::WaitMethod<FakeControllerBase,
                    &FakeControllerBase::HandleCommandPacket>
                    cmd_channel_wait_{this};
  async::WaitMethod<FakeControllerBase,
                    &FakeControllerBase::HandleACLPacket>
                    acl_channel_wait_{this};

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeControllerBase);
};

}  // namespace testing
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_TESTING_FAKE_CONTROLLER_BASE_H_
