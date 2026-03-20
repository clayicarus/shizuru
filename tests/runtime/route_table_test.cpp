// Unit tests for RouteTable

#include <gtest/gtest.h>

#include <string>

#include "runtime/route_table.h"

namespace shizuru::runtime {
namespace {

// Helper to build a PortAddress.
PortAddress PA(const std::string& device_id, const std::string& port_name) {
  return PortAddress{device_id, port_name};
}

// Helper: check if a destination is present in Lookup results.
bool Contains(const std::vector<std::pair<PortAddress, RouteOptions>>& results,
              const PortAddress& dest) {
  for (const auto& [d, _] : results) {
    if (d == dest) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// RouteTableTest
// ---------------------------------------------------------------------------

TEST(RouteTableTest, EmptyTableLookupReturnsEmpty) {
  RouteTable table;
  auto results = table.Lookup(PA("dev_a", "port_out"));
  EXPECT_TRUE(results.empty());
}

TEST(RouteTableTest, SingleRouteAddAndLookup) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst = PA("dev_b", "port_in");

  table.AddRoute(src, dst);

  auto results = table.Lookup(src);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].first, dst);
}

TEST(RouteTableTest, FanOutThreeDestinations) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst1 = PA("dev_b", "port_in");
  auto dst2 = PA("dev_c", "port_in");
  auto dst3 = PA("dev_d", "port_in");

  table.AddRoute(src, dst1);
  table.AddRoute(src, dst2);
  table.AddRoute(src, dst3);

  auto results = table.Lookup(src);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(Contains(results, dst1));
  EXPECT_TRUE(Contains(results, dst2));
  EXPECT_TRUE(Contains(results, dst3));
}

TEST(RouteTableTest, RemoveMiddleRouteFromFanOut) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst1 = PA("dev_b", "port_in");
  auto dst2 = PA("dev_c", "port_in");
  auto dst3 = PA("dev_d", "port_in");

  table.AddRoute(src, dst1);
  table.AddRoute(src, dst2);
  table.AddRoute(src, dst3);

  table.RemoveRoute(src, dst2);

  auto results = table.Lookup(src);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_TRUE(Contains(results, dst1));
  EXPECT_FALSE(Contains(results, dst2));
  EXPECT_TRUE(Contains(results, dst3));
}

TEST(RouteTableTest, IdempotentAddRoute) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst = PA("dev_b", "port_in");

  table.AddRoute(src, dst);
  table.AddRoute(src, dst);  // duplicate — should be a no-op

  auto results = table.Lookup(src);
  EXPECT_EQ(results.size(), 1u);
}

TEST(RouteTableTest, RemoveNonExistentRoute) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst = PA("dev_b", "port_in");

  // Should not crash on empty table.
  EXPECT_NO_THROW(table.RemoveRoute(src, dst));

  // Add one route, then remove a different (non-existent) destination.
  auto dst2 = PA("dev_c", "port_in");
  table.AddRoute(src, dst);
  EXPECT_NO_THROW(table.RemoveRoute(src, dst2));

  // Original route must still be present.
  auto results = table.Lookup(src);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].first, dst);
}

TEST(RouteTableTest, IsEmptyAfterAllRoutesRemoved) {
  RouteTable table;
  auto src = PA("dev_a", "port_out");
  auto dst = PA("dev_b", "port_in");

  EXPECT_TRUE(table.IsEmpty());

  table.AddRoute(src, dst);
  EXPECT_FALSE(table.IsEmpty());

  table.RemoveRoute(src, dst);
  EXPECT_TRUE(table.IsEmpty());
}

TEST(RouteTableTest, AllRoutesReturnsCorrectCount) {
  RouteTable table;
  auto src1 = PA("dev_a", "port_out");
  auto src2 = PA("dev_b", "port_out");
  auto dst1 = PA("dev_c", "port_in");
  auto dst2 = PA("dev_d", "port_in");
  auto dst3 = PA("dev_e", "port_in");

  table.AddRoute(src1, dst1);
  table.AddRoute(src1, dst2);
  table.AddRoute(src2, dst3);

  auto all = table.AllRoutes();
  EXPECT_EQ(all.size(), 3u);
}

}  // namespace
}  // namespace shizuru::runtime
