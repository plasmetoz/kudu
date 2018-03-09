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

#include "kudu/tserver/tserver_path_handlers.h"

#include <algorithm>
#include <iosfwd>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/bind.hpp> // IWYU pragma: keep
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/iterator_stats.h"
#include "kudu/common/partition.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/log_anchor_registry.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/consensus/quorum_util.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/server/webui_util.h"
#include "kudu/tablet/metadata.pb.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tablet/transactions/transaction.h"
#include "kudu/tserver/scanners.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/tserver/ts_tablet_manager.h"
#include "kudu/util/easy_json.h"
#include "kudu/util/maintenance_manager.h"
#include "kudu/util/maintenance_manager.pb.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/monotime.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/url-coding.h"
#include "kudu/util/web_callback_registry.h"

using kudu::MaintenanceManagerStatusPB;
using kudu::consensus::ConsensusStatePB;
using kudu::consensus::GetConsensusRole;
using kudu::consensus::RaftPeerPB;
using kudu::consensus::TransactionStatusPB;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using kudu::tablet::Tablet;
using kudu::tablet::TabletReplica;
using kudu::tablet::TabletStatePB;
using kudu::tablet::TabletStatusPB;
using kudu::tablet::Transaction;
using std::endl;
using std::map;
using std::ostringstream;
using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {

class Schema;

namespace tserver {

TabletServerPathHandlers::~TabletServerPathHandlers() {
}

Status TabletServerPathHandlers::Register(Webserver* server) {
  server->RegisterPathHandler(
    "/scans", "Scans",
    boost::bind(&TabletServerPathHandlers::HandleScansPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/tablets", "Tablets",
    boost::bind(&TabletServerPathHandlers::HandleTabletsPage, this, _1, _2),
    true /* styled */, true /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/tablet", "",
    boost::bind(&TabletServerPathHandlers::HandleTabletPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/transactions", "",
    boost::bind(&TabletServerPathHandlers::HandleTransactionsPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/tablet-rowsetlayout-svg", "",
    boost::bind(&TabletServerPathHandlers::HandleTabletSVGPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/tablet-consensus-status", "",
    boost::bind(&TabletServerPathHandlers::HandleConsensusStatusPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/log-anchors", "",
    boost::bind(&TabletServerPathHandlers::HandleLogAnchorsPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/dashboards", "Dashboards",
    boost::bind(&TabletServerPathHandlers::HandleDashboardsPage, this, _1, _2),
    true /* styled */, true /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/maintenance-manager", "",
    boost::bind(&TabletServerPathHandlers::HandleMaintenanceManagerPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);

  return Status::OK();
}

void TabletServerPathHandlers::HandleTransactionsPage(const Webserver::WebRequest& req,
                                                      Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  bool as_text = ContainsKey(req.parsed_args, "raw");

  vector<scoped_refptr<TabletReplica> > replicas;
  tserver_->tablet_manager()->GetTabletReplicas(&replicas);

  string arg = FindWithDefault(req.parsed_args, "include_traces", "false");
  Transaction::TraceType trace_type = ParseLeadingBoolValue(
      arg.c_str(), false) ? Transaction::TRACE_TXNS : Transaction::NO_TRACE_TXNS;

  if (!as_text) {
    *output << "<h1>Transactions</h1>\n";
    *output << "<table class='table table-striped'>\n";
    *output << "   <thead><tr><th>Tablet id</th><th>Op Id</th>"
      "<th>Transaction Type</th><th>"
      "Total time in-flight</th><th>Description</th></tr></thead>\n";
    *output << "<tbody>\n";
  }

  for (const scoped_refptr<TabletReplica>& replica : replicas) {
    vector<TransactionStatusPB> inflight;

    if (replica->tablet() == nullptr) {
      continue;
    }

    replica->GetInFlightTransactions(trace_type, &inflight);
    for (const TransactionStatusPB& inflight_tx : inflight) {
      string total_time_str = Substitute("$0 us.", inflight_tx.running_for_micros());
      string description;
      if (trace_type == Transaction::TRACE_TXNS) {
        description = Substitute("$0, Trace: $1",
                                  inflight_tx.description(), inflight_tx.trace_buffer());
      } else {
        description = inflight_tx.description();
      }

      if (!as_text) {
        *output << Substitute(
          "<tr><th>$0</th><th>$1</th><th>$2</th><th>$3</th><th>$4</th></tr>\n",
          EscapeForHtmlToString(replica->tablet_id()),
          EscapeForHtmlToString(SecureShortDebugString(inflight_tx.op_id())),
          OperationType_Name(inflight_tx.tx_type()),
          total_time_str,
          EscapeForHtmlToString(description));
      } else {
        *output << "Tablet: " << replica->tablet_id() << endl;
        *output << "Op ID: " << SecureShortDebugString(inflight_tx.op_id()) << endl;
        *output << "Type: " << OperationType_Name(inflight_tx.tx_type()) << endl;
        *output << "Running: " << total_time_str;
        *output << description << endl;
        *output << endl;
      }
    }
  }

  if (!as_text) {
    *output << "</tbody></table>\n";
  }
}

namespace {
string TabletLink(const string& id) {
  return Substitute("<a href=\"/tablet?id=$0\">$1</a>",
                    UrlEncodeToString(id),
                    EscapeForHtmlToString(id));
}

bool IsTombstoned(const scoped_refptr<TabletReplica>& replica) {
  return replica->data_state() == tablet::TABLET_DATA_TOMBSTONED;
}

} // anonymous namespace

void TabletServerPathHandlers::HandleTabletsPage(const Webserver::WebRequest& /*req*/,
                                                 Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  vector<scoped_refptr<TabletReplica>> replicas;
  tserver_->tablet_manager()->GetTabletReplicas(&replicas);

  // Sort by (table_name, tablet_id) tuples.
  std::sort(replicas.begin(), replicas.end(),
            [](const scoped_refptr<TabletReplica>& rep_a,
               const scoped_refptr<TabletReplica>& rep_b) {
              return std::make_pair(rep_a->tablet_metadata()->table_name(), rep_a->tablet_id()) <
                     std::make_pair(rep_b->tablet_metadata()->table_name(), rep_b->tablet_id());
            });

  // For assigning ids to table divs;
  int i = 0;
  auto generate_table = [this, &i](const vector<scoped_refptr<TabletReplica>>& replicas,
                                   std::ostream* output) {
    i++;

    *output << "<h4>Summary</h4>\n";
    map<string, int> tablet_statuses;
    for (const scoped_refptr<TabletReplica>& replica : replicas) {
      tablet_statuses[TabletStatePB_Name(replica->state())]++;
    }
    *output << "<table class='table table-striped table-hover'>\n";
    *output << "<thead><tr><th>Status</th><th>Count</th><th>Percentage</th></tr></thead>\n";
    *output << "<tbody>\n";
    for (const auto& entry : tablet_statuses) {
      double percent = replicas.empty() ? 0 : (100.0 * entry.second) / replicas.size();
      *output << Substitute("<tr><td>$0</td><td>$1</td><td>$2</td></tr>\n",
                            entry.first,
                            entry.second,
                            StringPrintf("%.2f", percent));
    }
    *output << "</tbody>\n";
    *output << Substitute("<tfoot><tr><td>Total</td><td>$0</td><td></td></tr></tfoot>\n",
                          replicas.size());
    *output << "</table>\n";

    *output << "<h4>Detail</h4>";
    *output << Substitute("<a href='#detail$0' data-toggle='collapse'>(toggle)</a>\n", i);
    *output << Substitute("<div id='detail$0' class='collapse'>\n", i);
    *output << "<table class='table table-striped table-hover'>\n";
    *output << "<thead><tr><th>Table name</th><th>Tablet ID</th>"
        "<th>Partition</th><th>State</th><th>Write buffer memory usage</th>"
        "<th>On-disk size</th><th>RaftConfig</th><th>Last status</th></tr></thead>\n";
    *output << "<tbody>\n";
    for (const scoped_refptr<TabletReplica>& replica : replicas) {
      TabletStatusPB status;
      replica->GetTabletStatusPB(&status);
      string id = status.tablet_id();
      string table_name = status.table_name();
      string tablet_id_or_link;
      if (replica->tablet() != nullptr) {
        tablet_id_or_link = TabletLink(id);
      } else {
        tablet_id_or_link = EscapeForHtmlToString(id);
      }
      string mem_bytes = "";
      if (replica->tablet() != nullptr) {
        mem_bytes = HumanReadableNumBytes::ToString(
            replica->tablet()->mem_tracker()->consumption());
      }
      string n_bytes = "";
      if (status.has_estimated_on_disk_size()) {
        n_bytes = HumanReadableNumBytes::ToString(status.estimated_on_disk_size());
      }
      string partition = replica->tablet_metadata()
                                ->partition_schema()
                                 .PartitionDebugString(replica->tablet_metadata()->partition(),
                                                       replica->tablet_metadata()->schema());

      // We don't show the config if it's a tombstone because it's misleading.
      string consensus_state_html;
      shared_ptr<consensus::RaftConsensus> consensus = replica->shared_consensus();
      if (!IsTombstoned(replica) && consensus) {
        ConsensusStatePB cstate;
        if (consensus->ConsensusState(&cstate).ok()) {
          consensus_state_html = ConsensusStatePBToHtml(cstate);
        }
      }

      *output << Substitute(
          // Table name, tablet id, partition
          "<tr><td>$0</td><td>$1</td><td>$2</td>"
          // State, on-disk size, consensus configuration, last status
          "<td>$3</td><td>$4</td><td>$5</td><td>$6</td><td>$7</td></tr>\n",
          EscapeForHtmlToString(table_name), // $0
          tablet_id_or_link, // $1
          EscapeForHtmlToString(partition), // $2
          EscapeForHtmlToString(replica->HumanReadableState()), mem_bytes, n_bytes, // $3, $4, $5
          consensus_state_html, // $6
          EscapeForHtmlToString(status.last_status())); // $7
    }
    *output << "<tbody></table>\n</div>\n";
  };

  vector<scoped_refptr<TabletReplica>> live_replicas;
  vector<scoped_refptr<TabletReplica>> tombstoned_replicas;
  for (const scoped_refptr<TabletReplica>& replica : replicas) {
    if (IsTombstoned(replica)) {
      tombstoned_replicas.push_back(replica);
    } else {
      live_replicas.push_back(replica);
    }
  }

  if (!live_replicas.empty()) {
    *output << "<h3>Live Tablets</h3>\n";
    generate_table(live_replicas, output);
  }
  if (!tombstoned_replicas.empty()) {
    *output << "<h3>Tombstoned Tablets</h3>\n";
    *output << "<p><small>Tombstone tablets are necessary for correct operation "
               "of Kudu. These tablets have had all of their data removed from "
               "disk and do not consume significant resources, and must not be "
               "deleted.</small></p>";
    generate_table(tombstoned_replicas, output);
  }
}

namespace {

bool CompareByMemberType(const RaftPeerPB& a, const RaftPeerPB& b) {
  if (!a.has_member_type()) return false;
  if (!b.has_member_type()) return true;
  return a.member_type() < b.member_type();
}

} // anonymous namespace

string TabletServerPathHandlers::ConsensusStatePBToHtml(const ConsensusStatePB& cstate) const {
  ostringstream html;

  html << "<ul>\n";
  std::vector<RaftPeerPB> sorted_peers;
  sorted_peers.assign(cstate.committed_config().peers().begin(),
                      cstate.committed_config().peers().end());
  std::sort(sorted_peers.begin(), sorted_peers.end(), &CompareByMemberType);
  for (const RaftPeerPB& peer : sorted_peers) {
    string peer_addr_or_uuid =
        peer.has_last_known_addr() ? Substitute("$0:$1",
                                                peer.last_known_addr().host(),
                                                peer.last_known_addr().port())
                                   : peer.permanent_uuid();
    peer_addr_or_uuid = EscapeForHtmlToString(peer_addr_or_uuid);
    string role_name = RaftPeerPB::Role_Name(GetConsensusRole(peer.permanent_uuid(), cstate));
    string formatted = Substitute("$0: $1", role_name, peer_addr_or_uuid);
    // Make the local peer bold.
    if (peer.permanent_uuid() == tserver_->instance_pb().permanent_uuid()) {
      formatted = Substitute("<b>$0</b>", formatted);
    }

    html << Substitute(" <li>$0</li>\n", formatted);
  }
  html << "</ul>\n";
  return html.str();
}

namespace {

bool GetTabletID(const Webserver::WebRequest& req,
                 string* id,
                 Webserver::PrerenderedWebResponse* resp) {
  if (!FindCopy(req.parsed_args, "id", id)) {
    resp->status_code = HttpStatusCode::BadRequest;
    *resp->output << "Tablet missing 'id' argument";
    return false;
  }
  return true;
}

bool GetTabletReplica(TabletServer* tserver, const Webserver::WebRequest& /*req*/,
                      scoped_refptr<TabletReplica>* replica, const string& tablet_id,
                      Webserver::PrerenderedWebResponse* resp) {
  if (!tserver->tablet_manager()->LookupTablet(tablet_id, replica)) {
    resp->status_code = HttpStatusCode::NotFound;
    *resp->output << "Tablet " << EscapeForHtmlToString(tablet_id) << " not found";
    return false;
  }
  return true;
}

bool TabletBootstrapping(const scoped_refptr<TabletReplica>& replica, const string& tablet_id,
                         Webserver::PrerenderedWebResponse* resp) {
  if (replica->state() == tablet::BOOTSTRAPPING) {
    resp->status_code = HttpStatusCode::ServiceUnavailable;
    *resp->output << "Tablet " << EscapeForHtmlToString(tablet_id) << " is still bootstrapping";
    return true;
  }
  return false;
}

// Returns true if the tablet_id was properly specified, the
// tablet is found, and is in a non-bootstrapping state.
bool LoadTablet(TabletServer* tserver,
                const Webserver::WebRequest& req,
                string* tablet_id, scoped_refptr<TabletReplica>* replica,
                Webserver::PrerenderedWebResponse* resp) {
  return GetTabletID(req, tablet_id, resp) &&
      GetTabletReplica(tserver, req, replica, *tablet_id, resp) &&
      !TabletBootstrapping(*replica, *tablet_id, resp);
}

} // anonymous namespace

void TabletServerPathHandlers::HandleTabletPage(const Webserver::WebRequest& req,
                                                Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;

  string table_name = replica->tablet_metadata()->table_name();
  RaftPeerPB::Role role = RaftPeerPB::UNKNOWN_ROLE;
  auto consensus = replica->consensus();
  if (consensus) {
    role = consensus->role();
  }

  *output << "<h1>Tablet " << EscapeForHtmlToString(tablet_id)
          << " (" << replica->HumanReadableState()
          << "/" << RaftPeerPB::Role_Name(role) << ")</h1>\n";
  *output << "<h3>Table " << EscapeForHtmlToString(table_name) << "</h3>";

  // Output schema in tabular format.
  *output << "<h2>Schema</h2>\n";
  const Schema& schema = replica->tablet_metadata()->schema();
  HtmlOutputSchemaTable(schema, output);

  *output << "<h2>Other Tablet Info Pages</h2>" << endl;

  // List of links to various tablet-specific info pages
  *output << "<ul>";

  // Link to output svg of current DiskRowSet layout over keyspace.
  *output << "<li>" << Substitute("<a href=\"/tablet-rowsetlayout-svg?id=$0\">$1</a>",
                                  UrlEncodeToString(tablet_id),
                                  "Rowset Layout Diagram")
          << "</li>" << endl;

  // Link to consensus status page.
  *output << "<li>" << Substitute("<a href=\"/tablet-consensus-status?id=$0\">$1</a>",
                                  UrlEncodeToString(tablet_id),
                                  "Consensus Status")
          << "</li>" << endl;

  // Log anchors info page.
  *output << "<li>" << Substitute("<a href=\"/log-anchors?id=$0\">$1</a>",
                                  UrlEncodeToString(tablet_id),
                                  "Tablet Log Anchors")
          << "</li>" << endl;

  // End list
  *output << "</ul>\n";
}

void TabletServerPathHandlers::HandleTabletSVGPage(const Webserver::WebRequest& req,
                                                   Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  string id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &id, &replica, resp)) return;
  shared_ptr<Tablet> tablet = replica->shared_tablet();
  if (!tablet) {
    *output << "Tablet " << EscapeForHtmlToString(id) << " not running";
    return;
  }

  *output << "<h1>Rowset Layout Diagram for Tablet "
          << TabletLink(id) << "</h1>\n";
  tablet->PrintRSLayout(output);

}

void TabletServerPathHandlers::HandleLogAnchorsPage(const Webserver::WebRequest& req,
                                                    Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;

  *output << "<h1>Log Anchors for Tablet " << EscapeForHtmlToString(tablet_id) << "</h1>"
          << std::endl;

  string dump = replica->log_anchor_registry()->DumpAnchorInfo();
  *output << "<pre>" << EscapeForHtmlToString(dump) << "</pre>" << std::endl;
}

void TabletServerPathHandlers::HandleConsensusStatusPage(const Webserver::WebRequest& req,
                                                         Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  string id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &id, &replica, resp)) return;
  shared_ptr<consensus::RaftConsensus> consensus = replica->shared_consensus();
  if (!consensus) {
    *output << "Tablet " << EscapeForHtmlToString(id) << " not initialized";
    return;
  }
  consensus->DumpStatusHtml(*output);
}

namespace {
// Pretty-prints a scan's state.
const char* ScanStateToString(const ScanState& scan_state) {
  switch (scan_state) {
    case ScanState::kActive: return "Active";
    case ScanState::kComplete: return "Complete";
    case ScanState::kFailed: return "Failed";
    case ScanState::kExpired: return "Expired";
  }
  LOG(FATAL) << "missing ScanState branch";
}

// Formats the scan descriptor's pseudo-SQL query string as HTML.
string ScanQueryHtml(const ScanDescriptor& scan) {
  string query = "<b>SELECT</b> ";
  if (scan.projected_columns.empty()) {
    query.append("COUNT(*)");
  } else {
    query.append(JoinMapped(scan.projected_columns, EscapeForHtmlToString, ",<br>       "));
  }
  query.append("<br>  <b>FROM</b> ");
  if (scan.table_name.empty()) {
    query.append("&lt;unknown&gt;");
  } else {
    query.append(EscapeForHtmlToString(scan.table_name));
  }

  if (!scan.predicates.empty()) {
    query.append("<br> <b>WHERE</b> ");
    query.append(JoinMapped(scan.predicates, EscapeForHtmlToString, "<br>   <b>AND</b> "));
  }

  return query;
}

void IteratorStatsToJson(const ScanDescriptor& scan, EasyJson* json) {

  auto fill_stats = [] (EasyJson& row, const string& column, const IteratorStats& stats) {
    row["column"] = column;

    row["bytes_read"] = HumanReadableNumBytes::ToString(stats.bytes_read);
    row["cells_read"] = HumanReadableInt::ToString(stats.cells_read);
    row["blocks_read"] = HumanReadableInt::ToString(stats.blocks_read);

    row["bytes_read_title"] = stats.bytes_read;
    row["cells_read_title"] = stats.cells_read;
    row["blocks_read_title"] = stats.blocks_read;
  };

  IteratorStats total_stats;
  for (const auto& column : scan.iterator_stats) {
    EasyJson row = json->PushBack(EasyJson::kObject);
    fill_stats(row, column.first, column.second);
    total_stats += column.second;
  }

  EasyJson total_row = json->PushBack(EasyJson::kObject);
  fill_stats(total_row, "total", total_stats);
}

void ScanToJson(const ScanDescriptor& scan, EasyJson* json) {
  MonoTime now = MonoTime::Now();
  MonoDelta duration;
  if (scan.state == ScanState::kActive) {
    duration = now - scan.start_time;
  } else {
    duration = scan.last_access_time - scan.start_time;
  }
  MonoDelta time_since_start = now - scan.start_time;

  json->Set("tablet_id", scan.tablet_id);
  json->Set("scanner_id", scan.scanner_id);
  json->Set("state", ScanStateToString(scan.state));
  json->Set("query", ScanQueryHtml(scan));
  json->Set("requestor", scan.requestor);

  json->Set("duration", HumanReadableElapsedTime::ToShortString(duration.ToSeconds()));
  json->Set("time_since_start",
            HumanReadableElapsedTime::ToShortString(time_since_start.ToSeconds()));

  json->Set("duration_title", duration.ToSeconds());
  json->Set("time_since_start_title", time_since_start.ToSeconds());

  EasyJson stats_json = json->Set("stats", EasyJson::kArray);
  IteratorStatsToJson(scan, &stats_json);
}
} // anonymous namespace

void TabletServerPathHandlers::HandleScansPage(const Webserver::WebRequest& /*req*/,
                                               Webserver::WebResponse* resp) {
  EasyJson scans = resp->output->Set("scans", EasyJson::kArray);
  vector<ScanDescriptor> descriptors = tserver_->scanner_manager()->ListScans();

  for (const auto& descriptor : descriptors) {
    EasyJson scan = scans.PushBack(EasyJson::kObject);
    ScanToJson(descriptor, &scan);
  }
}

void TabletServerPathHandlers::HandleDashboardsPage(const Webserver::WebRequest& /*req*/,
                                                    Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  *output << "<h3>Dashboards</h3>\n";
  *output << "<table class='table table-striped'>\n";
  *output << "  <thead><tr><th>Dashboard</th><th>Description</th></tr></thead>\n";
  *output << "  <tbody\n";
  *output << GetDashboardLine("scans", "Scans", "List of currently running and recently "
                                                "completed scans.");
  *output << GetDashboardLine("transactions", "Transactions", "List of transactions that are "
                                                              "currently running.");
  *output << GetDashboardLine("maintenance-manager", "Maintenance Manager",
                              "List of operations that are currently running and those "
                              "that are registered.");
  *output << "</tbody></table>\n";
}

string TabletServerPathHandlers::GetDashboardLine(const std::string& link,
                                                  const std::string& text,
                                                  const std::string& desc) {
  return Substitute("  <tr><td><a href=\"$0\">$1</a></td><td>$2</td></tr>\n",
                    EscapeForHtmlToString(link),
                    EscapeForHtmlToString(text),
                    EscapeForHtmlToString(desc));
}

void TabletServerPathHandlers::HandleMaintenanceManagerPage(const Webserver::WebRequest& req,
                                                            Webserver::WebResponse* resp) {
  EasyJson* output = resp->output;
  MaintenanceManager* manager = tserver_->maintenance_manager();
  MaintenanceManagerStatusPB pb;
  manager->GetMaintenanceManagerStatusDump(&pb);
  if (ContainsKey(req.parsed_args, "raw")) {
    (*output)["raw"] = SecureDebugString(pb);
    return;
  }

  EasyJson running_ops = output->Set("running_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.registered_operations()) {
    if (op_pb.running() > 0) {
      EasyJson running_op = running_ops.PushBack(EasyJson::kObject);
      running_op["name"] = op_pb.name();
      running_op["instances_running"] = op_pb.running();
    }
  }

  EasyJson completed_ops = output->Set("completed_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.completed_operations()) {
    EasyJson completed_op = completed_ops.PushBack(EasyJson::kObject);
    completed_op["name"] = op_pb.name();
    completed_op["duration"] =
      HumanReadableElapsedTime::ToShortString(op_pb.duration_millis() / 1000.0);
    completed_op["time_since_start"] =
      HumanReadableElapsedTime::ToShortString(op_pb.millis_since_start() / 1000.0);
  }

  EasyJson registered_ops = output->Set("registered_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.registered_operations()) {
    EasyJson registered_op = registered_ops.PushBack(EasyJson::kObject);
    registered_op["name"] = op_pb.name();
    registered_op["runnable"] = op_pb.runnable();
    registered_op["ram_anchored"] = HumanReadableNumBytes::ToString(op_pb.ram_anchored_bytes());
    registered_op["logs_retained"] = HumanReadableNumBytes::ToString(op_pb.logs_retained_bytes());
    registered_op["perf"] = op_pb.perf_improvement();
  }
}

} // namespace tserver
} // namespace kudu
