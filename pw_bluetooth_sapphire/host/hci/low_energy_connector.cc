// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "low_energy_connector.h"

#include <endian.h>
#include <zircon/assert.h>

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/defaults.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/local_address_delegate.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/util.h"
#include "src/lib/fxl/time/time_delta.h"

namespace bt {
namespace hci {

LowEnergyConnector::PendingRequest::PendingRequest(const DeviceAddress& peer_address,
                                                   StatusCallback status_callback)
    : peer_address(peer_address), status_callback(std::move(status_callback)) {}

LowEnergyConnector::LowEnergyConnector(fxl::RefPtr<Transport> hci,
                                       LocalAddressDelegate* local_addr_delegate,
                                       async_dispatcher_t* dispatcher,
                                       IncomingConnectionDelegate delegate)
    : dispatcher_(dispatcher),
      hci_(hci),
      local_addr_delegate_(local_addr_delegate),
      delegate_(std::move(delegate)),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(dispatcher_);
  ZX_DEBUG_ASSERT(hci_);
  ZX_DEBUG_ASSERT(local_addr_delegate_);
  ZX_DEBUG_ASSERT(delegate_);

  auto self = weak_ptr_factory_.GetWeakPtr();
  event_handler_id_ = hci_->command_channel()->AddLEMetaEventHandler(
      kLEConnectionCompleteSubeventCode,
      [self](const auto& event) {
        if (self) {
          return self->OnConnectionCompleteEvent(event);
        }
        return CommandChannel::EventCallbackResult::kRemove;
      },
      dispatcher_);
}

LowEnergyConnector::~LowEnergyConnector() {
  hci_->command_channel()->RemoveEventHandler(event_handler_id_);
  if (request_pending())
    Cancel();
}

bool LowEnergyConnector::CreateConnection(bool use_whitelist, const DeviceAddress& peer_address,
                                          uint16_t scan_interval, uint16_t scan_window,
                                          const LEPreferredConnectionParameters& initial_parameters,
                                          StatusCallback status_callback, zx::duration timeout) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(status_callback);
  ZX_DEBUG_ASSERT(timeout.get() > 0);

  if (request_pending())
    return false;

  ZX_DEBUG_ASSERT(!request_timeout_task_.is_pending());
  pending_request_ = PendingRequest(peer_address, std::move(status_callback));

  local_addr_delegate_->EnsureLocalAddress(
      [this, use_whitelist, peer_address, scan_interval, scan_window, initial_parameters,
       callback = std::move(status_callback), timeout](const auto& address) mutable {
        CreateConnectionInternal(address, use_whitelist, peer_address, scan_interval, scan_window,
                                 initial_parameters, std::move(callback), timeout);
      });

  return true;
}

void LowEnergyConnector::CreateConnectionInternal(
    const DeviceAddress& local_address, bool use_whitelist, const DeviceAddress& peer_address,
    uint16_t scan_interval, uint16_t scan_window,
    const LEPreferredConnectionParameters& initial_parameters, StatusCallback status_callback,
    zx::duration timeout) {
  // Check if the connection request was canceled via Cancel().
  if (!pending_request_ || pending_request_->canceled) {
    bt_log(TRACE, "hci-le", "connection request was canceled while obtaining local address");
    pending_request_.reset();
    return;
  }

  ZX_DEBUG_ASSERT(!pending_request_->initiating);

  pending_request_->initiating = true;
  pending_request_->local_address = local_address;

  auto request = CommandPacket::New(kLECreateConnection, sizeof(LECreateConnectionCommandParams));
  auto params = request->mutable_payload<LECreateConnectionCommandParams>();
  params->scan_interval = htole16(scan_interval);
  params->scan_window = htole16(scan_window);
  params->initiator_filter_policy =
      use_whitelist ? GenericEnableParam::kEnable : GenericEnableParam::kDisable;

  // TODO(armansito): Use the resolved address types for <5.0 LE Privacy.
  params->peer_address_type =
      peer_address.IsPublic() ? LEAddressType::kPublic : LEAddressType::kRandom;
  params->peer_address = peer_address.value();

  params->own_address_type =
      local_address.IsPublic() ? LEOwnAddressType::kPublic : LEOwnAddressType::kRandom;

  params->conn_interval_min = htole16(initial_parameters.min_interval());
  params->conn_interval_max = htole16(initial_parameters.max_interval());
  params->conn_latency = htole16(initial_parameters.max_latency());
  params->supervision_timeout = htole16(initial_parameters.supervision_timeout());
  params->minimum_ce_length = 0x0000;
  params->maximum_ce_length = 0x0000;

  // HCI Command Status Event will be sent as our completion callback.
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto complete_cb = [self, timeout](auto id, const EventPacket& event) {
    ZX_DEBUG_ASSERT(event.event_code() == kCommandStatusEventCode);

    if (!self)
      return;

    Status status = event.ToStatus();
    if (!status) {
      self->OnCreateConnectionComplete(Status(status), nullptr);
      return;
    }

    // The request was started but has not completed; initiate the command
    // timeout period. NOTE: The request will complete when the controller
    // asynchronously notifies us of with a LE Connection Complete event.
    self->request_timeout_task_.Cancel();
    self->request_timeout_task_.PostDelayed(async_get_default_dispatcher(), timeout);
  };

  hci_->command_channel()->SendCommand(std::move(request), dispatcher_, complete_cb,
                                       kCommandStatusEventCode);
}

void LowEnergyConnector::Cancel() { CancelInternal(false); }

void LowEnergyConnector::CancelInternal(bool timed_out) {
  ZX_DEBUG_ASSERT(request_pending());

  if (pending_request_->canceled) {
    bt_log(WARN, "hci-le", "connection attempt already canceled!");
    return;
  }

  // At this point we do not know whether the pending connection request has
  // completed or not (it may have completed in the controller but that does not
  // mean that we have processed the corresponding LE Connection Complete
  // event). Below we mark the request as canceled and tell the controller to
  // cancel its pending connection attempt.
  pending_request_->canceled = true;
  pending_request_->timed_out = timed_out;

  request_timeout_task_.Cancel();

  // Tell the controller to cancel the connection initiation attempt if a
  // request is outstanding. Otherwise there is no need to talk to the
  // controller.
  if (pending_request_->initiating) {
    bt_log(TRACE, "hci-le", "telling controller to cancel LE connection attempt");
    auto complete_cb = [](auto id, const EventPacket& event) {
      hci_is_error(event, WARN, "hci-le", "failed to cancel connection request");
    };
    auto cancel = CommandPacket::New(kLECreateConnectionCancel);
    hci_->command_channel()->SendCommand(std::move(cancel), dispatcher_, complete_cb);
    return;
  }

  bt_log(TRACE, "hci-le", "connection initiation aborted");
  OnCreateConnectionComplete(Status(HostError::kCanceled), nullptr);
}

CommandChannel::EventCallbackResult LowEnergyConnector::OnConnectionCompleteEvent(
    const EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == kLEMetaEventCode);
  ZX_DEBUG_ASSERT(event.params<LEMetaEventParams>().subevent_code ==
                  kLEConnectionCompleteSubeventCode);

  auto params = event.le_event_params<LEConnectionCompleteSubeventParams>();
  ZX_ASSERT(params);

  // First check if this event is related to the currently pending request.
  const bool matches_pending_request =
      pending_request_ && (pending_request_->peer_address.value() == params->peer_address);

  Status status(params->status);
  if (!status) {
    if (matches_pending_request) {
      // The "Unknown Connect Identifier" error code is returned if this event
      // was sent due to a successful cancelation via the
      // HCI_LE_Create_Connection_Cancel command (sent by Cancel()).
      if (pending_request_->timed_out) {
        status = Status(HostError::kTimedOut);
      } else if (params->status == StatusCode::kUnknownConnectionId) {
        status = Status(HostError::kCanceled);
      }
      OnCreateConnectionComplete(status, nullptr);
    } else {
      bt_log(WARN, "hci-le", "unexpected connection complete event with error received: %s",
             status.ToString().c_str());
    }
    return CommandChannel::EventCallbackResult::kContinue;
  }

  ConnectionHandle handle = le16toh(params->connection_handle);
  Connection::Role role = (params->role == ConnectionRole::kMaster) ? Connection::Role::kMaster
                                                                    : Connection::Role::kSlave;
  DeviceAddress peer_address(AddressTypeFromHCI(params->peer_address_type), params->peer_address);
  LEConnectionParameters connection_params(le16toh(params->conn_interval),
                                           le16toh(params->conn_latency),
                                           le16toh(params->supervision_timeout));

  // If the connection did not match a pending request then we pass the
  // information down to the incoming connection delegate.
  if (!matches_pending_request) {
    delegate_(handle, role, peer_address, connection_params);
    return CommandChannel::EventCallbackResult::kContinue;
  }

  // A new link layer connection was created. Create an object to track this
  // connection. Destroying this object will disconnect the link.
  auto connection = Connection::CreateLE(handle, role, pending_request_->local_address,
                                         peer_address, connection_params, hci_);

  if (pending_request_->timed_out) {
    status = Status(HostError::kTimedOut);
  } else if (pending_request_->canceled) {
    status = Status(HostError::kCanceled);
  } else {
    status = Status();
  }

  // If we were requested to cancel the connection after the logical link
  // is created we disconnect it.
  if (!status) {
    connection = nullptr;
  }
  OnCreateConnectionComplete(status, std::move(connection));
  return CommandChannel::EventCallbackResult::kContinue;
}

void LowEnergyConnector::OnCreateConnectionComplete(Status status, ConnectionPtr link) {
  ZX_DEBUG_ASSERT(pending_request_);

  bt_log(TRACE, "hci-le", "connection complete - status: %s", status.ToString().c_str());

  request_timeout_task_.Cancel();

  auto status_cb = std::move(pending_request_->status_callback);
  pending_request_.reset();

  status_cb(status, std::move(link));
}

void LowEnergyConnector::OnCreateConnectionTimeout() {
  ZX_DEBUG_ASSERT(pending_request_);
  bt_log(INFO, "hci-le", "create connection timed out: canceling request");

  // TODO(armansito): This should cancel the connection attempt only if the
  // connection attempt isn't using the white list.
  CancelInternal(true);
}

}  // namespace hci
}  // namespace bt
