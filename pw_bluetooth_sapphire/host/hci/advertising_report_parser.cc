// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "advertising_report_parser.h"

#include <zircon/assert.h>

#include "src/connectivity/bluetooth/core/bt-host/transport/control_packets.h"

namespace bt::hci {

AdvertisingReportParser::AdvertisingReportParser(const EventPacket& event)
    : encountered_error_(false) {
  ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kLEMetaEventCode);
  const auto& params = event.params<hci_spec::LEMetaEventParams>();
  ZX_DEBUG_ASSERT(params.subevent_code == hci_spec::kLEAdvertisingReportSubeventCode);

  auto subevent_params = event.le_event_params<hci_spec::LEAdvertisingReportSubeventParams>();

  remaining_reports_ = subevent_params->num_reports;
  remaining_bytes_ = event.view().payload_size() - sizeof(hci_spec::LEMetaEventParams) -
                     sizeof(hci_spec::LEAdvertisingReportSubeventParams);
  ptr_ = subevent_params->reports;
}

bool AdvertisingReportParser::GetNextReport(const hci_spec::LEAdvertisingReportData** out_data,
                                            int8_t* out_rssi) {
  ZX_DEBUG_ASSERT(out_data);
  ZX_DEBUG_ASSERT(out_rssi);

  if (encountered_error_ || !HasMoreReports())
    return false;

  const hci_spec::LEAdvertisingReportData* data =
      reinterpret_cast<const hci_spec::LEAdvertisingReportData*>(ptr_);

  // Each report contains the all the report data, followed by the advertising
  // payload, followed by a single octet for the RSSI.
  size_t report_size = sizeof(*data) + data->length_data + 1;
  if (report_size > remaining_bytes_) {
    // Report exceeds the bounds of the packet.
    encountered_error_ = true;
    return false;
  }

  remaining_bytes_ -= report_size;
  remaining_reports_--;
  ptr_ += report_size;

  *out_data = data;
  *out_rssi = *(ptr_ - 1);

  return true;
}

bool AdvertisingReportParser::HasMoreReports() {
  if (encountered_error_)
    return false;

  if (!!remaining_reports_ != !!remaining_bytes_) {
    // There should be no bytes remaining if there are no reports left to parse.
    encountered_error_ = true;
    return false;
  }
  return !!remaining_reports_;
}

}  // namespace bt::hci
