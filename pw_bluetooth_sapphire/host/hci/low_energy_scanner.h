// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_LOW_ENERGY_SCANNER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_LOW_ENERGY_SCANNER_H_

#include <lib/async/dispatcher.h>

#include <set>

#include <fbl/macros.h>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci_constants.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/local_address_delegate.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/sequential_command_runner.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace bt {
namespace hci {

class Transport;

// Represents a discovered Bluetooth Low Energy peer.
struct LowEnergyScanResult {
  LowEnergyScanResult();
  LowEnergyScanResult(const DeviceAddress& address, bool resolved, bool connectable,
                      bool scan_response, int8_t rssi);

  // The device address of the remote peer.
  DeviceAddress address;

  // True if |address| is a static or random identity address resolved by the
  // controller.
  bool resolved;

  // True if this peer accepts connections. This is the case if this peer
  // sent a connectable advertising PDU. If true, |scan_response| will always be false.
  bool connectable;

  // True if the scan result was generated due to a response to a scan request during an active
  // scan. A scan response always follows a regular advertising report. When |scan_response| is
  // true, |connectable| will always be false. This does not indicate that the peer is not
  // connectable but rather that the advertising event isn't.
  bool scan_response;

  // The received signal strength of the advertisement packet corresponding to
  // this peer.
  int8_t rssi;
};

// LowEnergyScanner manages Low Energy scan procedures that are used
// during general and limited discovery and connection establishment
// procedures. This is an abstract class that provides a common interface
// over 5.0 Extended Advertising and Legacy Advertising features.
//
// Instances of this class are expected to each as a singleton on a
// per-transport basis as multiple instances cannot accurately reflect the state
// of the controller while allowing simultaneous scan operations.
class LowEnergyScanner : public LocalAddressClient {
 public:
  // Value that can be passed to StartScan() to scan indefinitely.
  static constexpr zx::duration kPeriodInfinite = zx::duration::infinite();

  enum class State {
    // No scan is currently being performed.
    kIdle,

    // A previously running scan is being stopped.
    kStopping,

    // A scan is being initiated.
    kInitiating,

    // An active scan is currently being performed.
    kActiveScanning,

    // A passive scan is currently being performed.
    kPassiveScanning,
  };

  // Interface for receiving events related to Low Energy scan.
  class Delegate {
   public:
    virtual ~Delegate() = default;

    // Called when a peer is found due to a connectable, non-connectable, or scannable advertising
    // event. |data| contains the advertising data or the scan reponse data if
    // |result.scan_response| is true.
    virtual void OnPeerFound(const LowEnergyScanResult& result, const ByteBuffer& data);

    // Called when a directed advertising report is received from the peer
    // with the given address.
    virtual void OnDirectedAdvertisement(const LowEnergyScanResult& result);
  };

  LowEnergyScanner(fxl::RefPtr<Transport> hci, async_dispatcher_t* dispatcher);
  virtual ~LowEnergyScanner() = default;

  // Returns the current Scan state.
  State state() const { return state_; }

  bool IsActiveScanning() const { return state() == State::kActiveScanning; }
  bool IsPassiveScanning() const { return state() == State::kPassiveScanning; }
  bool IsScanning() const { return IsActiveScanning() || IsPassiveScanning(); }
  bool IsInitiating() const { return state() == State::kInitiating; }

  // LocalAddressClient override:
  bool AllowsRandomAddressChange() const override {
    return !IsScanning() && hci_cmd_runner()->IsReady();
  }

  // True if no scan procedure is currently enabled.
  bool IsIdle() const { return state() == State::kIdle; }

  // Initiates a scan. This is an asynchronous operation that abides by
  // the following rules:
  //
  //   - This method synchronously returns false if the procedure could not be
  //     started, e.g. because discovery is already in progress, or it is in the
  //     process of being stopped, or the controller does not support discovery,
  //     etc.
  //
  //   - Synchronously returns true if the procedure was initiated but the it is
  //     unknown whether or not the procedure has succeeded.
  //
  //   - |callback| is invoked asynchronously to report the status of the
  //     procedure. In the case of failure, |callback| will be invoked once to
  //     report the end of the procedure. In the case of success, |callback|
  //     will be invoked twice: the first time to report that the procedure has
  //     started, and a second time to report when the procedure ends, either
  //     due to a timeout or cancellation.
  //
  //   - |period| specifies (in milliseconds) the duration of the scan. If the
  //     special value of kPeriodInfinite is passed then scanning will continue
  //     indefinitely and must be explicitly stopped by calling StopScan().
  //     Otherwise, the value must be non-zero.
  //
  // Once started, a scan can be terminated at any time by calling the
  // StopScan() method. Otherwise, an ongoing scan will terminate at the end of
  // the scan period if a finite value for |period| was provided.
  //
  // If an active scan is being performed then scannable advertising reports (ADV_IND and
  // ADV_SCAN_IND) as well as any following scan response events will be reported in separate calls
  // to Delegate::OnPeerFound().
  enum class ScanStatus {
    // Reported when the scan could not be started.
    kFailed,

    // Reported when an active scan was started and is currently in progress.
    kActive,

    // Reported when a passive scan was started and is currently in progress.
    kPassive,

    // Called when the scan was terminated naturally at the end of the scan
    // period.
    kComplete,

    // Called when the scan was terminated due to a call to StopScan().
    kStopped,
  };
  using ScanStatusCallback = fit::function<void(ScanStatus)>;
  virtual bool StartScan(bool active, uint16_t scan_interval, uint16_t scan_window,
                         bool filter_duplicates, LEScanFilterPolicy filter_policy,
                         zx::duration period, ScanStatusCallback callback) = 0;

  // Stops a previously started scan. Returns false if a scan is not in
  // progress. Otherwise, cancels any in progress scan procedure and returns
  // true.
  virtual bool StopScan() = 0;

  // Assigns the delegate for scan events.
  void set_delegate(Delegate* delegate) { delegate_ = delegate; }

 protected:
  async_dispatcher_t* dispatcher() const { return dispatcher_; }
  Transport* transport() const { return transport_.get(); }
  SequentialCommandRunner* hci_cmd_runner() const { return hci_cmd_runner_.get(); }
  Delegate* delegate() const {
    ZX_DEBUG_ASSERT(delegate_);
    return delegate_;
  }

  void set_state(State state) { state_ = state; }

  // Returns true if an active scan was most recently requested. This applies to
  // the on-going scan only if IsScanning() returns true.
  bool active_scan_requested() const { return active_scan_requested_; }
  void set_active_scan_requested(bool value) { active_scan_requested_ = value; }

 private:
  State state_;
  bool active_scan_requested_;

  Delegate* delegate_;  // weak

  // Task runner for all asynchronous tasks.
  async_dispatcher_t* dispatcher_;

  // The HCI transport.
  fxl::RefPtr<Transport> transport_;

  // Command runner for all HCI commands sent out by implementations.
  std::unique_ptr<SequentialCommandRunner> hci_cmd_runner_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyScanner);
};

}  // namespace hci
}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_HCI_LOW_ENERGY_SCANNER_H_
