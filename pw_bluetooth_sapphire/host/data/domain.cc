// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "domain.h"

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/task_domain.h"
#include "src/connectivity/bluetooth/core/bt-host/data/socket_factory.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel_manager.h"

namespace bt {
namespace data {

class Impl final : public Domain {
 public:
  Impl(fxl::RefPtr<hci::Transport> hci, inspect::Node node)
      : Domain(), dispatcher_(async_get_default_dispatcher()), node_(std::move(node)), hci_(hci) {
    ZX_ASSERT(hci_);
    ZX_ASSERT(hci_->acl_data_channel());
    const auto& acl_buffer_info = hci_->acl_data_channel()->GetBufferInfo();
    const auto& le_buffer_info = hci_->acl_data_channel()->GetLEBufferInfo();

    // LE Buffer Info is always available from ACLDataChannel.
    ZX_ASSERT(acl_buffer_info.IsAvailable());
    auto send_packets =
        fit::bind_member(hci_->acl_data_channel(), &hci::ACLDataChannel::SendPackets);
    auto drop_queued_acl =
        fit::bind_member(hci_->acl_data_channel(), &hci::ACLDataChannel::DropQueuedPackets);

    channel_manager_ = std::make_unique<l2cap::ChannelManager>(
        acl_buffer_info.max_data_length(), le_buffer_info.max_data_length(),
        std::move(send_packets), std::move(drop_queued_acl), dispatcher_);
    hci_->acl_data_channel()->SetDataRxHandler(channel_manager_->MakeInboundDataHandler(),
                                               dispatcher_);

    l2cap_socket_factory_ = std::make_unique<internal::SocketFactory<l2cap::Channel>>();

    bt_log(DEBUG, "data-domain", "initialized");
  }

  ~Impl() {
    bt_log(DEBUG, "data-domain", "shutting down");
    l2cap_socket_factory_ = nullptr;

    ZX_ASSERT(hci_->acl_data_channel());
    hci_->acl_data_channel()->SetDataRxHandler(nullptr, nullptr);

    channel_manager_ = nullptr;
  }

  void AddACLConnection(hci::ConnectionHandle handle, hci::Connection::Role role,
                        l2cap::LinkErrorCallback link_error_callback,
                        l2cap::SecurityUpgradeCallback security_callback) override {
    channel_manager_->RegisterACL(handle, role, std::move(link_error_callback),
                                  std::move(security_callback));
  }

  LEFixedChannels AddLEConnection(hci::ConnectionHandle handle, hci::Connection::Role role,
                                  l2cap::LinkErrorCallback link_error_callback,
                                  l2cap::LEConnectionParameterUpdateCallback conn_param_callback,
                                  l2cap::SecurityUpgradeCallback security_callback) override {
    channel_manager_->RegisterLE(handle, role, std::move(conn_param_callback),
                                 std::move(link_error_callback), std::move(security_callback));

    auto att = channel_manager_->OpenFixedChannel(handle, l2cap::kATTChannelId);
    auto smp = channel_manager_->OpenFixedChannel(handle, l2cap::kLESMPChannelId);
    ZX_ASSERT(att);
    ZX_ASSERT(smp);
    return LEFixedChannels{.att = std::move(att), .smp = std::move(smp)};
  }

  void RemoveConnection(hci::ConnectionHandle handle) override {
    channel_manager_->Unregister(handle);
  }

  void AssignLinkSecurityProperties(hci::ConnectionHandle handle,
                                    sm::SecurityProperties security) override {
    channel_manager_->AssignLinkSecurityProperties(handle, security);
  }

  void RequestConnectionParameterUpdate(hci::ConnectionHandle handle,
                                        hci::LEPreferredConnectionParameters params,
                                        l2cap::ConnectionParameterUpdateRequestCallback request_cb,
                                        async_dispatcher_t* dispatcher) override {
    channel_manager_->RequestConnectionParameterUpdate(handle, params, std::move(request_cb),
                                                       dispatcher);
  }

  void OpenL2capChannel(hci::ConnectionHandle handle, l2cap::PSM psm,
                        l2cap::ChannelParameters params, l2cap::ChannelCallback cb) override {
    channel_manager_->OpenChannel(handle, psm, params, std::move(cb));
  }

  void OpenL2capChannel(hci::ConnectionHandle handle, l2cap::PSM psm,
                        l2cap::ChannelParameters params, SocketCallback socket_callback) override {
    OpenL2capChannel(handle, psm, params,
                     [this, handle, cb = std::move(socket_callback)](auto channel) {
                       // MakeSocketForChannel makes invalid sockets for null channels (i.e.
                       // that have failed to open).
                       zx::socket s = l2cap_socket_factory_->MakeSocketForChannel(channel);
                       auto chan_info = channel ? std::optional(channel->info()) : std::nullopt;
                       l2cap::ChannelSocket chan_sock(std::move(s), chan_info);
                       cb(std::move(chan_sock), handle);
                     });
  }

  void RegisterService(l2cap::PSM psm, l2cap::ChannelParameters params,
                       l2cap::ChannelCallback callback, async_dispatcher_t* dispatcher) override {
    const bool result =
        channel_manager_->RegisterService(psm, params, std::move(callback), dispatcher);
    ZX_DEBUG_ASSERT(result);
  }

  void RegisterService(l2cap::PSM psm, l2cap::ChannelParameters params,
                       SocketCallback socket_callback, async_dispatcher_t* cb_dispatcher) override {
    RegisterService(
        psm, params,
        [this, cb = std::move(socket_callback), cb_dispatcher](auto channel) mutable {
          zx::socket s = l2cap_socket_factory_->MakeSocketForChannel(channel);
          auto chan_info = channel ? std::optional(channel->info()) : std::nullopt;
          l2cap::ChannelSocket chan_sock(std::move(s), chan_info);
          // Called every time the service is connected, cb must be shared.
          async::PostTask(cb_dispatcher, [sock = std::move(chan_sock), cb = cb.share(),
                                          handle = channel->link_handle()]() mutable {
            cb(std::move(sock), handle);
          });
        },
        dispatcher_);
  }

  void UnregisterService(l2cap::PSM psm) override { channel_manager_->UnregisterService(psm); }

 private:
  async_dispatcher_t* dispatcher_;

  // Inspect hierarchy node representing the data domain.
  inspect::Node node_;

  // Handle to the underlying HCI transport.
  fxl::RefPtr<hci::Transport> hci_;

  std::unique_ptr<l2cap::ChannelManager> channel_manager_;

  // Creates sockets that bridge internal L2CAP channels to profile processes.
  std::unique_ptr<internal::SocketFactory<l2cap::Channel>> l2cap_socket_factory_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Impl);
};

// static
fbl::RefPtr<Domain> Domain::Create(fxl::RefPtr<hci::Transport> hci, inspect::Node node) {
  ZX_DEBUG_ASSERT(hci);
  return AdoptRef(new Impl(hci, std::move(node)));
}

}  // namespace data
}  // namespace bt
