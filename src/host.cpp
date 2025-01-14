/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "host.hpp"

#include "collection_iterator.hpp"
#include "connection.hpp"
#include "row.hpp"
#include "scoped_lock.hpp"
#include "value.hpp"

using namespace datastax;
using namespace datastax::internal::core;

namespace datastax { namespace internal { namespace core {

void add_host(CopyOnWriteHostVec& hosts, const Host::Ptr& host) {
  HostVec::iterator i;
  for (i = hosts->begin(); i != hosts->end(); ++i) {
    if ((*i)->address() == host->address()) {
      *i = host;
      break;
    }
  }
  if (i == hosts->end()) {
    hosts->push_back(host);
  }
}

void remove_host(CopyOnWriteHostVec& hosts, const Host::Ptr& host) {
  remove_host(hosts, host->address());
}

bool remove_host(CopyOnWriteHostVec& hosts, const Address& address) {
  HostVec::iterator i;
  for (i = hosts->begin(); i != hosts->end(); ++i) {
    if ((*i)->address() == address) {
      hosts->erase(i);
      return true;
    }
  }
  return false;
}

}}} // namespace datastax::internal::core

void Host::LatencyTracker::update(uint64_t latency_ns) {
  uint64_t now = uv_hrtime();

  ScopedSpinlock l(SpinlockPool<LatencyTracker>::get_spinlock(this));

  TimestampedAverage previous = current_;

  if (previous.num_measured < threshold_to_account_) {
    current_.average = -1;
  } else if (previous.average < 0) {
    current_.average = latency_ns;
  } else {
    int64_t delay = now - previous.timestamp;
    if (delay <= 0) {
      return;
    }

    double scaled_delay = static_cast<double>(delay) / scale_ns_;
    double weight = log(scaled_delay + 1) / scaled_delay;
    current_.average =
        static_cast<int64_t>((1.0 - weight) * latency_ns + weight * previous.average);
  }

  current_.num_measured = previous.num_measured + 1;
  current_.timestamp = now;
}

bool VersionNumber::parse(const String& version) {
  return sscanf(version.c_str(), "%d.%d.%d", &major_version_, &minor_version_, &patch_version_) >=
         2;
}

Host::Host(const Address& address)
    : address_(address)
    , rpc_address_(address)
    , rack_id_(0)
    , dc_id_(0)
    , address_string_(address.to_string())
    , connection_count_(0)
    , inflight_request_count_(0) {
  uv_mutex_init(&mutex_);
}

Host::~Host() {
    uv_mutex_destroy(&mutex_);
}

void Host::set(const Row* row, bool use_tokens) {
  const Value* v;

  String rack;
  row->get_string_by_name("rack", &rack);

  String dc;
  row->get_string_by_name("data_center", &dc);

  String release_version;
  row->get_string_by_name("release_version", &release_version);

  rack_ = rack;
  dc_ = dc;

  VersionNumber server_version;
  if (server_version.parse(release_version)) {
    server_version_ = server_version;
  } else {
    LOG_WARN("Invalid release version string \"%s\" on host %s", release_version.c_str(),
             address().to_string().c_str());
  }

  // Possibly correct for invalid Cassandra version numbers for specific
  // versions of DSE.
  if (server_version_ >= VersionNumber(4, 0, 0) &&
      row->get_by_name("dse_version") != NULL) { // DSE only
    String dse_version_str;
    row->get_string_by_name("dse_version", &dse_version_str);

    if (dse_server_version_.parse(dse_version_str)) {
      // Versions before DSE 6.7 erroneously return they support Cassandra 4.0.0
      // features even though they don't.
      if (dse_server_version_ < VersionNumber(6, 7, 0)) {
        server_version_ = VersionNumber(3, 11, 0);
      }
    } else {
      LOG_WARN("Invalid DSE version string \"%s\" on host %s", dse_version_str.c_str(),
               address().to_string().c_str());
    }
  }

  row->get_string_by_name("partitioner", &partitioner_);

  if (use_tokens) {
    v = row->get_by_name("tokens");
    if (v != NULL && v->is_collection()) {
      CollectionIterator iterator(v);
      while (iterator.next()) {
        tokens_.push_back(iterator.value()->to_string());
      }
    }
  }

  v = row->get_by_name("rpc_address");
  if (v && !v->is_null()) {
    if (!v->decoder().as_inet(v->size(), address_.port(), &rpc_address_)) {
      LOG_WARN("Invalid address format for `rpc_address`");
    }
    if (Address("0.0.0.0", 0).equals(rpc_address_, false) ||
        Address("::", 0).equals(rpc_address_, false)) {
      LOG_WARN("Found host with 'bind any' for rpc_address; using listen_address (%s) to contact "
               "instead. "
               "If this is incorrect you should configure a specific interface for rpc_address on "
               "the server.",
               address_string_.c_str());
    }
  } else {
    LOG_WARN("No rpc_address for host %s in system.local or system.peers.",
             address_string_.c_str());
  }
}

std::list<ExportedConnection::Ptr> Host::get_unpooled_connections(int shard_id, int how_many) {
  ScopedMutex lock(&mutex_);
  LOG_DEBUG("Requesting %d connection(s) to shard %d on host %s from the marketplace", how_many, shard_id, address_.to_string(true).c_str());
  auto conn_list_to_selected_shard_it = unpooled_connections_per_shard_.find(shard_id);
  if (conn_list_to_selected_shard_it == unpooled_connections_per_shard_.end() || conn_list_to_selected_shard_it->second.empty()) {
    return {};
  }

  auto& list_move_from = conn_list_to_selected_shard_it->second;
  const auto begin_move_from = list_move_from.begin();
  const auto end_move_from = std::next(begin_move_from, std::min(how_many, (int)list_move_from.size()));

  std::list<ExportedConnection::Ptr> ret;
  ret.splice(ret.begin(), list_move_from, begin_move_from, end_move_from);
  return ret;
}

void Host::add_unpooled_connection(Connection::Ptr conn) {
  ScopedMutex lock(&mutex_);
  LOG_DEBUG("Connection marketplace consumes a connection to shard %d on host %s", conn->shard_id(), address_.to_string(true).c_str());
  int32_t shard_id = conn->shard_id();
  ExportedConnection::Ptr exported(new ExportedConnection(std::move(conn)));
  unpooled_connections_per_shard_[shard_id].push_back(std::move(exported));
}

void Host::close_unpooled_connections(uv_loop_t *loop) {
  ScopedMutex lock(&mutex_);
  for (auto& conn_list : unpooled_connections_per_shard_) {
    for (auto& c : conn_list.second) {
      c->import_connection(loop)->close();
    }
    // We don't want to import same connection twice, everything would probably break.
    conn_list.second.clear();
  }
}

ExternalHostListener::ExternalHostListener(const CassHostListenerCallback callback, void* data)
    : callback_(callback)
    , data_(data) {}

void ExternalHostListener::on_host_up(const Host::Ptr& host) {
  CassInet address;
  address.address_length = host->address().to_inet(address.address);
  callback_(CASS_HOST_LISTENER_EVENT_UP, address, data_);
}

void ExternalHostListener::on_host_down(const Host::Ptr& host) {
  CassInet address;
  address.address_length = host->address().to_inet(address.address);
  callback_(CASS_HOST_LISTENER_EVENT_DOWN, address, data_);
}

void ExternalHostListener::on_host_added(const Host::Ptr& host) {
  CassInet address;
  address.address_length = host->address().to_inet(address.address);
  callback_(CASS_HOST_LISTENER_EVENT_ADD, address, data_);
}

void ExternalHostListener::on_host_removed(const Host::Ptr& host) {
  CassInet address;
  address.address_length = host->address().to_inet(address.address);
  callback_(CASS_HOST_LISTENER_EVENT_REMOVE, address, data_);
}
