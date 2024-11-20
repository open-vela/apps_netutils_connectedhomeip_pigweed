// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "logical_link.h"

#include <zircon/assert.h>

#include <functional>

#include "bredr_dynamic_channel.h"
#include "bredr_signaling_channel.h"
#include "channel.h"
#include "le_signaling_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/run_or_post.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace bt {
namespace l2cap {
namespace internal {
namespace {

constexpr bool IsValidLEFixedChannel(ChannelId id) {
  switch (id) {
    case kATTChannelId:
    case kLESignalingChannelId:
    case kLESMPChannelId:
      return true;
    default:
      break;
  }
  return false;
}

constexpr bool IsValidBREDRFixedChannel(ChannelId id) {
  switch (id) {
    case kSignalingChannelId:
    case kConnectionlessChannelId:
    case kSMPChannelId:
      return true;
    default:
      break;
  }
  return false;
}

}  // namespace

// static
fbl::RefPtr<LogicalLink> LogicalLink::New(
    hci::ConnectionHandle handle, hci::Connection::LinkType type, hci::Connection::Role role,
    async_dispatcher_t* dispatcher, size_t max_acl_payload_size,
    SendPacketsCallback send_packets_cb, DropQueuedAclCallback drop_queued_acl_cb,
    QueryServiceCallback query_service_cb) {
  auto ll = fbl::AdoptRef(new LogicalLink(handle, type, role, dispatcher, max_acl_payload_size,
                                          std::move(send_packets_cb), std::move(drop_queued_acl_cb),
                                          std::move(query_service_cb)));
  ll->Initialize();
  return ll;
}

LogicalLink::LogicalLink(hci::ConnectionHandle handle, hci::Connection::LinkType type,
                         hci::Connection::Role role, async_dispatcher_t* dispatcher,
                         size_t max_acl_payload_size, SendPacketsCallback send_packets_cb,
                         DropQueuedAclCallback drop_queued_acl_cb,
                         QueryServiceCallback query_service_cb)
    : dispatcher_(dispatcher),
      handle_(handle),
      type_(type),
      role_(role),
      closed_(false),
      fragmenter_(handle, max_acl_payload_size),
      recombiner_(handle),
      send_packets_cb_(std::move(send_packets_cb)),
      drop_queued_acl_cb_(std::move(drop_queued_acl_cb)),
      query_service_cb_(std::move(query_service_cb)),
      weak_ptr_factory_(this) {
  ZX_ASSERT(dispatcher_);
  ZX_ASSERT(type_ == hci::Connection::LinkType::kLE || type_ == hci::Connection::LinkType::kACL);
  ZX_ASSERT(send_packets_cb_);
  ZX_ASSERT(drop_queued_acl_cb_);
  ZX_ASSERT(query_service_cb_);
}

void LogicalLink::Initialize() {
  ZX_DEBUG_ASSERT(!signaling_channel_);
  ZX_DEBUG_ASSERT(!dynamic_registry_);

  // Set up the signaling channel and dynamic channels.
  if (type_ == hci::Connection::LinkType::kLE) {
    signaling_channel_ =
        std::make_unique<LESignalingChannel>(OpenFixedChannel(kLESignalingChannelId), role_);
    // TODO(armansito): Initialize LE registry when it exists.

    ServeConnectionParameterUpdateRequest();
  } else {
    signaling_channel_ =
        std::make_unique<BrEdrSignalingChannel>(OpenFixedChannel(kSignalingChannelId), role_);
    dynamic_registry_ = std::make_unique<BrEdrDynamicChannelRegistry>(
        signaling_channel_.get(), fit::bind_member(this, &LogicalLink::OnChannelDisconnectRequest),
        fit::bind_member(this, &LogicalLink::OnServiceRequest));

    SendFixedChannelsSupportedInformationRequest();
  }
}

LogicalLink::~LogicalLink() {
  bt_log(DEBUG, "l2cap", "LogicalLink destroyed (handle: %#.4x)", handle_);
  ZX_ASSERT(closed_);
}

fbl::RefPtr<Channel> LogicalLink::OpenFixedChannel(ChannelId id) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(!closed_);

  TRACE_DURATION("bluetooth", "LogicalLink::OpenFixedChannel", "handle", handle_, "channel id", id);

  // We currently only support the pre-defined fixed-channels.
  if (!AllowsFixedChannel(id)) {
    bt_log(ERROR, "l2cap", "cannot open fixed channel with id %#.4x", id);
    return nullptr;
  }

  auto iter = channels_.find(id);
  if (iter != channels_.end()) {
    bt_log(ERROR, "l2cap", "channel is already open! (id: %#.4x, handle: %#.4x)", id, handle_);
    return nullptr;
  }

  auto chan = ChannelImpl::CreateFixedChannel(id, GetWeakPtr());

  auto pp_iter = pending_pdus_.find(id);
  if (pp_iter != pending_pdus_.end()) {
    for (auto& pdu : pp_iter->second) {
      TRACE_FLOW_END("bluetooth", "LogicalLink::HandleRxPacket queued", pdu.trace_id());
      chan->HandleRxPdu(std::move(pdu));
    }
    pending_pdus_.erase(pp_iter);
  }

  channels_[id] = chan;

  return chan;
}

void LogicalLink::OpenChannel(PSM psm, ChannelParameters params, ChannelCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(!closed_);

  // TODO(NET-1437): Implement channels for LE credit-based connections
  if (type_ == hci::Connection::LinkType::kLE) {
    bt_log(WARN, "l2cap", "not opening LE channel for PSM %.4x", psm);
    CompleteDynamicOpen(nullptr, std::move(callback));
    return;
  }

  auto create_channel = [this, cb = std::move(callback)](const DynamicChannel* dyn_chan) mutable {
    CompleteDynamicOpen(dyn_chan, std::move(cb));
  };
  dynamic_registry_->OpenOutbound(psm, params, std::move(create_channel));
}

void LogicalLink::HandleRxPacket(hci::ACLDataPacketPtr packet) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(packet);
  ZX_DEBUG_ASSERT(!closed_);

  TRACE_DURATION("bluetooth", "LogicalLink::HandleRxPacket", "handle", handle_);

  auto result = recombiner_.ConsumeFragment(std::move(packet));
  if (result.frames_dropped) {
    bt_log(TRACE, "l2cap", "Frame(s) dropped due to recombination error");
  }

  if (!result.pdu) {
    // Either a partial fragment was received, which was buffered for recombination, or the packet
    // was dropped.
    return;
  }

  ZX_DEBUG_ASSERT(result.pdu->is_valid());

  uint16_t channel_id = result.pdu->channel_id();
  auto iter = channels_.find(channel_id);
  PendingPduMap::iterator pp_iter;

  if (iter == channels_.end()) {
    // Only buffer data for fixed channels. This prevents stale data that is
    // intended for a closed dynamic channel from being delivered to a new
    // channel that recycled the former's ID. The downside is that it's possible
    // to lose any data that is received after a dynamic channel's connection
    // request and before its completed configuration. This would require tricky
    // additional state to track "pending open" channels here and it's not clear
    // if that is necessary since hosts should not send data before a channel is
    // first configured.
    if (!AllowsFixedChannel(channel_id)) {
      bt_log(WARN, "l2cap", "Dropping PDU for nonexistent dynamic channel %#.4x on link %#.4x",
             channel_id, handle_);
      return;
    }

    // The packet was received on a channel for which no ChannelImpl currently
    // exists. Buffer packets for the channel to receive when it gets created.
    pp_iter = pending_pdus_.emplace(channel_id, std::list<PDU>()).first;
  } else {
    // A channel exists. |pp_iter| will be valid only if the drain task has not
    // run yet (see LogicalLink::OpenFixedChannel()).
    pp_iter = pending_pdus_.find(channel_id);
  }

  if (pp_iter != pending_pdus_.end()) {
    result.pdu->set_trace_id(TRACE_NONCE());
    TRACE_FLOW_BEGIN("bluetooth", "LogicalLink::HandleRxPacket queued", result.pdu->trace_id());
    pp_iter->second.emplace_back(std::move(*result.pdu));

    bt_log(TRACE, "l2cap", "PDU buffered (channel: %#.4x, ll: %#.4x)", channel_id, handle_);
    return;
  }

  iter->second->HandleRxPdu(std::move(*result.pdu));
}

void LogicalLink::UpgradeSecurity(sm::SecurityLevel level, sm::StatusCallback callback,
                                  async_dispatcher_t* dispatcher) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(security_callback_);
  ZX_DEBUG_ASSERT(dispatcher);

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignoring security request on closed link");
    return;
  }

  auto status_cb = [dispatcher, f = std::move(callback)](sm::Status status) mutable {
    async::PostTask(dispatcher, [f = std::move(f), status] { f(status); });
  };

  // Report success If the link already has the expected security level.
  if (level <= security().level()) {
    status_cb(sm::Status());
    return;
  }

  bt_log(DEBUG, "l2cap", "Security upgrade requested (level = %s)", sm::LevelToString(level));
  security_callback_(handle_, level, std::move(status_cb));
}

void LogicalLink::AssignSecurityProperties(const sm::SecurityProperties& security) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignoring security request on closed link");
    return;
  }

  bt_log(DEBUG, "l2cap", "Link security updated (handle: %#.4x): %s", handle_,
         security.ToString().c_str());

  std::lock_guard<std::mutex> lock(mtx_);
  security_ = security;
}

void LogicalLink::SendFrame(ChannelId id, const ByteBuffer& payload,
                            FrameCheckSequenceOption fcs_option) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Drop out-bound packet on closed link");
    return;
  }

  // Copy payload into L2CAP frame fragments, sized for the HCI data transport.
  PDU pdu = fragmenter_.BuildFrame(id, payload, fcs_option);
  auto fragments = pdu.ReleaseFragments();

  ZX_ASSERT(!fragments.is_empty());
  send_packets_cb_(std::move(fragments), id);
}

void LogicalLink::set_error_callback(fit::closure callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  link_error_cb_ = std::move(callback);
}

void LogicalLink::set_security_upgrade_callback(SecurityUpgradeCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  security_callback_ = std::move(callback);
}

void LogicalLink::set_connection_parameter_update_callback(
    LEConnectionParameterUpdateCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  connection_parameter_update_callback_ = std::move(callback);
}

LESignalingChannel* LogicalLink::le_signaling_channel() const {
  return (type_ == hci::Connection::LinkType::kLE)
             ? static_cast<LESignalingChannel*>(signaling_channel_.get())
             : nullptr;
}

bool LogicalLink::AllowsFixedChannel(ChannelId id) {
  return (type_ == hci::Connection::LinkType::kLE) ? IsValidLEFixedChannel(id)
                                                   : IsValidBREDRFixedChannel(id);
}

void LogicalLink::RemoveChannel(Channel* chan) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(chan);

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignore RemoveChannel() on closed link");
    return;
  }

  const ChannelId id = chan->id();
  auto iter = channels_.find(id);
  if (iter == channels_.end())
    return;

  // Ignore if the found channel doesn't match the requested one (even though
  // their IDs are the same).
  if (iter->second.get() != chan)
    return;

  pending_pdus_.erase(id);
  channels_.erase(iter);

  // Drop stale packets queued for this channel.
  hci::ACLPacketPredicate predicate = [this, id](const auto& packet, l2cap::ChannelId channel_id) {
    return packet->connection_handle() == handle_ && id == channel_id;
  };
  drop_queued_acl_cb_(std::move(predicate));

  // Disconnect the channel if it's a dynamic channel. This path is for local-
  // initiated closures and does not invoke callbacks back to the channel user.
  // TODO(armansito): Change this if statement into an assert when a registry
  // gets created for LE channels.
  if (dynamic_registry_) {
    dynamic_registry_->CloseChannel(id);
  }
}

void LogicalLink::SignalError() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignore SignalError() on closed link");
    return;
  }

  bt_log(INFO, "l2cap", "Signal upper layer error on link %#.4x; closing all channels", handle());

  for (auto channel_iter = channels_.begin(); channel_iter != channels_.end();) {
    auto& [_, channel] = *channel_iter++;

    // Signal the channel, as it did not request the closure.
    channel->OnClosed();

    // This erases from |channel_| and invalidates any iterator pointing to |channel|.
    RemoveChannel(channel.get());
  }

  if (link_error_cb_) {
    // TODO(fxbug.dev/53985): This should be removed when l2cap::Channel is no longer a thread-safe
    // message-passing interface. But while it is, the above channel teardown work will be posting
    // L2CAP Signaling Channel Disconnection Requests that must go out before we request a GAP
    // disconnection of the underlying link with this callback.
    async::PostTask(dispatcher(), link_error_cb_.share());
  }

  // Link is expected to be closed by its owner.
}

void LogicalLink::Close() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(!closed_);

  closed_ = true;

  auto channels = std::move(channels_);
  for (auto& iter : channels) {
    iter.second->OnClosed();
  }
}

std::optional<DynamicChannelRegistry::ServiceInfo> LogicalLink::OnServiceRequest(PSM psm) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(!closed_);

  // Query upper layer for a service handler attached to this PSM.
  auto result = query_service_cb_(handle_, psm);
  if (!result) {
    return std::nullopt;
  }

  auto channel_cb =
      [this, chan_cb = std::move(result->channel_cb)](const DynamicChannel* dyn_chan) mutable {
        CompleteDynamicOpen(dyn_chan, std::move(chan_cb));
      };
  return DynamicChannelRegistry::ServiceInfo(result->channel_params, std::move(channel_cb));
}

void LogicalLink::OnChannelDisconnectRequest(const DynamicChannel* dyn_chan) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(dyn_chan);
  ZX_DEBUG_ASSERT(!closed_);

  auto iter = channels_.find(dyn_chan->local_cid());
  if (iter == channels_.end()) {
    bt_log(WARN, "l2cap", "No ChannelImpl found for closing dynamic channel %#.4x",
           dyn_chan->local_cid());
    return;
  }

  fbl::RefPtr<ChannelImpl> channel = std::move(iter->second);
  ZX_DEBUG_ASSERT(channel->remote_id() == dyn_chan->remote_cid());
  channels_.erase(iter);

  hci::ACLPacketPredicate predicate = [this, id = channel->id()](const auto& packet,
                                                                 l2cap::ChannelId channel_id) {
    return packet->connection_handle() == handle_ && id == channel_id;
  };
  drop_queued_acl_cb_(std::move(predicate));

  // Signal closure because this is a remote disconnection.
  channel->OnClosed();
}

void LogicalLink::CompleteDynamicOpen(const DynamicChannel* dyn_chan, ChannelCallback open_cb) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(!closed_);

  if (!dyn_chan) {
    open_cb(nullptr);
    return;
  }

  const ChannelId local_cid = dyn_chan->local_cid();
  const ChannelId remote_cid = dyn_chan->remote_cid();
  bt_log(DEBUG, "l2cap", "Link %#.4x: Channel opened with ID %#.4x (remote ID %#.4x)", handle_,
         local_cid, remote_cid);

  auto chan =
      ChannelImpl::CreateDynamicChannel(local_cid, remote_cid, GetWeakPtr(), dyn_chan->info());
  channels_[local_cid] = chan;
  open_cb(std::move(chan));
}

void LogicalLink::SendFixedChannelsSupportedInformationRequest() {
  ZX_ASSERT(signaling_channel_);

  BrEdrCommandHandler cmd_handler(signaling_channel_.get());
  if (!cmd_handler.SendInformationRequest(InformationType::kFixedChannelsSupported,
                                          [self = GetWeakPtr()](auto& rsp) {
                                            if (self) {
                                              self->OnRxFixedChannelsSupportedInfoRsp(rsp);
                                            }
                                          })) {
    bt_log(ERROR, "l2cap", "Failed to send Fixed Channels Supported Information Request");
    return;
  }

  bt_log(TRACE, "l2cap", "Sent Fixed Channels Supported Information Request");
}

void LogicalLink::OnRxFixedChannelsSupportedInfoRsp(
    const BrEdrCommandHandler::InformationResponse& rsp) {
  if (rsp.status() == BrEdrCommandHandler::Status::kReject) {
    bt_log(TRACE, "l2cap", "Fixed Channels Supported Information Request rejected (reason %#.4hx)",
           rsp.reject_reason());
    return;
  }

  if (rsp.result() == InformationResult::kNotSupported) {
    bt_log(TRACE, "l2cap",
           "Received Fixed Channels Supported Information Response (result: Not Supported)");
    return;
  }

  if (rsp.result() != InformationResult::kSuccess) {
    bt_log(TRACE, "l2cap", "Received Fixed Channels Supported Information Response (result: %.4hx)",
           static_cast<uint16_t>(rsp.result()));
    return;
  }

  if (rsp.type() != InformationType::kFixedChannelsSupported) {
    bt_log(TRACE, "l2cap",
           "Incorrect Fixed Channels Supported Information Response type (type: %#.4hx)",
           rsp.type());
    return;
  }

  bt_log(TRACE, "l2cap", "Received Fixed Channels Supported Information Response (mask: %#016lx)",
         rsp.fixed_channels());
}

void LogicalLink::SendConnectionParameterUpdateRequest(
    hci::LEPreferredConnectionParameters params,
    ConnectionParameterUpdateRequestCallback request_cb) {
  ZX_ASSERT(signaling_channel_);
  ZX_ASSERT(type_ == hci::Connection::LinkType::kLE);
  ZX_ASSERT(role_ == hci::Connection::Role::kSlave);

  LowEnergyCommandHandler cmd_handler(signaling_channel_.get());
  cmd_handler.SendConnectionParameterUpdateRequest(
      params.min_interval(), params.max_interval(), params.max_latency(),
      params.supervision_timeout(),
      [cb = std::move(request_cb)](
          const LowEnergyCommandHandler::ConnectionParameterUpdateResponse& rsp) mutable {
        bool accepted = false;

        if (rsp.status() != LowEnergyCommandHandler::Status::kSuccess) {
          bt_log(TRACE, "l2cap", "LE Connection Parameter Update Request rejected (reason: %#.4hx)",
                 rsp.reject_reason());
        } else {
          accepted = rsp.result() == ConnectionParameterUpdateResult::kAccepted;
        }
        cb(accepted);
      });
}

void LogicalLink::ServeConnectionParameterUpdateRequest() {
  ZX_ASSERT(signaling_channel_);
  ZX_ASSERT(type_ == hci::Connection::LinkType::kLE);

  LowEnergyCommandHandler cmd_handler(signaling_channel_.get());
  cmd_handler.ServeConnectionParameterUpdateRequest(
      fit::bind_member(this, &LogicalLink::OnRxConnectionParameterUpdateRequest));
}

void LogicalLink::OnRxConnectionParameterUpdateRequest(
    uint16_t interval_min, uint16_t interval_max, uint16_t slave_latency,
    uint16_t timeout_multiplier,
    LowEnergyCommandHandler::ConnectionParameterUpdateResponder* responder) {
  // Only a LE slave can send this command. "If an LE slave Host receives a
  // Connection Parameter Update Request packet it shall respond with a Command
  // Reject Packet [...]" (v5.0, Vol 3, Part A, Section 4.20).
  if (role_ == hci::Connection::Role::kSlave) {
    bt_log(DEBUG, "l2cap", "rejecting conn. param. update request from master");
    responder->RejectNotUnderstood();
    return;
  }

  // Reject the connection parameters if they are outside the ranges allowed by
  // the HCI specification (see HCI_LE_Connection_Update command v5.0, Vol 2,
  // Part E, Section 7.8.18).
  bool reject = false;

  hci::LEPreferredConnectionParameters params(interval_min, interval_max, slave_latency,
                                              timeout_multiplier);

  if (params.min_interval() > params.max_interval()) {
    bt_log(DEBUG, "l2cap", "conn. min interval larger than max");
    reject = true;
  } else if (params.min_interval() < hci::kLEConnectionIntervalMin) {
    bt_log(DEBUG, "l2cap", "conn. min interval outside allowed range: %#.4x",
           params.min_interval());
    reject = true;
  } else if (params.max_interval() > hci::kLEConnectionIntervalMax) {
    bt_log(DEBUG, "l2cap", "conn. max interval outside allowed range: %#.4x",
           params.max_interval());
    reject = true;
  } else if (params.max_latency() > hci::kLEConnectionLatencyMax) {
    bt_log(DEBUG, "l2cap", "conn. slave latency too large: %#.4x", params.max_latency());
    reject = true;
  } else if (params.supervision_timeout() < hci::kLEConnectionSupervisionTimeoutMin ||
             params.supervision_timeout() > hci::kLEConnectionSupervisionTimeoutMax) {
    bt_log(DEBUG, "l2cap", "conn supv. timeout outside allowed range: %#.4x",
           params.supervision_timeout());
    reject = true;
  }

  ConnectionParameterUpdateResult result = reject ? ConnectionParameterUpdateResult::kRejected
                                                  : ConnectionParameterUpdateResult::kAccepted;
  responder->Send(result);

  if (!reject) {
    if (!connection_parameter_update_callback_) {
      bt_log(DEBUG, "l2cap", "no callback set for LE Connection Parameter Update Request");
      return;
    }

    connection_parameter_update_callback_(params);
  }
}
}  // namespace internal
}  // namespace l2cap
}  // namespace bt
