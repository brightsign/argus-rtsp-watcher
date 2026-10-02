#include "npu_core.h"
#include <gtest/gtest.h>

// All other tests in this binary link gtest_main, which supplies main();
// a standalone main() here would collide with it at link time, so these
// assertions are expressed as gtest cases rather than the plain
// assert()-based main() sketched in the task brief.

TEST(SelectCoreMaskTest, NegativeOneMeansAllCores) {
    EXPECT_EQ(select_core_mask(-1), RKNN_NPU_CORE_0_1_2);
}

TEST(SelectCoreMaskTest, ZeroMapsToCore0) {
    EXPECT_EQ(select_core_mask(0), RKNN_NPU_CORE_0);
}

TEST(SelectCoreMaskTest, OneMapsToCore1) {
    EXPECT_EQ(select_core_mask(1), RKNN_NPU_CORE_1);
}

TEST(SelectCoreMaskTest, TwoMapsToCore2) {
    EXPECT_EQ(select_core_mask(2), RKNN_NPU_CORE_2);
}

TEST(SelectCoreMaskTest, OtherOutOfRangeValuesMeanAllCores) {
    EXPECT_EQ(select_core_mask(3), RKNN_NPU_CORE_0_1_2);
    EXPECT_EQ(select_core_mask(-2), RKNN_NPU_CORE_0_1_2);
}
