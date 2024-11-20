// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bredr_connection.h"

#include <lib/async/default.h>

namespace bt::gap {

namespace {

const char* const kInspectPeerIdPropertyName = "peer_id";

}

BrEdrConnection::BrEdrConnection(fxl::WeakPtr<Peer> peer, std::unique_ptr<hci::Connection> link,
                                 fit::closure send_auth_request_cb,
                                 fit::callback<void()> disconnect_cb,
                                 fit::closure on_peer_disconnect_cb,
                                 fbl::RefPtr<l2cap::L2cap> l2cap,
                                 fxl::WeakPtr<hci::Transport> transport,
                                 std::optional<Request> request)
    : peer_id_(peer->identifier()),
      peer_(std::move(peer)),
      link_(std::move(link)),
      request_(std::move(request)),
      pairing_state_(std::make_unique<PairingState>(
          peer_, link_.get(), request_ && request_->AwaitingOutgoing(),
          std::move(send_auth_request_cb),
          fit::bind_member(this, &BrEdrConnection::OnPairingStateStatus))),
      domain_(std::move(l2cap)),
      sco_manager_(std::make_unique<sco::ScoConnectionManager>(
          peer_id_, link_->handle(), link_->peer_address(), link_->local_address(),
          std::move(transport))),
      create_time_(async::Now(async_get_default_dispatcher())),
      disconnect_cb_(std::move(disconnect_cb)),
      peer_init_token_(request_->take_peer_init_token()),
      peer_conn_token_(peer_->MutBrEdr().RegisterConnection()) {
  link_->set_peer_disconnect_callback([peer_disconnect_cb = std::move(on_peer_disconnect_cb)](
                                          auto conn, auto /*reason*/) { peer_disconnect_cb(); });
}

BrEdrConnection::~BrEdrConnection() {
  if (auto request = std::exchange(request_, std::nullopt); request.has_value()) {
    // Connection never completed so signal the requester(s).
    request->NotifyCallbacks(hci::Status(HostError::kNotSupported), [] { return nullptr; });
  }

  sco_manager_.reset();
  pairing_state_.reset();
  link_.reset();
}

void BrEdrConnection::OnInterrogationComplete() {
  ZX_ASSERT_MSG(!interrogation_complete(), "%s on a connection that's already been interrogated",
                __FUNCTION__);

  // Fulfill and clear request so that the dtor does not signal requester(s) with errors.
  if (auto request = std::exchange(request_, std::nullopt); request.has_value()) {
    request->NotifyCallbacks(hci::Status(), [this] { return this; });
  }
}

void BrEdrConnection::AddRequestCallback(BrEdrConnection::Request::OnComplete cb) {
  if (!request_.has_value()) {
    cb(hci::Status(), this);
    return;
  }

  ZX_ASSERT(request_);
  request_->AddCallback(std::move(cb));
}

void BrEdrConnection::OpenL2capChannel(l2cap::PSM psm, l2cap::ChannelParameters params,
                                       l2cap::ChannelCallback cb) {
  if (!interrogation_complete()) {
    // Connection is not yet ready for L2CAP; return a null channel.
    bt_log(INFO, "gap-bredr", "Connection to %s not complete; canceling channel to PSM %.4x",
           bt_str(peer_id()), psm);
    cb(nullptr);
    return;
  }

  bt_log(INFO, "gap-bredr", "opening l2cap channel on psm %#.4x (peer: %s)", psm,
         bt_str(peer_id()));
  domain_->OpenL2capChannel(link().handle(), psm, params, std::move(cb));
}

BrEdrConnection::ScoRequestHandle BrEdrConnection::OpenScoConnection(
    hci_spec::SynchronousConnectionParameters parameters,
    sco::ScoConnectionManager::OpenConnectionCallback callback) {
  return sco_manager_->OpenConnection(parameters, std::move(callback));
}
BrEdrConnection::ScoRequestHandle BrEdrConnection::AcceptScoConnection(
    std::vector<hci_spec::SynchronousConnectionParameters> parameters,
    sco::ScoConnectionManager::AcceptConnectionCallback callback) {
  return sco_manager_->AcceptConnection(std::move(parameters), std::move(callback));
}

void BrEdrConnection::AttachInspect(inspect::Node& parent, std::string name) {
  inspect_node_ = parent.CreateChild(name);
  inspect_properties_.peer_id =
      inspect_node_.CreateString(kInspectPeerIdPropertyName, peer_id_.ToString());
}

void BrEdrConnection::OnPairingStateStatus(hci_spec::ConnectionHandle handle, hci::Status status) {
  if (bt_is_error(status, DEBUG, "gap-bredr",
                  "PairingState error status, disconnecting (peer id: %s)", bt_str(peer_id_))) {
    if (disconnect_cb_) {
      disconnect_cb_();
    }
    return;
  }

  // Once pairing succeeds for the first time, the transition from Initializing -> Connected can
  // happen.
  peer_init_token_.reset();
}

}  // namespace bt::gap
