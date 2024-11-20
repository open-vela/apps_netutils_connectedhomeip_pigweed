// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "src/connectivity/bluetooth/core/bt-host/sm/test_security_manager.h"

#include <zircon/assert.h>

#include <memory>

#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/smp.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace bt {
namespace sm {
namespace testing {

TestSecurityManager::TestSecurityManager(
    fxl::WeakPtr<hci::Connection> link, fbl::RefPtr<l2cap::Channel> smp, IOCapability io_capability,
    fxl::WeakPtr<Delegate> delegate, BondableMode bondable_mode, gap::LeSecurityMode security_mode)
    : SecurityManager(bondable_mode, security_mode),
      role_(link->role() == hci::Connection::Role::kMaster ? Role::kInitiator : Role::kResponder),
      weak_ptr_factory_(this) {}

bool TestSecurityManager::AssignLongTermKey(const LTK& ltk) {
  current_ltk_ = ltk;
  if (role_ == Role::kInitiator) {
    set_security(ltk.security());
  }
  return true;
}

void TestSecurityManager::UpgradeSecurity(SecurityLevel level, PairingCallback callback) {
  last_requested_upgrade_ = level;
  set_security(SecurityProperties(level, kMaxEncryptionKeySize, true));
  callback(Status(), security());
}

void TestSecurityManager::Reset(IOCapability io_capability) {}
void TestSecurityManager::Abort(ErrorCode ecode) {}

std::unique_ptr<SecurityManager> TestSecurityManagerFactory::CreateSm(
    fxl::WeakPtr<hci::Connection> link, fbl::RefPtr<l2cap::Channel> smp, IOCapability io_capability,
    fxl::WeakPtr<Delegate> delegate, BondableMode bondable_mode,
    gap::LeSecurityMode security_mode) {
  hci::ConnectionHandle conn = link->handle();
  auto test_sm = std::unique_ptr<TestSecurityManager>(
      new TestSecurityManager(std::move(link), std::move(smp), io_capability, std::move(delegate),
                              bondable_mode, security_mode));
  test_sms_[conn] = test_sm->GetWeakPtr();
  return test_sm;
}

fxl::WeakPtr<TestSecurityManager> TestSecurityManagerFactory::GetTestSm(
    hci::ConnectionHandle conn_handle) {
  auto iter = test_sms_.find(conn_handle);
  ZX_ASSERT(iter != test_sms_.end());
  return iter->second;
}

}  // namespace testing
}  // namespace sm
}  // namespace bt
