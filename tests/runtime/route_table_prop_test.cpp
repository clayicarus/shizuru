// Property-based tests for RouteTable
// Uses RapidCheck + Google Test

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <vector>

#include "runtime/route_table.h"

namespace shizuru::runtime {
namespace {

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<std::string> genNonEmptyString() {
  return rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
}

rc::Gen<PortAddress> genPortAddress() {
  return rc::gen::apply(
      [](std::string device_id, std::string port_name) {
        return PortAddress{std::move(device_id), std::move(port_name)};
      },
      genNonEmptyString(), genNonEmptyString());
}

// Generate N distinct PortAddresses (distinct by device_id+port_name).
std::vector<PortAddress> genDistinctPortAddresses(int n) {
  std::vector<PortAddress> result;
  result.reserve(n);
  int counter = 0;
  while (static_cast<int>(result.size()) < n) {
    PortAddress addr{"device_" + std::to_string(counter),
                     "port_" + std::to_string(counter)};
    result.push_back(addr);
    ++counter;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Property 3: Route Table Add/Remove Round Trip
// Feature: runtime-io-redesign, Property 3: Route Table Add/Remove Round Trip
// ---------------------------------------------------------------------------
// **Validates: Requirements 3.2, 3.3**
RC_GTEST_PROP(RouteTablePropTest, prop_add_remove_round_trip, ()) {
  auto source = *genPortAddress();
  auto dest = *genPortAddress();

  RouteTable table;

  // After AddRoute: Lookup must include dest.
  table.AddRoute(source, dest);
  auto results = table.Lookup(source);

  bool found = false;
  for (const auto& [d, _] : results) {
    if (d == dest) {
      found = true;
      break;
    }
  }
  RC_ASSERT(found);

  // After RemoveRoute: Lookup must NOT include dest.
  table.RemoveRoute(source, dest);
  auto after_remove = table.Lookup(source);

  bool still_present = false;
  for (const auto& [d, _] : after_remove) {
    if (d == dest) {
      still_present = true;
      break;
    }
  }
  RC_ASSERT(!still_present);
}

// ---------------------------------------------------------------------------
// Property 4: Fan-Out Delivery
// Feature: runtime-io-redesign, Property 4: Fan-Out Delivery
// ---------------------------------------------------------------------------
// **Validates: Requirements 3.4**
RC_GTEST_PROP(RouteTablePropTest, prop_fan_out_delivery, ()) {
  auto source = *genPortAddress();
  int n = *rc::gen::inRange(1, 6);  // 1..5 destinations

  auto destinations = genDistinctPortAddresses(n);

  RouteTable table;
  for (const auto& dest : destinations) {
    table.AddRoute(source, dest);
  }

  auto results = table.Lookup(source);

  // Must return exactly N destinations.
  RC_ASSERT(static_cast<int>(results.size()) == n);

  // All destinations must be present.
  for (const auto& expected_dest : destinations) {
    bool found = false;
    for (const auto& [d, _] : results) {
      if (d == expected_dest) {
        found = true;
        break;
      }
    }
    RC_ASSERT(found);
  }
}

// ---------------------------------------------------------------------------
// Property 5: Control-Plane Gating vs DMA Bypass
// Feature: runtime-io-redesign, Property 5: Control-Plane Gating vs DMA Bypass
// ---------------------------------------------------------------------------
// **Validates: Requirements 3.7, 3.8, 4.1**
RC_GTEST_PROP(RouteTablePropTest, prop_control_plane_flag_preserved, ()) {
  auto source = *genPortAddress();
  int n = *rc::gen::inRange(1, 6);  // 1..5 routes

  // Generate N destinations with random requires_control_plane flags.
  std::vector<std::pair<PortAddress, bool>> routes;
  routes.reserve(n);
  for (int i = 0; i < n; ++i) {
    PortAddress dest{"dest_device_" + std::to_string(i),
                     "dest_port_" + std::to_string(i)};
    bool requires_cp = *rc::gen::arbitrary<bool>();
    routes.push_back({dest, requires_cp});
  }

  RouteTable table;
  for (const auto& [dest, requires_cp] : routes) {
    RouteOptions opts;
    opts.requires_control_plane = requires_cp;
    table.AddRoute(source, dest, opts);
  }

  auto results = table.Lookup(source);
  RC_ASSERT(static_cast<int>(results.size()) == n);

  // Each destination must carry the correct flag value.
  for (const auto& [expected_dest, expected_flag] : routes) {
    bool found = false;
    for (const auto& [d, opts] : results) {
      if (d == expected_dest) {
        RC_ASSERT(opts.requires_control_plane == expected_flag);
        found = true;
        break;
      }
    }
    RC_ASSERT(found);
  }

  // DMA routes (requires_control_plane=false) are always present in lookup.
  for (const auto& [expected_dest, expected_flag] : routes) {
    if (!expected_flag) {
      bool found = false;
      for (const auto& [d, opts] : results) {
        if (d == expected_dest && !opts.requires_control_plane) {
          found = true;
          break;
        }
      }
      RC_ASSERT(found);
    }
  }
}

}  // namespace
}  // namespace shizuru::runtime
