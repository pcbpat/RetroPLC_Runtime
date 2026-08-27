#pragma once

#include "strucpp-runtime/iec_located.hpp"

#include <cstdint>

/** Capture and publish a coherent end-of-scan snapshot for MCUmgr clients. */
void plc_mgmt_publish_scan(const strucpp::LocatedVar *located_vars,
                           uint32_t located_var_count);
