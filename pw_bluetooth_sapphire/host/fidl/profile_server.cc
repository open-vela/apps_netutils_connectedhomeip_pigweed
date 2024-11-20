// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "profile_server.h"

#include <zircon/status.h>

#include "helpers.h"
#include "lib/fit/result.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/uuid.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/types.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/types.h"
#include "src/connectivity/bluetooth/core/bt-host/sdp/status.h"

using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Status;

namespace fidlbredr = fuchsia::bluetooth::bredr;
using fidlbredr::DataElement;
using fidlbredr::Profile;

namespace bthost {

namespace {

bt::l2cap::ChannelParameters FidlToChannelParameters(const fidlbredr::ChannelParameters& fidl) {
  bt::l2cap::ChannelParameters params;
  if (fidl.has_channel_mode()) {
    switch (fidl.channel_mode()) {
      case fidlbredr::ChannelMode::BASIC:
        params.mode = bt::l2cap::ChannelMode::kBasic;
        break;
      case fidlbredr::ChannelMode::ENHANCED_RETRANSMISSION:
        params.mode = bt::l2cap::ChannelMode::kEnhancedRetransmission;
        break;
      default:
        ZX_PANIC("FIDL channel parameter contains invalid mode");
    }
  }
  if (fidl.has_max_rx_sdu_size()) {
    params.max_rx_sdu_size = fidl.max_rx_sdu_size();
  }
  return params;
}

fidlbredr::ChannelMode ChannelModeToFidl(bt::l2cap::ChannelMode mode) {
  switch (mode) {
    case bt::l2cap::ChannelMode::kBasic:
      return fidlbredr::ChannelMode::BASIC;
      break;
    case bt::l2cap::ChannelMode::kEnhancedRetransmission:
      return fidlbredr::ChannelMode::ENHANCED_RETRANSMISSION;
      break;
    default:
      ZX_PANIC("Could not convert channel parameter mode to unsupported FIDL mode");
  }
}

fidlbredr::DataElementPtr DataElementToFidl(const bt::sdp::DataElement* in) {
  auto elem = fidlbredr::DataElement::New();
  bt_log(TRACE, "fidl", "DataElementToFidl: %s", in->ToString().c_str());
  ZX_DEBUG_ASSERT(in);
  switch (in->type()) {
    case bt::sdp::DataElement::Type::kUnsignedInt: {
      switch (in->size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          elem->set_uint8(*in->Get<uint8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          elem->set_uint16(*in->Get<uint16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          elem->set_uint32(*in->Get<uint32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          elem->set_uint64(*in->Get<uint64_t>());
          break;
        default:
          bt_log(INFO, "fidl", "no 128-bit integer support in FIDL yet");
          return nullptr;
      }
      return elem;
    }
    case bt::sdp::DataElement::Type::kSignedInt: {
      switch (in->size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          elem->set_int8(*in->Get<int8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          elem->set_int16(*in->Get<int16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          elem->set_int32(*in->Get<int32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          elem->set_int64(*in->Get<int64_t>());
          break;
        default:
          bt_log(INFO, "fidl", "no 128-bit integer support in FIDL yet");
          return nullptr;
      }
      return elem;
    }
    case bt::sdp::DataElement::Type::kUuid: {
      auto uuid = in->Get<bt::UUID>();
      ZX_DEBUG_ASSERT(uuid);
      elem->set_uuid(fidl_helpers::UuidToFidl(*uuid));
      return elem;
    }
    case bt::sdp::DataElement::Type::kString: {
      elem->set_str(*in->Get<std::string>());
      return elem;
    }
    case bt::sdp::DataElement::Type::kBoolean: {
      elem->set_b(*in->Get<bool>());
      return elem;
    }
    case bt::sdp::DataElement::Type::kSequence: {
      std::vector<fidlbredr::DataElementPtr> elems;
      const bt::sdp::DataElement* it;
      for (size_t idx = 0; (it = in->At(idx)); ++idx) {
        elems.emplace_back(DataElementToFidl(it));
      }
      elem->set_sequence(std::move(elems));
      return elem;
    }
    case bt::sdp::DataElement::Type::kAlternative: {
      std::vector<fidlbredr::DataElementPtr> elems;
      const bt::sdp::DataElement* it;
      for (size_t idx = 0; (it = in->At(idx)); ++idx) {
        elems.emplace_back(DataElementToFidl(it));
      }
      elem->set_alternatives(std::move(elems));
      return elem;
    }
    case bt::sdp::DataElement::Type::kUrl: {
      bt_log(INFO, "fidl", "no support for Url types in DataElement yet");
      return nullptr;
    }
    case bt::sdp::DataElement::Type::kNull: {
      bt_log(INFO, "fidl", "no support for null DataElement types in FIDL");
      return nullptr;
    }
  }
}

fidlbredr::ProtocolDescriptorPtr DataElementToProtocolDescriptor(const bt::sdp::DataElement* in) {
  auto desc = fidlbredr::ProtocolDescriptor::New();
  if (in->type() != bt::sdp::DataElement::Type::kSequence) {
    return nullptr;
  }
  const auto protocol_uuid = in->At(0)->Get<bt::UUID>();
  if (!protocol_uuid) {
    return nullptr;
  }
  desc->protocol = fidlbredr::ProtocolIdentifier(*protocol_uuid->As16Bit());
  const bt::sdp::DataElement* it;
  for (size_t idx = 1; (it = in->At(idx)); ++idx) {
    desc->params.push_back(std::move(*DataElementToFidl(it)));
  }

  return desc;
}

bt::l2cap::AclPriority FidlToAclPriority(fidlbredr::A2dpDirectionPriority in) {
  switch (in) {
    case fidlbredr::A2dpDirectionPriority::SOURCE:
      return bt::l2cap::AclPriority::kSource;
    case fidlbredr::A2dpDirectionPriority::SINK:
      return bt::l2cap::AclPriority::kSink;
    default:
      return bt::l2cap::AclPriority::kNormal;
  }
}

}  // namespace

ProfileServer::ProfileServer(fxl::WeakPtr<bt::gap::Adapter> adapter,
                             fidl::InterfaceRequest<Profile> request)
    : ServerBase(this, std::move(request)),
      advertised_total_(0),
      searches_total_(0),
      adapter_(adapter),
      weak_ptr_factory_(this) {}

ProfileServer::~ProfileServer() {
  if (adapter()) {
    // Unregister anything that we have registered.
    for (const auto& it : current_advertised_) {
      adapter()->bredr()->UnregisterService(it.second.registration_handle);
      it.second.disconnection_cb(fit::ok());
    }
    for (const auto& it : searches_) {
      adapter()->bredr()->RemoveServiceSearch(it.second.search_id);
    }
  }
}

void ProfileServer::Advertise(
    std::vector<fidlbredr::ServiceDefinition> definitions, fidlbredr::ChannelParameters parameters,
    fidl::InterfaceHandle<fuchsia::bluetooth::bredr::ConnectionReceiver> receiver,
    AdvertiseCallback callback) {
  // TODO: check that the service definition is valid for useful error messages

  std::vector<bt::sdp::ServiceRecord> registering;

  for (auto& definition : definitions) {
    auto rec = fidl_helpers::ServiceDefinitionToServiceRecord(definition);
    // Drop the receiver on error.
    if (rec.is_error()) {
      bt_log(WARN, "fidl", "%s: Failed to create service record from service defintion",
             __FUNCTION__);
      callback(fit::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
      return;
    }
    registering.emplace_back(std::move(rec.value()));
  }

  ZX_ASSERT(adapter());
  ZX_ASSERT(adapter()->bredr());

  uint64_t next = advertised_total_ + 1;

  auto registration_handle = adapter()->bredr()->RegisterService(
      std::move(registering), FidlToChannelParameters(parameters),
      [this, next](auto channel, const auto& protocol_list) {
        OnChannelConnected(next, std::move(channel), std::move(protocol_list));
      });

  if (!registration_handle) {
    bt_log(WARN, "fidl", "%s: Failed to register service", __FUNCTION__);
    callback(fit::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  };

  auto receiverptr = receiver.Bind();

  receiverptr.set_error_handler(
      [this, next](zx_status_t status) { OnConnectionReceiverError(next, status); });

  current_advertised_.try_emplace(next, std::move(receiverptr), registration_handle,
                                  std::move(callback));
  advertised_total_ = next;
}

void ProfileServer::Search(
    fidlbredr::ServiceClassProfileIdentifier service_uuid, std::vector<uint16_t> attr_ids,
    fidl::InterfaceHandle<fuchsia::bluetooth::bredr::SearchResults> results) {
  bt::UUID search_uuid(static_cast<uint32_t>(service_uuid));
  std::unordered_set<bt::sdp::AttributeId> attributes(attr_ids.begin(), attr_ids.end());
  if (!attr_ids.empty()) {
    // Always request the ProfileDescriptor for the event
    attributes.insert(bt::sdp::kBluetoothProfileDescriptorList);
  }

  ZX_DEBUG_ASSERT(adapter());

  auto next = searches_total_ + 1;

  auto search_id = adapter()->bredr()->AddServiceSearch(
      search_uuid, std::move(attributes),
      [this, next](auto id, const auto& attrs) { OnServiceFound(next, id, attrs); });

  if (!search_id) {
    return;
  }

  auto results_ptr = results.Bind();
  results_ptr.set_error_handler(
      [this, next](zx_status_t status) { OnSearchResultError(next, status); });

  searches_.try_emplace(next, std::move(results_ptr), search_id);
  searches_total_ = next;
}

void ProfileServer::Connect(fuchsia::bluetooth::PeerId peer_id,
                            fidlbredr::ConnectParameters connection, ConnectCallback callback) {
  bt::PeerId id{peer_id.value};

  // Anything other than L2CAP is not supported by this server.
  if (!connection.is_l2cap()) {
    bt_log(WARN, "fidl", "%s: non-l2cap connections are not supported (is_rfcomm: %d, peer: %s)",
           __FUNCTION__, connection.is_rfcomm(), bt_str(id));
    callback(fit::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  }

  // The L2CAP parameters must include a PSM. ChannelParameters are optional.
  auto l2cap_params = std::move(connection.l2cap());
  if (!l2cap_params.has_psm()) {
    bt_log(WARN, "fidl", "%s: missing l2cap psm (peer: %s)", __FUNCTION__, bt_str(id));
    callback(fit::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  }
  uint16_t psm = l2cap_params.psm();

  fidlbredr::ChannelParameters parameters = std::move(*l2cap_params.mutable_parameters());

  auto connected_cb = [self = weak_ptr_factory_.GetWeakPtr(), cb = callback.share(), id,
                       func = __FUNCTION__](fbl::RefPtr<bt::l2cap::Channel> chan) {
    if (!chan) {
      bt_log(INFO, "fidl", "%s: Channel socket is empty, returning failed. (peer: %s)", func,
             bt_str(id));
      cb(fit::error(fuchsia::bluetooth::ErrorCode::FAILED));
      return;
    }

    if (!self) {
      cb(fit::error(fuchsia::bluetooth::ErrorCode::FAILED));
      return;
    }

    auto fidl_chan = self->ChannelToFidl(std::move(chan));

    cb(fit::ok(std::move(fidl_chan)));
  };
  ZX_DEBUG_ASSERT(adapter());

  adapter()->bredr()->OpenL2capChannel(
      id, psm, fidl_helpers::FidlToBrEdrSecurityRequirements(parameters),
      FidlToChannelParameters(parameters), std::move(connected_cb));
}

void ProfileServer::ConnectSco(
    ::fuchsia::bluetooth::PeerId fidl_peer_id, bool initiator,
    fuchsia::bluetooth::bredr::ScoConnectionParameters fidl_params,
    fidl::InterfaceHandle<fuchsia::bluetooth::bredr::ScoConnectionReceiver> receiver) {
  bt::PeerId peer_id(fidl_peer_id.value);
  auto client = receiver.Bind();

  auto params_result = fidl_helpers::FidlToScoParameters(fidl_params);
  if (params_result.is_error()) {
    bt_log(WARN, "fidl", "%s: invalid parameters (peer: %s)", __FUNCTION__, bt_str(peer_id));
    client->Error(fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
    return;
  }
  auto params = params_result.value();

  auto request = fbl::MakeRefCounted<ScoRequest>();
  client.set_error_handler([request](zx_status_t status) { request->request_handle.reset(); });
  request->receiver = std::move(client);

  auto callback = [self = weak_ptr_factory_.GetWeakPtr(), request](auto result) {
    // The connection may complete after this server is destroyed.
    if (!self) {
      // Prevent leaking connections.
      if (result.is_ok()) {
        result.value()->Deactivate();
      }
      return;
    }

    self->OnScoConnectionResult(request, std::move(result));
  };

  request->request_handle =
      adapter()->bredr()->OpenScoConnection(peer_id, initiator, params, std::move(callback));
}

void ProfileServer::OnChannelConnected(uint64_t ad_id, fbl::RefPtr<bt::l2cap::Channel> channel,
                                       const bt::sdp::DataElement& protocol_list) {
  auto it = current_advertised_.find(ad_id);
  if (it == current_advertised_.end()) {
    // The receiver has disappeared, do nothing.
    return;
  }

  ZX_DEBUG_ASSERT(adapter());
  auto handle = channel->link_handle();
  auto id = adapter()->bredr()->GetPeerId(handle);

  // The protocol that is connected should be L2CAP, because that is the only thing that
  // we can connect. We can't say anything about what the higher level protocols will be.
  auto prot_seq = protocol_list.At(0);
  ZX_ASSERT(prot_seq);

  fidlbredr::ProtocolDescriptorPtr desc = DataElementToProtocolDescriptor(prot_seq);
  ZX_ASSERT(desc);

  fuchsia::bluetooth::PeerId peer_id{id.value()};

  std::vector<fidlbredr::ProtocolDescriptor> list;
  list.emplace_back(std::move(*desc));

  auto fidl_chan = ChannelToFidl(std::move(channel));

  it->second.receiver->Connected(peer_id, std::move(fidl_chan), std::move(list));
}

void ProfileServer::OnConnectionReceiverError(uint64_t ad_id, zx_status_t status) {
  bt_log(DEBUG, "fidl", "Connection receiver closed, ending advertisement %lu", ad_id);

  auto it = current_advertised_.find(ad_id);

  if (it == current_advertised_.end() || !adapter()) {
    return;
  }

  adapter()->bredr()->UnregisterService(it->second.registration_handle);
  it->second.disconnection_cb(fit::ok());

  current_advertised_.erase(it);
}

void ProfileServer::OnSearchResultError(uint64_t search_id, zx_status_t status) {
  bt_log(DEBUG, "fidl", "Search result closed, ending search %lu reason %s", search_id,
         zx_status_get_string(status));

  auto it = searches_.find(search_id);

  if (it == searches_.end() || !adapter()) {
    return;
  }

  adapter()->bredr()->RemoveServiceSearch(it->second.search_id);

  searches_.erase(it);
}

void ProfileServer::OnServiceFound(
    uint64_t search_id, bt::PeerId peer_id,
    const std::map<bt::sdp::AttributeId, bt::sdp::DataElement>& attributes) {
  auto search_it = searches_.find(search_id);
  if (search_it == searches_.end()) {
    // Search was de-registered.
    return;
  }

  // Convert ProfileDescriptor Attribute
  auto it = attributes.find(bt::sdp::kProtocolDescriptorList);

  fidl::VectorPtr<fidlbredr::ProtocolDescriptor> descriptor_list;

  if (it != attributes.end()) {
    std::vector<fidlbredr::ProtocolDescriptor> list;
    size_t idx = 0;
    auto* sdp_list_element = it->second.At(idx);
    while (sdp_list_element != nullptr) {
      fidlbredr::ProtocolDescriptorPtr desc = DataElementToProtocolDescriptor(sdp_list_element);
      if (!desc) {
        break;
      }
      list.push_back(std::move(*desc));
      sdp_list_element = it->second.At(++idx);
    }
    descriptor_list = std::move(list);
  }

  // Add the rest of the attributes
  std::vector<fidlbredr::Attribute> fidl_attrs;

  for (const auto& it : attributes) {
    auto attr = fidlbredr::Attribute::New();
    attr->id = it.first;
    attr->element = std::move(*DataElementToFidl(&it.second));
    fidl_attrs.emplace_back(std::move(*attr));
  }

  fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  search_it->second.results->ServiceFound(fidl_peer_id, std::move(descriptor_list),
                                          std::move(fidl_attrs), []() {});
}

void ProfileServer::OnScoConnectionResult(fbl::RefPtr<ScoRequest> request,
                                          bt::sco::ScoConnectionManager::ConnectionResult result) {
  auto receiver = std::move(request->receiver);

  if (result.is_error()) {
    if (!receiver.is_bound()) {
      return;
    }

    bt_log(INFO, "fidl", "%s: SCO connection failed (status: %s)", __FUNCTION__,
           bt::HostErrorToString(result.error()).c_str());

    if (result.error() == bt::HostError::kCanceled) {
      receiver->Error(fuchsia::bluetooth::bredr::ScoErrorCode::CANCELLED);
      return;
    }
    receiver->Error(fuchsia::bluetooth::bredr::ScoErrorCode::FAILURE);
    return;
  }

  fidlbredr::ScoConnection connection;
  connection.set_socket(sco_socket_factory_.MakeSocketForChannel(result.value()));

  if (!receiver.is_bound()) {
    return;
  }
  receiver->Connected(std::move(connection));
}

void ProfileServer::OnAudioDirectionExtError(AudioDirectionExt* ext_server, zx_status_t status) {
  bt_log(DEBUG, "fidl", "audio direction ext server closed (reason: %s)",
         zx_status_get_string(status));

  auto it = audio_direction_ext_servers_.find(ext_server);
  if (it == audio_direction_ext_servers_.end()) {
    bt_log(WARN, "fidl", "could not find ext server in audio direction ext error callback");
    return;
  }

  audio_direction_ext_servers_.erase(it);
}

fidl::InterfaceHandle<fidlbredr::AudioDirectionExt> ProfileServer::BindAudioDirectionExtServer(
    fbl::RefPtr<bt::l2cap::Channel> channel) {
  fidl::InterfaceHandle<fidlbredr::AudioDirectionExt> client;

  auto audio_direction_ext_server =
      std::make_unique<AudioDirectionExt>(client.NewRequest(), std::move(channel));
  AudioDirectionExt* server_ptr = audio_direction_ext_server.get();

  audio_direction_ext_server->set_error_handler(
      [this, server_ptr](zx_status_t status) { OnAudioDirectionExtError(server_ptr, status); });

  audio_direction_ext_servers_[server_ptr] = std::move(audio_direction_ext_server);

  return client;
}

fuchsia::bluetooth::bredr::Channel ProfileServer::ChannelToFidl(
    fbl::RefPtr<bt::l2cap::Channel> channel) {
  ZX_ASSERT(channel);
  fidlbredr::Channel fidl_chan;
  fidl_chan.set_channel_mode(ChannelModeToFidl(channel->mode()));
  fidl_chan.set_max_tx_sdu_size(channel->max_tx_sdu_size());
  auto sock = l2cap_socket_factory_.MakeSocketForChannel(channel);
  fidl_chan.set_socket(std::move(sock));

  if (adapter()->state().vendor_features() & BT_VENDOR_FEATURES_SET_ACL_PRIORITY_COMMAND) {
    fidl_chan.set_ext_direction(BindAudioDirectionExtServer(std::move(channel)));
  }

  return fidl_chan;
}

ProfileServer::AudioDirectionExt::AudioDirectionExt(
    fidl::InterfaceRequest<fidlbredr::AudioDirectionExt> request,
    fbl::RefPtr<bt::l2cap::Channel> channel)
    : ServerBase(this, std::move(request)), channel_(std::move(channel)) {}

void ProfileServer::AudioDirectionExt::SetPriority(
    fuchsia::bluetooth::bredr::A2dpDirectionPriority priority, SetPriorityCallback callback) {
  channel_->RequestAclPriority(FidlToAclPriority(priority),
                               [cb = std::move(callback)](auto result) {
                                 if (result.is_ok()) {
                                   cb(fit::ok());
                                   return;
                                 }
                                 bt_log(DEBUG, "fidl", "ACL priority request failed");
                                 cb(fit::error(fuchsia::bluetooth::ErrorCode::FAILED));
                               });
}

}  // namespace bthost
