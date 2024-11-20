// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "socket_factory.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "src/connectivity/bluetooth/core/bt-host/gap/sco_connection.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/l2cap.h"

namespace bt {
namespace socket {
namespace {

// We'll test the template just for L2CAP channels.
using FactoryT = SocketFactory<l2cap::Channel>;

constexpr l2cap::ChannelId kDynamicChannelIdMin = 0x0040;
constexpr l2cap::ChannelId kRemoteChannelId = 0x0050;
constexpr hci::ConnectionHandle kDefaultConnectionHandle = 0x0001;
constexpr hci::ConnectionHandle kAnotherConnectionHandle = 0x0002;

class DATA_SocketFactoryTest : public ::testing::Test {
 public:
  DATA_SocketFactoryTest() : loop_(&kAsyncLoopConfigAttachToCurrentThread) {
    EXPECT_EQ(ASYNC_LOOP_RUNNABLE, loop_.GetState());
    channel_ = fbl::MakeRefCounted<l2cap::testing::FakeChannel>(
        kDynamicChannelIdMin, kRemoteChannelId, kDefaultConnectionHandle,
        hci::Connection::LinkType::kACL);
    EXPECT_TRUE(channel_);
  }

  void TearDown() {
    // Process any pending events, to tickle any use-after-free bugs.
    RunLoopUntilIdle();
  }

 protected:
  fbl::RefPtr<l2cap::testing::FakeChannel> channel() { return channel_; }
  async_dispatcher_t* dispatcher() { return loop_.dispatcher(); }
  void RunLoopUntilIdle() { loop_.RunUntilIdle(); }

 private:
  async::Loop loop_;
  fbl::RefPtr<l2cap::testing::FakeChannel> channel_;
};

TEST_F(DATA_SocketFactoryTest, TemplatesCompile) {
  socket::SocketFactory<l2cap::Channel> l2cap_factory;
  socket::SocketFactory<gap::ScoConnection> sco_factory;
}

TEST_F(DATA_SocketFactoryTest, CanCreateSocket) {
  FactoryT socket_factory;
  EXPECT_TRUE(socket_factory.MakeSocketForChannel(channel()));
}

TEST_F(DATA_SocketFactoryTest, SocketCreationFailsIfChannelIsNullptr) {
  FactoryT socket_factory;
  EXPECT_FALSE(socket_factory.MakeSocketForChannel(nullptr));
}

TEST_F(DATA_SocketFactoryTest, SocketCreationFailsIfChannelAlreadyHasASocket) {
  FactoryT socket_factory;
  zx::socket socket = socket_factory.MakeSocketForChannel(channel());
  ASSERT_TRUE(socket);

  EXPECT_FALSE(socket_factory.MakeSocketForChannel(channel()));
}

TEST_F(DATA_SocketFactoryTest, SocketCreationFailsIfChannelActivationFails) {
  channel()->set_activate_fails(true);
  EXPECT_FALSE(FactoryT().MakeSocketForChannel(channel()));
}

TEST_F(DATA_SocketFactoryTest, CanCreateSocketForNewChannelWithRecycledId) {
  FactoryT socket_factory;
  auto original_channel = fbl::MakeRefCounted<l2cap::testing::FakeChannel>(
      kDynamicChannelIdMin + 1, kRemoteChannelId, kDefaultConnectionHandle,
      hci::Connection::LinkType::kACL);
  zx::socket socket = socket_factory.MakeSocketForChannel(original_channel);
  ASSERT_TRUE(socket);
  original_channel->Close();
  RunLoopUntilIdle();  // Process any events related to channel closure.

  auto new_channel = fbl::MakeRefCounted<l2cap::testing::FakeChannel>(
      kDynamicChannelIdMin + 1, kRemoteChannelId, kDefaultConnectionHandle,
      hci::Connection::LinkType::kACL);
  EXPECT_TRUE(socket_factory.MakeSocketForChannel(new_channel));
}

TEST_F(DATA_SocketFactoryTest, DestructionWithActiveRelayDoesNotCrash) {
  FactoryT socket_factory;
  zx::socket socket = socket_factory.MakeSocketForChannel(channel());
  ASSERT_TRUE(socket);
  // |socket_factory| is destroyed implicitly.
}

TEST_F(DATA_SocketFactoryTest, DestructionAfterDeactivatingRelayDoesNotCrash) {
  FactoryT socket_factory;
  zx::socket socket = socket_factory.MakeSocketForChannel(channel());
  ASSERT_TRUE(socket);
  channel()->Close();
  RunLoopUntilIdle();  // Process any events related to channel closure.
  // |socket_factory| is destroyed implicitly.
}

TEST_F(DATA_SocketFactoryTest, SameChannelIdDifferentHandles) {
  FactoryT socket_factory;
  EXPECT_TRUE(socket_factory.MakeSocketForChannel(channel()));
  auto another_channel = fbl::MakeRefCounted<l2cap::testing::FakeChannel>(
      kDynamicChannelIdMin, kRemoteChannelId, kAnotherConnectionHandle,
      hci::Connection::LinkType::kACL);
  EXPECT_TRUE(socket_factory.MakeSocketForChannel(another_channel));
}

}  // namespace
}  // namespace socket
}  // namespace bt
