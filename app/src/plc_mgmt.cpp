#include "plc_mgmt.hpp"

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t kPlcMgmtGroupId = MGMT_GROUP_ID_PERUSER;
constexpr uint8_t kSnapshotCommandId = 0;
constexpr uint32_t kDefaultWaitMs = 5000;
constexpr uint32_t kMaximumWaitMs = 30000;
constexpr uint8_t kAfterKey[] = {'a', 'f', 't', 'e', 'r'};
constexpr uint8_t kTimeoutMsKey[] = {'t', 'i', 'm', 'e', 'o',
                                     'u', 't', '_', 'm', 's'};

struct SnapshotValue {
  strucpp::LocatedArea area;
  strucpp::LocatedSize size;
  uint16_t byte_index;
  uint8_t bit_index;
  uint64_t raw_value;
};

struct ScanSnapshot {
  uint32_t generation;
  uint32_t total_count;
  uint32_t value_count;
  SnapshotValue values[CONFIG_RETROPLC_MGMT_MAX_LOCATED_VARS];
};

ScanSnapshot snapshots[2]{};
ScanSnapshot response_snapshot{};
atomic_t published_snapshot = ATOMIC_INIT(0);
atomic_t scan_generation = ATOMIC_INIT(0);
K_MUTEX_DEFINE(snapshot_mutex);
K_SEM_DEFINE(scan_completed, 0, 1);

bool generation_is_after(uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

uint64_t read_raw_value(const strucpp::LocatedVar &located) {
  if (located.pointer == nullptr) {
    return 0;
  }

  if (located.size == strucpp::LocatedSize::Bit) {
    return *static_cast<const bool *>(located.pointer) ? 1U : 0U;
  }

  uint64_t value = 0;
  const size_t byte_count = located.byte_size();
  if (byte_count != 0 && byte_count <= sizeof(value)) {
    std::memcpy(&value, located.pointer, byte_count);
  }
  return value;
}

void copy_published_snapshot(ScanSnapshot &destination) {
  k_mutex_lock(&snapshot_mutex, K_FOREVER);
  destination =
      snapshots[static_cast<uint32_t>(atomic_get(&published_snapshot)) & 1U];
  k_mutex_unlock(&snapshot_mutex);
}

bool encode_address(zcbor_state_t *zse, const SnapshotValue &value) {
  char address[20];
  int length;

  if (value.size == strucpp::LocatedSize::Bit) {
    length = std::snprintf(address, sizeof(address), "%cX%u.%u",
                           strucpp::area_to_char(value.area), value.byte_index,
                           value.bit_index);
  } else {
    length = std::snprintf(address, sizeof(address), "%c%c%u",
                           strucpp::area_to_char(value.area),
                           strucpp::size_to_char(value.size), value.byte_index);
  }

  return length > 0 && static_cast<size_t>(length) < sizeof(address) &&
         zcbor_tstr_put_term(zse, address, sizeof(address));
}

bool encode_snapshot_value(zcbor_state_t *zse, const SnapshotValue &value) {
  bool ok = zcbor_map_start_encode(zse, 2) &&
            zcbor_tstr_put_lit(zse, "address") && encode_address(zse, value);

  if (value.size == strucpp::LocatedSize::Bit) {
    ok = ok && zcbor_tstr_put_lit(zse, "value") &&
         zcbor_bool_put(zse, value.raw_value != 0);
  } else {
    /* LocatedSize identifies width, not the signed IEC type. Keep the
     * integer representation explicitly raw so the IDE can interpret it. */
    ok = ok && zcbor_tstr_put_lit(zse, "raw") &&
         zcbor_uint64_put(zse, value.raw_value);
  }

  return ok && zcbor_map_end_encode(zse, 2);
}

int snapshot_read(struct smp_streamer *ctxt) {
  zcbor_state_t *zsd = ctxt->reader->zs;
  zcbor_state_t *zse = ctxt->writer->zs;
  uint32_t requested_after = 0;
  uint32_t timeout_ms = kDefaultWaitMs;
  size_t decoded = 0;
  struct zcbor_map_decode_key_val request_decode[] = {
      ZCBOR_MAP_DECODE_KEY_DECODER(kAfterKey, zcbor_uint32_decode,
                                   &requested_after),
      ZCBOR_MAP_DECODE_KEY_DECODER(kTimeoutMsKey, zcbor_uint32_decode,
                                   &timeout_ms),
  };

  if (zcbor_map_decode_bulk(zsd, request_decode, ARRAY_SIZE(request_decode),
                            &decoded) != 0 ||
      timeout_ms > kMaximumWaitMs) {
    return MGMT_ERR_EINVAL;
  }

  const bool after_was_supplied = zcbor_map_decode_bulk_key_found(
      request_decode, ARRAY_SIZE(request_decode), "after");
  if (!after_was_supplied) {
    requested_after = static_cast<uint32_t>(atomic_get(&scan_generation));
  }

  const int64_t deadline = k_uptime_get() + timeout_ms;
  while (!generation_is_after(
      static_cast<uint32_t>(atomic_get(&scan_generation)), requested_after)) {
    const int64_t remaining_ms = deadline - k_uptime_get();
    if (remaining_ms <= 0 ||
        k_sem_take(&scan_completed, K_MSEC(remaining_ms)) == -EAGAIN) {
      break;
    }
  }

  copy_published_snapshot(response_snapshot);
  const bool timed_out =
      !generation_is_after(response_snapshot.generation, requested_after);
  const bool truncated =
      response_snapshot.value_count < response_snapshot.total_count;

  bool ok = zcbor_tstr_put_lit(zse, "scan") &&
            zcbor_uint32_put(zse, response_snapshot.generation) &&
            zcbor_tstr_put_lit(zse, "timed_out") &&
            zcbor_bool_put(zse, timed_out) &&
            zcbor_tstr_put_lit(zse, "total") &&
            zcbor_uint32_put(zse, response_snapshot.total_count) &&
            zcbor_tstr_put_lit(zse, "count") &&
            zcbor_uint32_put(zse, response_snapshot.value_count) &&
            zcbor_tstr_put_lit(zse, "truncated") &&
            zcbor_bool_put(zse, truncated) && zcbor_tstr_put_lit(zse, "vars") &&
            zcbor_list_start_encode(zse, response_snapshot.value_count);

  for (uint32_t index = 0; ok && index < response_snapshot.value_count;
       ++index) {
    ok = encode_snapshot_value(zse, response_snapshot.values[index]);
  }

  ok = ok && zcbor_list_end_encode(zse, response_snapshot.value_count);
  return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

const struct mgmt_handler plc_mgmt_handlers[] = {
    [kSnapshotCommandId] =
        {
            .mh_read = snapshot_read,
            .mh_write = nullptr,
        },
};

struct mgmt_group plc_mgmt_group = {
    .mg_handlers = plc_mgmt_handlers,
    .mg_handlers_count = ARRAY_SIZE(plc_mgmt_handlers),
    .mg_group_id = kPlcMgmtGroupId,
};

void plc_mgmt_register() { mgmt_register_group(&plc_mgmt_group); }

MCUMGR_HANDLER_DEFINE(plc_mgmt, plc_mgmt_register);

} // namespace

void plc_mgmt_publish_scan(const strucpp::LocatedVar *located_vars,
                           uint32_t located_var_count) {
  const uint32_t current_index =
      static_cast<uint32_t>(atomic_get(&published_snapshot)) & 1U;
  const uint32_t next_index = current_index ^ 1U;
  ScanSnapshot &next = snapshots[next_index];

  next.total_count = located_var_count;
  next.value_count = std::min<uint32_t>(located_var_count,
                                        CONFIG_RETROPLC_MGMT_MAX_LOCATED_VARS);
  next.generation = static_cast<uint32_t>(atomic_get(&scan_generation)) + 1U;

  for (uint32_t index = 0; index < next.value_count; ++index) {
    const strucpp::LocatedVar &located = located_vars[index];
    SnapshotValue &value = next.values[index];
    value.area = located.area;
    value.size = located.size;
    value.byte_index = located.byte_index;
    value.bit_index = located.bit_index;
    value.raw_value = read_raw_value(located);
  }

  /* The handler holds this mutex only while copying the already-built
   * snapshot. No CBOR encoding or USB transmission occurs under the lock. */
  k_mutex_lock(&snapshot_mutex, K_FOREVER);
  atomic_set(&published_snapshot, next_index);
  atomic_set(&scan_generation, next.generation);
  k_mutex_unlock(&snapshot_mutex);

  k_sem_give(&scan_completed);
}
