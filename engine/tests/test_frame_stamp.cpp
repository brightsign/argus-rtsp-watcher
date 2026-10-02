/**
 * @file test_frame_stamp.cpp
 * @brief Unit tests for encode_frame_stamp_bytes(), the pure byte/checksum
 * encoder behind the machine-readable frame stamp (see
 * orchestration/frame_stamp.h and draw_frame_stamp() in
 * orchestration/inference_worker.cpp for the pixel layout that consumes it).
 */

#include <gtest/gtest.h>
#include "orchestration/frame_stamp.h"

using inference_worker::encode_frame_stamp_bytes;

TEST(FrameStampEncode, ZeroSeqEncodesToAllZeroBytes) {
    auto bytes = encode_frame_stamp_bytes(0);
    for (uint8_t b : bytes) {
        EXPECT_EQ(b, 0);
    }
}

TEST(FrameStampEncode, KnownValueIsMsbFirst) {
    auto bytes = encode_frame_stamp_bytes(0x0102030405060708ULL);
    EXPECT_EQ(bytes[0], 0x01);
    EXPECT_EQ(bytes[1], 0x02);
    EXPECT_EQ(bytes[2], 0x03);
    EXPECT_EQ(bytes[3], 0x04);
    EXPECT_EQ(bytes[4], 0x05);
    EXPECT_EQ(bytes[5], 0x06);
    EXPECT_EQ(bytes[6], 0x07);
    EXPECT_EQ(bytes[7], 0x08);

    const uint8_t expected_checksum = 0x01 ^ 0x02 ^ 0x03 ^ 0x04 ^ 0x05 ^ 0x06 ^ 0x07 ^ 0x08;
    EXPECT_EQ(bytes[8], expected_checksum);
}

TEST(FrameStampEncode, MaxSeqEncodesToAllOnesWithZeroChecksum) {
    auto bytes = encode_frame_stamp_bytes(UINT64_MAX);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(bytes[static_cast<size_t>(i)], 0xFF);
    }
    // XOR of eight identical 0xFF bytes is 0.
    EXPECT_EQ(bytes[8], 0x00);
}

TEST(FrameStampEncode, ChecksumIsDeterministicAndReproducible) {
    auto a = encode_frame_stamp_bytes(123456789ULL);
    auto b = encode_frame_stamp_bytes(123456789ULL);
    EXPECT_EQ(a, b);
}

TEST(FrameStampEncode, ChecksumDetectsSingleByteCorruption) {
    auto bytes = encode_frame_stamp_bytes(987654321ULL);

    // Simulate a decoder misreading one cell (e.g. a JPEG artifact flips a
    // sampled bit): corrupt one seq byte and confirm the checksum no longer
    // matches, which is the whole point of carrying it.
    bytes[3] ^= 0x01;

    uint8_t recomputed_checksum = 0;
    for (int i = 0; i < 8; ++i) {
        recomputed_checksum ^= bytes[static_cast<size_t>(i)];
    }
    EXPECT_NE(recomputed_checksum, bytes[8]);
}
