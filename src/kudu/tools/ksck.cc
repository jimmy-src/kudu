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

#include "kudu/tools/ksck.h"

#include <algorithm>
#include <glog/logging.h>
#include <iostream>
#include <mutex>

#include "kudu/consensus/quorum_util.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/atomic.h"
#include "kudu/util/blocking_queue.h"
#include "kudu/util/locks.h"
#include "kudu/util/monotime.h"
#include "kudu/util/threadpool.h"

namespace kudu {
namespace tools {

using std::cerr;
using std::cout;
using std::endl;
using std::ostream;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using strings::Substitute;

DEFINE_int32(checksum_timeout_sec, 3600,
             "Maximum total seconds to wait for a checksum scan to complete "
             "before timing out.");
DEFINE_int32(checksum_scan_concurrency, 4,
             "Number of concurrent checksum scans to execute per tablet server.");
DEFINE_bool(checksum_snapshot, true, "Should the checksum scanner use a snapshot scan");
DEFINE_uint64(checksum_snapshot_timestamp, ChecksumOptions::kCurrentTimestamp,
              "timestamp to use for snapshot checksum scans, defaults to 0, which "
              "uses the current timestamp of a tablet server involved in the scan");

DEFINE_int32(fetch_replica_info_concurrency, 20,
             "Number of concurrent tablet servers to fetch replica info from.");

// The stream to write output to. If this is NULL, defaults to cerr.
// This is used by tests to capture output.
ostream* g_err_stream = NULL;

// Print an informational message to cerr.
static ostream& Out() {
  return (g_err_stream ? *g_err_stream : cerr);
}
static ostream& Info() {
  return Out() << "INFO: ";
}

// Print a warning message to cerr.
static ostream& Warn() {
  return Out() << "WARNING: ";
}

// Print an error message to cerr.
static ostream& Error() {
  return Out() << "ERROR: ";
}

namespace {
// Return true if 'str' matches any of the patterns in 'patterns', or if
// 'patterns' is empty.
bool MatchesAnyPattern(const vector<string>& patterns, const string& str) {
  // Consider no filter a wildcard.
  if (patterns.empty()) return true;

  for (const auto& p : patterns) {
    if (MatchPattern(str, p)) return true;
  }
  return false;
}
} // anonymous namespace

ChecksumOptions::ChecksumOptions()
    : timeout(MonoDelta::FromSeconds(FLAGS_checksum_timeout_sec)),
      scan_concurrency(FLAGS_checksum_scan_concurrency),
      use_snapshot(FLAGS_checksum_snapshot),
      snapshot_timestamp(FLAGS_checksum_snapshot_timestamp) {
}

ChecksumOptions::ChecksumOptions(MonoDelta timeout, int scan_concurrency,
                                 bool use_snapshot, uint64_t snapshot_timestamp)
    : timeout(std::move(timeout)),
      scan_concurrency(scan_concurrency),
      use_snapshot(use_snapshot),
      snapshot_timestamp(snapshot_timestamp) {}

const uint64_t ChecksumOptions::kCurrentTimestamp = 0;

tablet::TabletStatePB KsckTabletServer::ReplicaState(const std::string& tablet_id) const {
  CHECK_EQ(state_, kFetched);
  if (!ContainsKey(tablet_status_map_, tablet_id)) {
    return tablet::UNKNOWN;
  }
  return tablet_status_map_.at(tablet_id).state();
}

KsckCluster::~KsckCluster() {
}

Status KsckCluster::FetchTableAndTabletInfo() {
  RETURN_NOT_OK(master_->Connect());
  RETURN_NOT_OK(RetrieveTablesList());
  RETURN_NOT_OK(RetrieveTabletServers());
  for (const shared_ptr<KsckTable>& table : tables()) {
    RETURN_NOT_OK(RetrieveTabletsList(table));
  }
  return Status::OK();
}

// Gets the list of tablet servers from the Master.
Status KsckCluster::RetrieveTabletServers() {
  return master_->RetrieveTabletServers(&tablet_servers_);
}

// Gets the list of tables from the Master.
Status KsckCluster::RetrieveTablesList() {
  return master_->RetrieveTablesList(&tables_);
}

Status KsckCluster::RetrieveTabletsList(const shared_ptr<KsckTable>& table) {
  return master_->RetrieveTabletsList(table);
}

Status Ksck::CheckMasterRunning() {
  VLOG(1) << "Connecting to the Master";
  Status s = cluster_->master()->Connect();
  if (s.ok()) {
    Info() << "Connected to the Master" << endl;
  }
  return s;
}

Status Ksck::FetchTableAndTabletInfo() {
  return cluster_->FetchTableAndTabletInfo();
}

Status Ksck::FetchInfoFromTabletServers() {
  VLOG(1) << "Getting the Tablet Servers list";
  int servers_count = cluster_->tablet_servers().size();
  VLOG(1) << Substitute("List of $0 Tablet Servers retrieved", servers_count);

  if (servers_count == 0) {
    return Status::NotFound("No tablet servers found");
  }


  gscoped_ptr<ThreadPool> pool;
  RETURN_NOT_OK(ThreadPoolBuilder("ksck-fetch")
                .set_max_threads(FLAGS_fetch_replica_info_concurrency)
                .Build(&pool));

  AtomicInt<int32_t> bad_servers(0);
  VLOG(1) << "Fetching info from all the Tablet Servers";
  for (const KsckMaster::TSMap::value_type& entry : cluster_->tablet_servers()) {
    CHECK_OK(pool->SubmitFunc([&]() {
          Status s = ConnectToTabletServer(entry.second);
          if (!s.ok()) {
            bad_servers.Increment();
          }
        }));
  }
  pool->Wait();

  if (bad_servers.Load() == 0) {
    Info() << Substitute("Fetched info from all $0 Tablet Servers", servers_count) << endl;
    return Status::OK();
  } else {
    Warn() << Substitute("Fetched info from $0 Tablet Servers, $1 weren't reachable",
                         servers_count - bad_servers.Load(), bad_servers.Load()) << endl;
    return Status::NetworkError("Not all Tablet Servers are reachable");
  }
}

Status Ksck::ConnectToTabletServer(const shared_ptr<KsckTabletServer>& ts) {
  VLOG(1) << "Going to connect to Tablet Server: " << ts->uuid();
  Status s = ts->FetchInfo();
  if (s.ok()) {
    VLOG(1) << "Connected to Tablet Server: " << ts->uuid();
  } else {
    Warn() << Substitute("Unable to connect to Tablet Server $0: $1",
                         ts->ToString(), s.ToString()) << endl;
  }
  return s;
}

Status Ksck::CheckTablesConsistency() {
  int tables_checked = 0;
  int bad_tables_count = 0;
  for (const shared_ptr<KsckTable> &table : cluster_->tables()) {
    if (!MatchesAnyPattern(table_filters_, table->name())) {
      VLOG(1) << "Skipping table " << table->name();
      continue;
    }
    tables_checked++;
    if (!VerifyTable(table)) {
      bad_tables_count++;
    }
  }

  if (tables_checked == 0) {
    Info() << "The cluster doesn't have any matching tables" << endl;
    return Status::OK();
  }

  if (bad_tables_count == 0) {
    Info() << Substitute("The metadata for $0 table(s) is HEALTHY", tables_checked) << endl;
    return Status::OK();
  } else {
    Warn() << Substitute("$0 out of $1 table(s) are not in a healthy state",
                         bad_tables_count, tables_checked) << endl;
    return Status::Corruption(Substitute("$0 table(s) are bad", bad_tables_count));
  }
}

// Class to act as a collector of scan results.
// Provides thread-safe accessors to update and read a hash table of results.
class ChecksumResultReporter : public RefCountedThreadSafe<ChecksumResultReporter> {
 public:
  typedef std::pair<Status, uint64_t> ResultPair;
  typedef std::unordered_map<std::string, ResultPair> ReplicaResultMap;
  typedef std::unordered_map<std::string, ReplicaResultMap> TabletResultMap;

  // Initialize reporter with the number of replicas being queried.
  explicit ChecksumResultReporter(int num_tablet_replicas)
      : expected_count_(num_tablet_replicas),
        responses_(num_tablet_replicas),
        rows_summed_(0),
        disk_bytes_summed_(0) {
  }

  void ReportProgress(int64_t delta_rows, int64_t delta_bytes) {
    rows_summed_.IncrementBy(delta_rows);
    disk_bytes_summed_.IncrementBy(delta_bytes);
  }

  // Write an entry to the result map indicating a response from the remote.
  void ReportResult(const std::string& tablet_id,
                    const std::string& replica_uuid,
                    const Status& status,
                    uint64_t checksum) {
    std::lock_guard<simple_spinlock> guard(lock_);
    unordered_map<string, ResultPair>& replica_results =
        LookupOrInsert(&checksums_, tablet_id, unordered_map<string, ResultPair>());
    InsertOrDie(&replica_results, replica_uuid, ResultPair(status, checksum));
    responses_.CountDown();
  }

  // Blocks until either the number of results plus errors reported equals
  // num_tablet_replicas (from the constructor), or until the timeout expires,
  // whichever comes first.
  // Returns false if the timeout expired before all responses came in.
  // Otherwise, returns true.
  bool WaitFor(const MonoDelta& timeout) const {
    MonoTime start = MonoTime::Now(MonoTime::FINE);

    MonoTime deadline = start;
    deadline.AddDelta(timeout);

    bool done = false;
    while (!done) {
      MonoTime now = MonoTime::Now(MonoTime::FINE);
      int rem_ms = deadline.GetDeltaSince(now).ToMilliseconds();
      if (rem_ms <= 0) return false;

      done = responses_.WaitFor(MonoDelta::FromMilliseconds(std::min(rem_ms, 5000)));
      string status = done ? "finished in " : "running for ";
      int run_time_sec = MonoTime::Now(MonoTime::FINE).GetDeltaSince(start).ToSeconds();
      Info() << "Checksum " << status << run_time_sec << "s: "
             << responses_.count() << "/" << expected_count_ << " replicas remaining ("
             << HumanReadableNumBytes::ToString(disk_bytes_summed_.Load()) << " from disk, "
             << HumanReadableInt::ToString(rows_summed_.Load()) << " rows summed)"
             << endl;
    }
    return true;
  }

  // Returns true iff all replicas have reported in.
  bool AllReported() const { return responses_.count() == 0; }

  // Get reported results.
  TabletResultMap checksums() const {
    std::lock_guard<simple_spinlock> guard(lock_);
    return checksums_;
  }

 private:
  friend class RefCountedThreadSafe<ChecksumResultReporter>;
  ~ChecksumResultReporter() {}

  // Report either a success or error response.
  void HandleResponse(const std::string& tablet_id, const std::string& replica_uuid,
                      const Status& status, uint64_t checksum);

  const int expected_count_;
  CountDownLatch responses_;

  mutable simple_spinlock lock_; // Protects 'checksums_'.
  // checksums_ is an unordered_map of { tablet_id : { replica_uuid : checksum } }.
  TabletResultMap checksums_;

  AtomicInt<int64_t> rows_summed_;
  AtomicInt<int64_t> disk_bytes_summed_;
};

// Queue of tablet replicas for an individual tablet server.
typedef shared_ptr<BlockingQueue<std::pair<Schema, std::string> > > SharedTabletQueue;

// A set of callbacks which records the result of a tablet replica's checksum,
// and then checks if the tablet server has any more tablets to checksum. If so,
// a new async checksum scan is started.
class TabletServerChecksumCallbacks : public ChecksumProgressCallbacks {
 public:
  TabletServerChecksumCallbacks(
    scoped_refptr<ChecksumResultReporter> reporter,
    shared_ptr<KsckTabletServer> tablet_server,
    SharedTabletQueue queue,
    std::string tablet_id,
    ChecksumOptions options) :
      reporter_(std::move(reporter)),
      tablet_server_(std::move(tablet_server)),
      queue_(std::move(queue)),
      options_(std::move(options)),
      tablet_id_(std::move(tablet_id)) {
  }

  void Progress(int64_t rows_summed, int64_t disk_bytes_summed) override {
    reporter_->ReportProgress(rows_summed, disk_bytes_summed);
  }

  void Finished(const Status& status, uint64_t checksum) override {
    reporter_->ReportResult(tablet_id_, tablet_server_->uuid(), status, checksum);

    std::pair<Schema, std::string> table_tablet;
    if (queue_->BlockingGet(&table_tablet)) {
      const Schema& table_schema = table_tablet.first;
      tablet_id_ = table_tablet.second;
      tablet_server_->RunTabletChecksumScanAsync(tablet_id_, table_schema, options_, this);
    } else {
      delete this;
    }
  }

 private:
  const scoped_refptr<ChecksumResultReporter> reporter_;
  const shared_ptr<KsckTabletServer> tablet_server_;
  const SharedTabletQueue queue_;
  const ChecksumOptions options_;

  std::string tablet_id_;
};

Status Ksck::ChecksumData(const ChecksumOptions& opts) {
  // Copy options so that local modifications can be made and passed on.
  ChecksumOptions options = opts;

  typedef unordered_map<shared_ptr<KsckTablet>, shared_ptr<KsckTable>> TabletTableMap;
  TabletTableMap tablet_table_map;

  int num_tablet_replicas = 0;
  for (const shared_ptr<KsckTable>& table : cluster_->tables()) {
    VLOG(1) << "Table: " << table->name();
    if (!MatchesAnyPattern(table_filters_, table->name())) continue;
    for (const shared_ptr<KsckTablet>& tablet : table->tablets()) {
      VLOG(1) << "Tablet: " << tablet->id();
      if (!MatchesAnyPattern(tablet_id_filters_, tablet->id())) continue;
      InsertOrDie(&tablet_table_map, tablet, table);
      num_tablet_replicas += tablet->replicas().size();
    }
  }
  if (num_tablet_replicas == 0) {
    string msg = "No tablet replicas found.";
    if (!table_filters_.empty() || !tablet_id_filters_.empty()) {
      msg += " Filter: ";
      if (!table_filters_.empty()) {
        msg += "table_filters=" + JoinStrings(table_filters_, ",");
      }
      if (!tablet_id_filters_.empty()) {
        msg += "tablet_id_filters=" + JoinStrings(tablet_id_filters_, ",");
      }
    }
    return Status::NotFound(msg);
  }

  // Map of tablet servers to tablet queue.
  typedef unordered_map<shared_ptr<KsckTabletServer>, SharedTabletQueue> TabletServerQueueMap;

  TabletServerQueueMap tablet_server_queues;
  scoped_refptr<ChecksumResultReporter> reporter(new ChecksumResultReporter(num_tablet_replicas));

  // Create a queue of checksum callbacks grouped by the tablet server.
  for (const TabletTableMap::value_type& entry : tablet_table_map) {
    const shared_ptr<KsckTablet>& tablet = entry.first;
    const shared_ptr<KsckTable>& table = entry.second;
    for (const shared_ptr<KsckTabletReplica>& replica : tablet->replicas()) {
      const shared_ptr<KsckTabletServer>& ts =
          FindOrDie(cluster_->tablet_servers(), replica->ts_uuid());

      const SharedTabletQueue& queue =
          LookupOrInsertNewSharedPtr(&tablet_server_queues, ts, num_tablet_replicas);
      CHECK_EQ(QUEUE_SUCCESS, queue->Put(make_pair(table->schema(), tablet->id())));
    }
  }

  if (options.use_snapshot && options.snapshot_timestamp == ChecksumOptions::kCurrentTimestamp) {
    // Set the snapshot timestamp to the current timestamp of the first healthy tablet server
    // we can find.
    for (const auto& ts : tablet_server_queues) {
      if (ts.first->is_healthy()) {
        options.snapshot_timestamp = ts.first->current_timestamp();
        break;
      }
    }
    if (options.snapshot_timestamp == ChecksumOptions::kCurrentTimestamp) {
      return Status::ServiceUnavailable(
          "No tablet servers were available to fetch the current timestamp");
    }
    Info() << "Using snapshot timestamp: " << options.snapshot_timestamp << endl;
  }

  // Kick off checksum scans in parallel. For each tablet server, we start
  // scan_concurrency scans. Each callback then initiates one additional
  // scan when it returns if the queue for that TS is not empty.
  for (const TabletServerQueueMap::value_type& entry : tablet_server_queues) {
    const shared_ptr<KsckTabletServer>& tablet_server = entry.first;
    const SharedTabletQueue& queue = entry.second;
    queue->Shutdown(); // Ensures that BlockingGet() will not block.
    for (int i = 0; i < options.scan_concurrency; i++) {
      std::pair<Schema, std::string> table_tablet;
      if (queue->BlockingGet(&table_tablet)) {
        const Schema& table_schema = table_tablet.first;
        const std::string& tablet_id = table_tablet.second;
        auto* cbs = new TabletServerChecksumCallbacks(
            reporter, tablet_server, queue, tablet_id, options);
        // 'cbs' deletes itself when complete.
        tablet_server->RunTabletChecksumScanAsync(tablet_id, table_schema, options, cbs);
      }
    }
  }

  bool timed_out = false;
  if (!reporter->WaitFor(options.timeout)) {
    timed_out = true;
  }
  ChecksumResultReporter::TabletResultMap checksums = reporter->checksums();

  int num_errors = 0;
  int num_mismatches = 0;
  int num_results = 0;
  for (const shared_ptr<KsckTable>& table : cluster_->tables()) {
    bool printed_table_name = false;
    for (const shared_ptr<KsckTablet>& tablet : table->tablets()) {
      if (ContainsKey(checksums, tablet->id())) {
        if (!printed_table_name) {
          printed_table_name = true;
          cout << "-----------------------" << endl;
          cout << table->name() << endl;
          cout << "-----------------------" << endl;
        }
        bool seen_first_replica = false;
        uint64_t first_checksum = 0;

        for (const ChecksumResultReporter::ReplicaResultMap::value_type& r :
                      FindOrDie(checksums, tablet->id())) {
          const string& replica_uuid = r.first;

          shared_ptr<KsckTabletServer> ts = FindOrDie(cluster_->tablet_servers(), replica_uuid);
          const ChecksumResultReporter::ResultPair& result = r.second;
          const Status& status = result.first;
          uint64_t checksum = result.second;
          string status_str = (status.ok()) ? Substitute("Checksum: $0", checksum)
                                            : Substitute("Error: $0", status.ToString());
          cout << Substitute("T $0 P $1 ($2): $3", tablet->id(), ts->uuid(), ts->address(),
                                                   status_str) << endl;
          if (!status.ok()) {
            num_errors++;
          } else if (!seen_first_replica) {
            seen_first_replica = true;
            first_checksum = checksum;
          } else if (checksum != first_checksum) {
            num_mismatches++;
            Error() << ">> Mismatch found in table " << table->name()
                    << " tablet " << tablet->id() << endl;
          }
          num_results++;
        }
      }
    }
    if (printed_table_name) cout << endl;
  }
  if (num_results != num_tablet_replicas) {
    CHECK(timed_out) << Substitute("Unexpected error: only got $0 out of $1 replica results",
                                   num_results, num_tablet_replicas);
    return Status::TimedOut(Substitute("Checksum scan did not complete within the timeout of $0: "
                                       "Received results for $1 out of $2 expected replicas",
                                       options.timeout.ToString(), num_results,
                                       num_tablet_replicas));
  }
  if (num_mismatches != 0) {
    return Status::Corruption(Substitute("$0 checksum mismatches were detected", num_mismatches));
  }
  if (num_errors != 0) {
    return Status::Aborted(Substitute("$0 errors were detected", num_errors));
  }

  return Status::OK();
}

bool Ksck::VerifyTable(const shared_ptr<KsckTable>& table) {
  bool good_table = true;
  const auto all_tablets = table->tablets();
  vector<shared_ptr<KsckTablet>> tablets;
  std::copy_if(all_tablets.begin(), all_tablets.end(), std::back_inserter(tablets),
                 [&](const shared_ptr<KsckTablet>& t) {
                   return MatchesAnyPattern(tablet_id_filters_, t->id());
                 });

  if (tablets.empty()) {
    Info() << Substitute("Table $0 has 0 matching tablets", table->name()) << endl;
    return true;
  }
  int table_num_replicas = table->num_replicas();
  VLOG(1) << Substitute("Verifying $0 tablets for table $1 configured with num_replicas = $2",
                        tablets.size(), table->name(), table_num_replicas);

  int bad_tablets_count = 0;
  for (const shared_ptr<KsckTablet> &tablet : tablets) {
    if (!VerifyTablet(tablet, table_num_replicas)) {
      bad_tablets_count++;
    }
  }
  if (bad_tablets_count == 0) {
    Info() << Substitute("Table $0 is HEALTHY ($1 tablets checked)",
                         table->name(), tablets.size()) << endl;
  } else {
    Warn() << Substitute("Table $0 has $1 bad tablets", table->name(), bad_tablets_count) << endl;
    good_table = false;
  }
  return good_table;
}

bool Ksck::VerifyTablet(const shared_ptr<KsckTablet>& tablet, int table_num_replicas) {
  string tablet_str = Substitute("Tablet $0 of table '$1'",
                                 tablet->id(), tablet->table()->name());
  vector<shared_ptr<KsckTabletReplica> > replicas = tablet->replicas();
  vector<string> warnings, errors, infos;
  if (check_replica_count_ && replicas.size() != table_num_replicas) {
    warnings.push_back(Substitute("$0 has $1 instead of $2 replicas",
                                  tablet_str, replicas.size(), table_num_replicas));
  }
  int leaders_count = 0;
  int followers_count = 0;
  int alive_count = 0;
  int running_count = 0;
  for (const shared_ptr<KsckTabletReplica> replica : replicas) {
    VLOG(1) << Substitute("A replica of tablet $0 is on live tablet server $1",
                          tablet->id(), replica->ts_uuid());
    // Check for agreement on tablet assignment and state between the master
    // and the tablet server.
    auto ts = FindPtrOrNull(cluster_->tablet_servers(), replica->ts_uuid());
    if (ts && ts->is_healthy()) {
      alive_count++;
      auto state = ts->ReplicaState(tablet->id());
      if (state != tablet::UNKNOWN) {
        VLOG(1) << Substitute("Tablet server $0 agrees that it hosts a replica of $1",
                              ts->ToString(), tablet_str);
      }

      switch (state) {
        case tablet::RUNNING:
          VLOG(1) << Substitute("Tablet replica for $0 on TS $1 is RUNNING",
                                tablet_str, ts->ToString());
          running_count++;
          infos.push_back(Substitute("OK state on TS $0: $1",
                                     ts->ToString(), tablet::TabletStatePB_Name(state)));
          break;

        case tablet::UNKNOWN:
          warnings.push_back(Substitute("Missing a tablet replica on tablet server $0",
                                        ts->ToString()));
          break;

        default: {
          const auto& status_pb = ts->tablet_status_map().at(tablet->id());
          warnings.push_back(
              Substitute("Bad state on TS $0: $1\n"
                         "  Last status: $2\n"
                         "  Data state:  $3",
                         ts->ToString(), tablet::TabletStatePB_Name(state),
                         status_pb.last_status(),
                         tablet::TabletDataState_Name(status_pb.tablet_data_state())));
          break;
        }
      }
    } else {
      // no TS or unhealthy TS
      warnings.push_back(Substitute("Should have a replica on TS $0, but TS is unavailable",
                                    ts ? ts->ToString() : replica->ts_uuid()));
    }
    if (replica->is_leader()) {
      VLOG(1) << Substitute("Replica at $0 is a LEADER", replica->ts_uuid());
      leaders_count++;
    } else if (replica->is_follower()) {
      VLOG(1) << Substitute("Replica at $0 is a FOLLOWER", replica->ts_uuid());
      followers_count++;
    }
  }
  if (leaders_count == 0) {
    errors.push_back("No leader detected");
  }
  VLOG(1) << Substitute("$0 has $1 leader and $2 followers",
                        tablet_str, leaders_count, followers_count);
  int majority_size = consensus::MajoritySize(table_num_replicas);
  if (alive_count < majority_size) {
    errors.push_back(Substitute("$0 does not have a majority of replicas on live tablet servers",
                                tablet_str));
  } else if (running_count < majority_size) {
    errors.push_back(Substitute("$0 does not have a majority of replicas in RUNNING state",
                                tablet_str));
  }

  bool has_issues = !warnings.empty() || !errors.empty();
  if (has_issues) {
    Warn() << "Detected problems with " << tablet_str << endl
           << "------------------------------------------------------------" << endl;
    for (const auto& s : warnings) {
      Warn() << s << endl;
    }
    for (const auto& s : errors) {
      Error() << s << endl;
    }
    // We only print the 'INFO' messages on tablets that have some issues.
    // Otherwise, it's a bit verbose.
    for (const auto& s : infos) {
      Info() << s << endl;
    }
    Out() << endl;
  }

  return !has_issues;
}

} // namespace tools
} // namespace kudu
