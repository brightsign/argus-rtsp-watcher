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

using zone::any_in_zone;
using zone::box_in_zone;

// Frame 1000x1000, zone [0.1, 0.1, 0.8, 0.9] as above.

TEST(AnyInZone, BoxFullyInsideOverlaps) {
    EXPECT_TRUE(any_in_zone(200, 200, 700, 800, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(AnyInZone, BoxFullyOutsideDoesNotOverlap) {
    // Box entirely left of the zone (x1=90 -> 0.09 < 0.1).
    EXPECT_FALSE(any_in_zone(0, 200, 90, 800, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
    // Box entirely below the zone (y0=950 -> 0.95 > 0.9).
    EXPECT_FALSE(any_in_zone(200, 950, 700, 990, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(AnyInZone, PartialOverlapCounts) {
    // Box straddles the top-left zone corner; only a corner is inside.
    EXPECT_TRUE(any_in_zone(0, 0, 150, 150, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(AnyInZone, EdgeTouchIsInclusive) {
    // Box's right edge exactly on the zone's left edge (x1=100 -> 0.1 == zx0).
    EXPECT_TRUE(any_in_zone(0, 200, 100, 800, 1000, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(AnyInZone, NonPositiveFrameIsOutside) {
    EXPECT_FALSE(any_in_zone(200, 200, 700, 800, 0, 1000, 0.1f, 0.1f, 0.8f, 0.9f));
    EXPECT_FALSE(any_in_zone(200, 200, 700, 800, 1000, 0, 0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(ParseHexColor, ValidWithAndWithoutHash) {
    int r = 0, g = 0, b = 0;
    EXPECT_TRUE(zone::parse_hex_color("#ff8800", r, g, b));
    EXPECT_EQ(r, 255); EXPECT_EQ(g, 136); EXPECT_EQ(b, 0);
    EXPECT_TRUE(zone::parse_hex_color("00FF00", r, g, b));
    EXPECT_EQ(r, 0); EXPECT_EQ(g, 255); EXPECT_EQ(b, 0);
}

TEST(ParseHexColor, MalformedLeavesOutputsUnchanged) {
    int r = 1, g = 2, b = 3;
    EXPECT_FALSE(zone::parse_hex_color("#ff88", r, g, b));   // too short
    EXPECT_FALSE(zone::parse_hex_color("#gg0000", r, g, b)); // non-hex digit
    EXPECT_FALSE(zone::parse_hex_color("", r, g, b));
    EXPECT_EQ(r, 1); EXPECT_EQ(g, 2); EXPECT_EQ(b, 3);
}

TEST(BoxInZone, ModeSelectsPredicate) {
    // Tall box whose feet are below the zone but whose body overlaps it:
    // Base -> outside (feet y1=950 -> 0.95 > 0.9), Any -> inside (body overlaps).
    EXPECT_FALSE(box_in_zone(zone::Mode::Base, 400, 100, 600, 950, 1000, 1000,
                             0.1f, 0.1f, 0.8f, 0.9f));
    EXPECT_TRUE(box_in_zone(zone::Mode::Any, 400, 100, 600, 950, 1000, 1000,
                            0.1f, 0.1f, 0.8f, 0.9f));
}

TEST(EventWindow, FiresOnSecondHitWithinWindow) {
    zone::EventWindow w;  // window 1000ms, min 2, repeat 500ms
    EXPECT_FALSE(w.record(true, 0));     // first hit, not enough yet
    EXPECT_TRUE(w.record(true, 200));    // second hit within 1s -> fire
}

TEST(EventWindow, SingleHitNeverFires) {
    zone::EventWindow w;
    EXPECT_FALSE(w.record(true, 0));
    EXPECT_FALSE(w.record(false, 200));
    EXPECT_FALSE(w.record(false, 900));
}

TEST(EventWindow, StaleHitExpiresFromWindow) {
    zone::EventWindow w;
    EXPECT_FALSE(w.record(true, 0));      // hit at t=0
    // Second hit arrives after the first has aged out of the 1000ms window.
    EXPECT_FALSE(w.record(true, 1500));   // only one hit in [500,1500] -> no fire
    EXPECT_TRUE(w.record(true, 1700));    // now two hits within window -> fire
}

TEST(EventWindow, RateLimitsRepeatedFires) {
    zone::EventWindow w;
    EXPECT_FALSE(w.record(true, 0));
    EXPECT_TRUE(w.record(true, 100));     // fire at t=100
    EXPECT_FALSE(w.record(true, 300));    // within 500ms repeat -> suppressed
    EXPECT_TRUE(w.record(true, 650));     // >=500ms since last fire -> fire again
}

TEST(EventWindow, ResetClearsHits) {
    zone::EventWindow w;
    EXPECT_FALSE(w.record(true, 0));
    w.reset();
    EXPECT_FALSE(w.record(true, 100));    // window emptied -> needs a fresh pair
    EXPECT_TRUE(w.record(true, 200));
}
