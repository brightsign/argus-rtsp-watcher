/**
 * @file test_zone_strips.cpp
 * @brief Unit tests for zone_strips::outline_strips(), the geometry that lets
 * the faint zone outline be blended over a few thin strips instead of the whole
 * frame (see orchestration/zone_strips.h). The property that matters is that no
 * pixel the stroke can touch is left out, and none is blended twice.
 */

#include <gtest/gtest.h>
#include <vector>
#include "orchestration/zone_strips.h"

using zone_strips::Rect;
using zone_strips::outline_strips;
using zone_strips::stroke_reach;

namespace {

bool covers(const Rect& rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

int coverage_count(const std::vector<Rect>& strips, int x, int y) {
    int count = 0;
    for (const auto& strip : strips) count += covers(strip, x, y) ? 1 : 0;
    return count;
}

long long total_area(const std::vector<Rect>& strips) {
    long long area = 0;
    for (const auto& strip : strips) area += static_cast<long long>(strip.width) * strip.height;
    return area;
}

// True for a pixel an outline of this thickness on (x0,y0)-(x1,y1) can reach:
// within the stroke's reach of an edge line, and not past the far side.
bool stroke_can_reach(int x, int y, int x0, int y0, int x1, int y1, int thickness) {
    const int reach = stroke_reach(thickness);
    const bool in_box = x >= x0 - reach && x <= x1 + reach && y >= y0 - reach && y <= y1 + reach;
    const bool near_edge = x <= x0 + reach || x >= x1 - reach || y <= y0 + reach || y >= y1 - reach;
    return in_box && near_edge;
}

constexpr int kCanvasWidth = 200;
constexpr int kCanvasHeight = 120;

}  // namespace

TEST(ZoneStrips, EveryPixelTheStrokeCanReachIsCovered) {
    for (const int thickness : {1, 2, 3, 6, 10}) {
        const auto strips = outline_strips(30, 20, 150, 90, thickness, kCanvasWidth, kCanvasHeight);
        for (int y = 0; y < kCanvasHeight; ++y) {
            for (int x = 0; x < kCanvasWidth; ++x) {
                if (stroke_can_reach(x, y, 30, 20, 150, 90, thickness)) {
                    ASSERT_GE(coverage_count(strips, x, y), 1)
                        << "stroke pixel (" << x << "," << y << ") missed at thickness " << thickness;
                }
            }
        }
    }
}

TEST(ZoneStrips, NoPixelIsBlendedTwice) {
    // Blending an overlapped pixel twice would apply the faint alpha twice and
    // change the picture where two strips meet.
    for (const int thickness : {1, 3, 8}) {
        const auto strips = outline_strips(30, 20, 150, 90, thickness, kCanvasWidth, kCanvasHeight);
        for (int y = 0; y < kCanvasHeight; ++y) {
            for (int x = 0; x < kCanvasWidth; ++x) {
                ASSERT_LE(coverage_count(strips, x, y), 1)
                    << "pixel (" << x << "," << y << ") in two strips at thickness " << thickness;
            }
        }
    }
}

TEST(ZoneStrips, TheInteriorAndTheFarOutsideAreLeftAlone) {
    const auto strips = outline_strips(30, 20, 150, 90, 3, kCanvasWidth, kCanvasHeight);

    EXPECT_EQ(coverage_count(strips, 90, 55), 0);   // middle of the zone
    EXPECT_EQ(coverage_count(strips, 199, 119), 0); // far corner of the canvas
    EXPECT_EQ(coverage_count(strips, 5, 5), 0);
}

TEST(ZoneStrips, StripsStayInsideTheCanvas) {
    for (const auto& strip : outline_strips(-40, -30, 260, 150, 4, kCanvasWidth, kCanvasHeight)) {
        EXPECT_GE(strip.x, 0);
        EXPECT_GE(strip.y, 0);
        EXPECT_LE(strip.x + strip.width, kCanvasWidth);
        EXPECT_LE(strip.y + strip.height, kCanvasHeight);
        EXPECT_GT(strip.width, 0);
        EXPECT_GT(strip.height, 0);
    }
}

TEST(ZoneStrips, ALargeZoneBlendsAFractionOfTheFrame) {
    // The real case: a 1280x720 frame with a zone over most of it. The old code
    // blended all 921,600 pixels; the strips must be a small part of that.
    const auto strips = outline_strips(20, 200, 1260, 700, 3, 1280, 720);
    const long long frame_area = 1280LL * 720;

    EXPECT_LT(total_area(strips) * 10, frame_area) << "strips cover " << total_area(strips) << " px";
    EXPECT_LE(strips.size(), 4u);
}

TEST(ZoneStrips, ATinyZoneFallsBackToOneStrip) {
    // Too small for an inner box: the outer box is returned whole.
    const auto strips = outline_strips(100, 60, 104, 63, 3, kCanvasWidth, kCanvasHeight);

    ASSERT_EQ(strips.size(), 1u);
    EXPECT_TRUE(covers(strips[0], 102, 61));
}

TEST(ZoneStrips, AZoneEntirelyOffTheCanvasHasNoStrips) {
    EXPECT_TRUE(outline_strips(500, 500, 600, 600, 3, kCanvasWidth, kCanvasHeight).empty());
    EXPECT_TRUE(outline_strips(-300, -300, -200, -200, 3, kCanvasWidth, kCanvasHeight).empty());
}

TEST(ZoneStrips, AZonePartlyOffTheCanvasStillCoversItsVisibleEdges) {
    // The left edge is off-canvas; the top, bottom and right edges are visible.
    const auto strips = outline_strips(-50, 20, 150, 90, 3, kCanvasWidth, kCanvasHeight);

    EXPECT_GE(coverage_count(strips, 100, 20), 1);   // top edge
    EXPECT_GE(coverage_count(strips, 100, 90), 1);   // bottom edge
    EXPECT_GE(coverage_count(strips, 150, 55), 1);   // right edge
    EXPECT_EQ(coverage_count(strips, 100, 55), 0);   // interior
}

TEST(ZoneStrips, CornerOrderDoesNotMatter) {
    // After a horizontal flip the zone's x coordinates arrive swapped.
    const auto normal = outline_strips(30, 20, 150, 90, 3, kCanvasWidth, kCanvasHeight);
    const auto swapped = outline_strips(150, 90, 30, 20, 3, kCanvasWidth, kCanvasHeight);

    ASSERT_EQ(normal.size(), swapped.size());
    for (size_t i = 0; i < normal.size(); ++i) {
        EXPECT_EQ(normal[i].x, swapped[i].x);
        EXPECT_EQ(normal[i].y, swapped[i].y);
        EXPECT_EQ(normal[i].width, swapped[i].width);
        EXPECT_EQ(normal[i].height, swapped[i].height);
    }
}

TEST(ZoneStrips, MarginAlwaysExceedsTheStrokeReach) {
    // The strips are sized from the margin; if it ever fell to the reach, the
    // anti-aliased fringe of the line would be cut off at a strip edge.
    for (int thickness = 1; thickness <= 30; ++thickness) {
        EXPECT_GT(thickness + zone_strips::kAntialiasMarginPx, stroke_reach(thickness)) << thickness;
    }
}
