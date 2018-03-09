// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glog/logging.h>

#include "kudu/gutil/port.h"
#include "kudu/rpc/sasl_common.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"
#include "kudu/util/subprocess.h" // IWYU pragma: keep

namespace kudu {
namespace hms {

class MiniHms {
 public:

  MiniHms() = default;

  ~MiniHms();

  // Configures the notification log TTL. Must be called before Start().
  void SetNotificationLogTtl(MonoDelta ttl) {
    CHECK(hms_process_);
    notification_log_ttl_ = ttl;
  }

  // Configures the mini HMS to use Kerberos.
  void EnableKerberos(std::string krb5_conf,
                      std::string service_principal,
                      std::string keytab_file,
                      rpc::SaslProtection::Type protection);

  // Starts the mini Hive metastore.
  //
  // If the MiniHms has already been started and stopped, it will be restarted
  // using the same listening port.
  Status Start() WARN_UNUSED_RESULT;

  // Stops the mini Hive metastore.
  Status Stop() WARN_UNUSED_RESULT;

  // Pause the Hive metastore process.
  Status Pause() WARN_UNUSED_RESULT;

  // Unpause the Hive metastore process.
  Status Resume() WARN_UNUSED_RESULT;

  // Returns the address of the Hive metastore. Should only be called after the
  // metastore is started.
  HostPort address() const {
    return HostPort("127.0.0.1", port_);
  }

 private:

  // Creates a hive-site.xml for the mini HMS.
  Status CreateHiveSite(const std::string& tmp_dir) const WARN_UNUSED_RESULT;

  // Creates a core-site.xml for the mini HMS.
  Status CreateCoreSite(const std::string& tmp_dir) const WARN_UNUSED_RESULT;

  // Waits for the metastore process to bind to a port.
  Status WaitForHmsPorts() WARN_UNUSED_RESULT;

  std::unique_ptr<Subprocess> hms_process_;
  MonoDelta notification_log_ttl_ = MonoDelta::FromSeconds(86400);
  uint16_t port_ = 0;

  // Kerberos configuration
  std::string krb5_conf_;
  std::string service_principal_;
  std::string keytab_file_;
  rpc::SaslProtection::Type protection_ = rpc::SaslProtection::kAuthentication;
};

} // namespace hms
} // namespace kudu
