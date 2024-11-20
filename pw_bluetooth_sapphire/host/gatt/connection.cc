// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "connection.h"

#include <zircon/assert.h>

#include <numeric>

#include "client.h"
#include "server.h"
#include "src/connectivity/bluetooth/core/bt-host/att/database.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"

namespace bt::gatt::internal {

Connection::Connection(PeerId peer_id, fbl::RefPtr<att::Bearer> att_bearer,
                       std::unique_ptr<Client> client, fbl::RefPtr<att::Database> local_db,
                       RemoteServiceWatcher svc_watcher, async_dispatcher_t* gatt_dispatcher)
    : att_(std::move(att_bearer)), weak_ptr_factory_(this) {
  ZX_ASSERT(att_);
  ZX_ASSERT(local_db);
  ZX_ASSERT(svc_watcher);

  server_ = std::make_unique<gatt::Server>(peer_id, std::move(local_db), att_);
  remote_service_manager_ =
      std::make_unique<RemoteServiceManager>(std::move(client), gatt_dispatcher);
  remote_service_manager_->set_service_watcher(std::move(svc_watcher));
}

void Connection::Initialize(std::vector<UUID> service_uuids) {
  ZX_ASSERT(remote_service_manager_);

  auto uuids_count = service_uuids.size();
  // status_cb must not capture att_ in order to prevent reference cycle.
  auto status_cb = [self = weak_ptr_factory_.GetWeakPtr(), uuids_count](att::Status status) {
    if (!self) {
      return;
    }

    if (bt_is_error(status, ERROR, "gatt", "client setup failed")) {
      // Signal a link error.
      self->att_->ShutDown();
    } else if (uuids_count > 0) {
      bt_log(DEBUG, "gatt", "primary service discovery complete for (%zu) service uuids",
             uuids_count);
    } else {
      bt_log(DEBUG, "gatt", "primary service discovery complete");
    }
  };

  remote_service_manager_->Initialize(std::move(status_cb), std::move(service_uuids));
}

}  // namespace bt::gatt::internal
