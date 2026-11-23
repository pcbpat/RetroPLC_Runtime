#ifdef RETROPLC_HAS_GENERATED_DEBUG
#include RETROPLC_GENERATED_HEADER
#include "strucpp-runtime/debug_dispatch.hpp"

#include <array>
#include <type_traits>

extern strucpp::RETROPLC_CONFIGURATION_CLASS g_config;
#endif

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
constexpr uint8_t kDebugCommandId = 1;
constexpr uint32_t kDefaultWaitMs = 5000;
constexpr uint32_t kMaximumWaitMs = 30000;
constexpr uint8_t kAfterKey[] = "after";
constexpr uint8_t kTimeoutMsKey[] = "timeout_ms";

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

#ifdef RETROPLC_HAS_GENERATED_DEBUG
namespace debug = strucpp::debug;
constexpr uint8_t kDebugArray = 0;
constexpr uint32_t kMaxDebugEntries = CONFIG_RETROPLC_MGMT_MAX_DEBUG_ENTRIES;
constexpr uint32_t kMaxDebugValueSize = CONFIG_RETROPLC_MGMT_MAX_DEBUG_VALUE_SIZE;
constexpr uint32_t kDefaultDebugLimit = std::min(8U, kMaxDebugEntries);

template <debug::TypeTag Tag, typename Storage>
struct DebugScalarMatches : std::false_type {};

template <debug::TypeTag Tag, typename T>
struct DebugScalarMatches<Tag, strucpp::IECVar<T>>
    : std::bool_constant<debug::type_ops[Tag].read == &debug::read_impl<T>> {};

template <debug::TypeTag Tag, typename T>
constexpr bool debug_storage_matches() {
  using Storage = std::remove_const_t<std::remove_reference_t<T>>;
  if constexpr (Tag == debug::TAG_STRING) {
    return std::is_same_v<Storage, strucpp::IECStringVar<254>>;
  } else if constexpr (Tag == debug::TAG_WSTRING) {
    return std::is_same_v<Storage, strucpp::IECWStringVar<254>>;
  } else {
    return DebugScalarMatches<Tag, Storage>::value;
  }
}

namespace debug_storage {
using namespace strucpp; // Generated global variable expressions use this scope.
#include "debug_storage_checks.inc"
} // namespace debug_storage

enum class DebugStatus : uint8_t { Ok, InvalidEntry, Unsupported, TooLarge };

struct DebugValue {
  uint16_t size;
  uint8_t tag;
  DebugStatus status;
  uint8_t data[kMaxDebugValueSize];
};

struct DebugPage {
  uint32_t generation;
  uint32_t offset;
  uint32_t count;
  DebugValue values[kMaxDebugEntries];
};

DebugPage debug_page{};
K_MUTEX_DEFINE(debug_page_mutex);
K_SEM_DEFINE(debug_page_ready, 0, 1);
// Pending: the scan thread may populate the page. Busy: a handler owns
// the request through encoding, including after the page becomes ready.
bool debug_page_pending = false;
bool debug_page_busy = false;

void capture_debug_value(uint16_t index, DebugValue &value) {
  const debug::Entry entry = debug::read_entry(kDebugArray, index);
  value.tag = entry.tag;
  value.size = 0;
  value.status = DebugStatus::InvalidEntry;
  if (!entry.ptr || entry.tag >= debug::TAG__COUNT) {
    return;
  }

  value.status = DebugStatus::Unsupported;
  if (!debug_storage::debug_storage_valid[index]) {
    return;
  }
  const uint16_t size = debug::handle_size(kDebugArray, index);
  if (size == 0) {
    return;
  }
  if (size > sizeof(value.data)) {
    value.status = DebugStatus::TooLarge;
    return;
  }
  if (debug::handle_read(kDebugArray, index, value.data) == size) {
    value.size = size;
    value.status = DebugStatus::Ok;
  }
}

bool encode_debug_value(zcbor_state_t *zse, uint32_t id,
                        const DebugValue &value) {
  const bool ok = zcbor_map_start_encode(zse, 3) &&
                  zcbor_tstr_put_lit(zse, "id") && zcbor_uint32_put(zse, id) &&
                  zcbor_tstr_put_lit(zse, "type") &&
                  zcbor_uint32_put(zse, value.tag);
  if (value.status == DebugStatus::Ok) {
    const zcbor_string data{value.data, value.size};
    return ok && zcbor_tstr_put_lit(zse, "data") &&
           zcbor_bstr_encode(zse, &data) && zcbor_map_end_encode(zse, 3);
  }
  return ok && zcbor_tstr_put_lit(zse, "status") &&
         zcbor_uint32_put(zse, static_cast<uint8_t>(value.status)) &&
         zcbor_map_end_encode(zse, 3);
}

int debug_read(struct smp_streamer *ctxt) {
  constexpr uint8_t offset_key[] = "offset";
  constexpr uint8_t limit_key[] = "limit";
  uint32_t offset = 0;
  uint32_t limit = kDefaultDebugLimit;
  size_t decoded = 0;
  struct zcbor_map_decode_key_val request_decode[] = {
      ZCBOR_MAP_DECODE_KEY_DECODER(offset_key, zcbor_uint32_decode, &offset),
      ZCBOR_MAP_DECODE_KEY_DECODER(limit_key, zcbor_uint32_decode, &limit),
  };
  if (zcbor_map_decode_bulk(ctxt->reader->zs, request_decode,
                            ARRAY_SIZE(request_decode), &decoded) != 0 ||
      limit == 0 || limit > kMaxDebugEntries) {
    return MGMT_ERR_EINVAL;
  }
  if (debug::handle_array_count() == 0) {
    return MGMT_ERR_ENOENT;
  }
  // The count and storage checks are immutable generated metadata.
  const uint32_t total = debug::handle_elem_count(kDebugArray);
  if (total != debug_storage::debug_storage_valid.size()) {
    return MGMT_ERR_ECORRUPT;
  }
  if (offset > total) {
    return MGMT_ERR_EINVAL;
  }

  zcbor_state_t *zse = ctxt->writer->zs;
  // Upper bounds for either CBOR container mode: 64 bytes for the outer
  // map, keys, five uint32s and list framing, 32 for each record's map,
  // keys, ID, tag and byte-string length (or status).
  constexpr size_t envelope_bytes = 64;
  constexpr size_t record_overhead = 32;
  constexpr size_t smp_header_bytes = 8;
  const size_t capacity = std::min<size_t>(
      zse->payload_end - zse->payload,
      CONFIG_MCUMGR_TRANSPORT_UART_MTU > smp_header_bytes
          ? CONFIG_MCUMGR_TRANSPORT_UART_MTU - smp_header_bytes
          : 0);
  if (capacity < envelope_bytes) {
    return MGMT_ERR_EMSGSIZE;
  }
  const uint32_t page_capacity =
      (capacity - envelope_bytes) / (record_overhead + kMaxDebugValueSize);
  const uint32_t count = std::min({limit, total - offset, page_capacity});
  if (count == 0 && offset < total) {
    return MGMT_ERR_EMSGSIZE;
  }

  k_mutex_lock(&debug_page_mutex, K_FOREVER);
  if (debug_page_busy) {
    k_mutex_unlock(&debug_page_mutex);
    return MGMT_ERR_EBUSY;
  }
  k_sem_reset(&debug_page_ready);
  debug_page_busy = true;
  debug_page_pending = true;
  debug_page.offset = offset;
  debug_page.count = count;
  k_mutex_unlock(&debug_page_mutex);

  k_sem_take(&debug_page_ready, K_MSEC(kDefaultWaitMs));
  k_mutex_lock(&debug_page_mutex, K_FOREVER);
  const bool ready = !debug_page_pending;
  debug_page_pending = false;
  k_mutex_unlock(&debug_page_mutex);

  int result = MGMT_ERR_ETIMEOUT;
  if (ready) {
    bool ok = zcbor_tstr_put_lit(zse, "scan") &&
              zcbor_uint32_put(zse, debug_page.generation) &&
              zcbor_tstr_put_lit(zse, "total") && zcbor_uint32_put(zse, total) &&
              zcbor_tstr_put_lit(zse, "offset") && zcbor_uint32_put(zse, offset) &&
              zcbor_tstr_put_lit(zse, "count") && zcbor_uint32_put(zse, count) &&
              zcbor_tstr_put_lit(zse, "next") &&
              zcbor_uint32_put(zse, offset + count) &&
              zcbor_tstr_put_lit(zse, "vars") &&
              zcbor_list_start_encode(zse, count);
    for (uint32_t index = 0; ok && index < count; ++index) {
      ok = encode_debug_value(zse, offset + index, debug_page.values[index]);
    }
    ok = ok && zcbor_list_end_encode(zse, count);
    result = ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
  }

  k_mutex_lock(&debug_page_mutex, K_FOREVER);
  debug_page_busy = false;
  k_mutex_unlock(&debug_page_mutex);
  return result;
}
#else
int debug_read(struct smp_streamer *) { return MGMT_ERR_ENOENT; }
#endif

const struct mgmt_handler plc_mgmt_handlers[] = {
    [kSnapshotCommandId] =
        {
            .mh_read = snapshot_read,
            .mh_write = nullptr,
        },
    [kDebugCommandId] =
        {
            .mh_read = debug_read,
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

void plc_mgmt_publish_debug_snapshot() {
#ifdef RETROPLC_HAS_GENERATED_DEBUG
  // A request racing this boundary can be picked up at the following one.
  if (k_mutex_lock(&debug_page_mutex, K_NO_WAIT) != 0) {
    return;
  }
  if (debug_page_pending) {
    debug_page.generation = static_cast<uint32_t>(atomic_get(&scan_generation));
    for (uint32_t index = 0; index < debug_page.count; ++index) {
      capture_debug_value(debug_page.offset + index, debug_page.values[index]);
    }
    debug_page_pending = false;
    k_sem_give(&debug_page_ready);
  }
  k_mutex_unlock(&debug_page_mutex);
#endif
}
