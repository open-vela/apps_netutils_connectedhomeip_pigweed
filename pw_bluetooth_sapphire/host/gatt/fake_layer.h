// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_

#include "src/connectivity/bluetooth/core/bt-host/gatt/fake_client.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt/gatt.h"

namespace bt::gatt::testing {

// This is a fake version of the root GATT object that can be injected in unit
// tests.
class FakeLayer final : public GATT {
 public:
  FakeLayer() = default;
  ~FakeLayer() override = default;

  // Create a new peer GATT service. Creates a peer entry if it doesn't already exist.
  // Notifies the remote service watcher if |notify| is true.
  //
  // Returns the fake remote service and a handle to the fake object.
  //
  // NOTE: the remote service watcher can also get triggered by calling DiscoverServices().
  std::pair<fbl::RefPtr<RemoteService>, fxl::WeakPtr<FakeClient>> AddPeerService(
      PeerId peer_id, const ServiceData& info, bool notify = false);

  // Assign a callback to be notified when a service discovery has been requested.
  using DiscoverServicesCallback = fit::function<void(PeerId, std::vector<UUID>)>;
  void SetDiscoverServicesCallback(DiscoverServicesCallback cb);

  // Assign a callback to be notified when the persist service changed CCC callback is set.
  using SetPersistServiceChangedCCCCallbackCallback = fit::function<void()>;
  void SetSetPersistServiceChangedCCCCallbackCallback(
      SetPersistServiceChangedCCCCallbackCallback cb);

  // Assign a callback to be notified when the retrieve service changed CCC callback is set.
  using SetRetrieveServiceChangedCCCCallbackCallback = fit::function<void()>;
  void SetSetRetrieveServiceChangedCCCCallbackCallback(
      SetRetrieveServiceChangedCCCCallbackCallback cb);

  // Directly force the fake layer to call the persist service changed CCC callback, to test the
  // GAP adapter and peer cache.
  void CallPersistServiceChangedCCCCallback(PeerId peer_id, bool notify, bool indicate);

  // Directly force the fake layer to call the retrieve service changed CCC callback, to test the
  // GAP adapter and peer cache.
  std::optional<ServiceChangedCCCPersistedData> CallRetrieveServiceChangedCCCCallback(
      PeerId peer_id);

  // GATT overrides:
  void AddConnection(PeerId peer_id, fbl::RefPtr<l2cap::Channel> att_chan) override;
  void RemoveConnection(PeerId peer_id) override;
  void RegisterService(ServicePtr service, ServiceIdCallback callback, ReadHandler read_handler,
                       WriteHandler write_handler, ClientConfigCallback ccc_callback) override;
  void UnregisterService(IdType service_id) override;
  void SendNotification(IdType service_id, IdType chrc_id, PeerId peer_id,
                        ::std::vector<uint8_t> value, bool indicate) override;
  void SetPersistServiceChangedCCCCallback(PersistServiceChangedCCCCallback callback) override;
  void SetRetrieveServiceChangedCCCCallback(RetrieveServiceChangedCCCCallback callback) override;
  void DiscoverServices(PeerId peer_id, std::vector<UUID> service_uuids) override;
  void RegisterRemoteServiceWatcher(RemoteServiceWatcher callback) override;
  void ListServices(PeerId peer_id, std::vector<UUID> uuids, ServiceListCallback callback) override;
  void FindService(PeerId peer_id, IdType service_id, RemoteServiceCallback callback) override;

 private:
  // Test callbacks
  DiscoverServicesCallback discover_services_cb_;
  SetPersistServiceChangedCCCCallbackCallback set_persist_service_changed_ccc_cb_cb_;
  SetRetrieveServiceChangedCCCCallbackCallback set_retrieve_service_changed_ccc_cb_cb_;

  // Emulated callbacks
  RemoteServiceWatcher remote_service_watcher_;

  PersistServiceChangedCCCCallback persist_service_changed_ccc_cb_;
  RetrieveServiceChangedCCCCallback retrieve_service_changed_ccc_cb_;

  // Emulated GATT peer.
  struct TestPeer {
    TestPeer();
    ~TestPeer();

    FakeClient fake_client;
    std::vector<fbl::RefPtr<RemoteService>> services;

    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(TestPeer);
  };
  std::unordered_map<PeerId, TestPeer> peers_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeLayer);
};

}  // namespace bt::gatt::testing

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_GATT_FAKE_LAYER_H_
