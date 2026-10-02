/**
 * @file test_zone.cpp
 * @brief Unit tests for zone::feet_in_zone(), the pure feet-in-zone predicate
 * behind the per-track in_zone MQTT field (see orchestration/zone.h; the
 * publisher consumes it in output/mqtt_publisher.cpp).
 */

#include <gtest/gtest.h>
#include "orchestration/zone.h"

using zone::feet_in_zone;

// Frame 1000x1000 so pixel/normalized conversions are exact.
// Zone [0.1, 0.1, 0.8, 0.9].

TEST(FeetInZone, FeetAtCenterAreInside) {
    // feet = ((400+600)/2, 500) = (500,500) -> (0.5,0.5)
    EXPECT_TRUE(feet_in_zone(400, 600, 500, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(FeetInZone, EdgesAreInclusive) {
    // feet x exactly on zone x0 (0.1): cx=100 -> fx=0.1
    EXPECT_TRUE(feet_in_zone(0, 200, 500, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
    // feet y exactly on zone y1 (0.9): by1=900 -> fy=0.9
    EXPECT_TRUE(feet_in_zone(400, 600, 900, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
    // feet x exactly on zone x1 (0.8): cx = (600+1000)/2 = 800 -> fx = 0.8
    EXPECT_TRUE(feet_in_zone(600, 1000, 500, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
    // feet y exactly on zone y0 (0.1): by1 = 100 -> fy = 0.1
    EXPECT_TRUE(feet_in_zone(400, 600, 100, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(FeetInZone, JustOutsideLeftIsOutside) {
    // cx=99 -> fx=0.099 < 0.1
    EXPECT_FALSE(feet_in_zone(0, 198, 500, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(FeetInZone, CenterInsideButFeetOutsideIsOutside) {
    // Tall box: center y = (100+950)/2 = 525 -> 0.525 inside; feet y1=950 -> 0.95 > 0.9
    EXPECT_FALSE(feet_in_zone(400, 600, 950, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(FeetInZone, FeetAtBottomEdgeInsideFullHeightZone) {
    // by1 == frame_h -> fy = 1.0; zone reaches 1.0
    EXPECT_TRUE(feet_in_zone(400, 600, 1000, 1000, 1000, 0.0f, 0.0f, 1.0f, 1.0f));
    // but not when the zone stops short
    EXPECT_FALSE(feet_in_zone(400, 600, 1000, 1000, 1000, 0.0f, 0.0f, 1.0f, 0.9f));
}

TEST(FeetInZone, NonPositiveFrameIsOutside) {
    EXPECT_FALSE(feet_in_zone(400, 600, 500, 0, 1000, 0.0f, 0.0f, 1.0f, 1.0f));
    EXPECT_FALSE(feet_in_zone(400, 600, 500, 1000, 0, 0.0f, 0.0f, 1.0f, 1.0f));
}
