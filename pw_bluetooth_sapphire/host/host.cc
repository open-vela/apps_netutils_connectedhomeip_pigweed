// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "host.h"

#include "fidl/host_server.h"
#include "gatt_host.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/device_wrapper.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap.h"

using namespace bt;

namespace bthost {

Host::Host(const bt_hci_protocol_t& hci_proto, std::optional<bt_vendor_protocol_t> vendor_proto)
    : hci_proto_(hci_proto), vendor_proto_(vendor_proto) {}

Host::~Host() {}

bool Host::Initialize(inspect::Node& root_node, InitCallback callback) {
  auto dev = std::make_unique<hci::DdkDeviceWrapper>(hci_proto_, vendor_proto_);

  auto hci_result = hci::Transport::Create(std::move(dev));
  if (hci_result.is_error()) {
    bt_log(ERROR, "bt-host", "failed to initialize HCI transport");
    return false;
  }
  hci_ = hci_result.take_value();

  gatt_host_ = std::make_unique<GattHost>();

  gap_ = std::make_unique<gap::Adapter>(hci_->WeakPtr(), gatt_host_->profile(), std::nullopt);
  if (!gap_)
    return false;

  gap_->AttachInspect(root_node);
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  // Called when the GAP layer is ready. We initialize the GATT profile after
  // initial setup in GAP. The data domain will be initialized by GAP because it
  // both sets up the HCI ACL data channel that L2CAP relies on and registers
  // L2CAP services.
  auto gap_init_callback = [callback = std::move(callback)](bool success) {
    bt_log(DEBUG, "bt-host", "GAP init complete (%s)", (success ? "success" : "failure"));

    callback(success);
  };

  bt_log(DEBUG, "bt-host", "initializing GAP");
  // TODO(fxbug.dev/52588): remove bt-host device and shut down the stack when HCI transport has
  // closed.
  return gap_->Initialize(std::move(gap_init_callback),
                          [] { bt_log(DEBUG, "bt-host", "bt-host: HCI transport has closed"); });
}

void Host::ShutDown() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  bt_log(DEBUG, "bt-host", "shutting down");

  if (!gap_) {
    bt_log(DEBUG, "bt-host", "already shut down");
    return;
  }

  // Closes all FIDL channels owned by |host_server_|.
  host_server_ = nullptr;

  // Make sure that |gap_| gets shut down and destroyed on its creation thread
  // as it is not thread-safe.
  gap_->ShutDown();
  gap_ = nullptr;

  // This shuts down the GATT profile and all of its clients.
  gatt_host_ = nullptr;

  // Shuts down HCI command channel and ACL data channel.
  hci_ = nullptr;
}

void Host::BindHostInterface(zx::channel channel) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  if (host_server_) {
    bt_log(WARN, "bt-host", "Host interface channel already open!");
    return;
  }

  ZX_DEBUG_ASSERT(gap_);
  ZX_DEBUG_ASSERT(gatt_host_);

  host_server_ =
      std::make_unique<HostServer>(std::move(channel), gap_->AsWeakPtr(), gatt_host_->AsWeakPtr());
  host_server_->set_error_handler([this](zx_status_t status) {
    ZX_DEBUG_ASSERT(host_server_);
    bt_log(DEBUG, "bt-host", "Host interface disconnected");
    host_server_ = nullptr;
  });
}

}  // namespace bthost
