/**
 * @file test_log_naming.cpp
 * @brief Unit tests for the per-stream log file naming rules (see
 * metrics/log_naming.h). The one that matters most: a device id taken from
 * config must never be able to name a file outside the log directory.
 */

#include <gtest/gtest.h>
#include "metrics/log_naming.h"

using log_naming::path_stem;
using log_naming::sanitize_log_stem;
using log_naming::stream_log_path;

TEST(SanitizeLogStem, KeepsOrdinaryStreamIds) {
    EXPECT_EQ(sanitize_log_stem("reo1"), "reo1");
    EXPECT_EQ(sanitize_log_stem("back-door"), "back-door");
    EXPECT_EQ(sanitize_log_stem("Front_Door_2"), "Front_Door_2");
}

TEST(SanitizeLogStem, ReplacesPathSyntaxSoTheNameCannotEscapeTheDirectory) {
    EXPECT_EQ(sanitize_log_stem("../../etc/passwd"), "______etc_passwd");
    EXPECT_EQ(sanitize_log_stem("a/b"), "a_b");
    EXPECT_EQ(sanitize_log_stem(".."), "__");
    EXPECT_EQ(sanitize_log_stem("."), "_");
    EXPECT_EQ(sanitize_log_stem("a\\b"), "a_b");
    for (const char* hostile : {"../x", "/abs", "a/../b", "...", "x/", "\\..\\y"}) {
        const std::string safe = sanitize_log_stem(hostile);
        EXPECT_EQ(safe.find('/'), std::string::npos) << hostile;
        EXPECT_EQ(safe.find('.'), std::string::npos) << hostile;
        EXPECT_EQ(safe.find('\\'), std::string::npos) << hostile;
    }
}

TEST(SanitizeLogStem, ReplacesOtherUnsafeCharacters) {
    EXPECT_EQ(sanitize_log_stem("my camera"), "my_camera");
    EXPECT_EQ(sanitize_log_stem("a:b*c?"), "a_b_c_");
    EXPECT_EQ(sanitize_log_stem("xt5-01.local"), "xt5-01_local");
}

TEST(SanitizeLogStem, ReplacesNonAsciiAndControlBytes) {
    EXPECT_EQ(sanitize_log_stem("caf\xc3\xa9"), "caf__");
    EXPECT_EQ(sanitize_log_stem(std::string("a\0b", 3)), "a_b");
    EXPECT_EQ(sanitize_log_stem("a\nb"), "a_b");
}

TEST(SanitizeLogStem, EmptyInputFallsBack) {
    EXPECT_EQ(sanitize_log_stem(""), log_naming::kFallbackStem);
}

TEST(PathStem, StripsDirectoriesAndTheLastExtension) {
    EXPECT_EQ(path_stem("/tmp/argus-watcher/reo1.json"), "reo1");
    EXPECT_EQ(path_stem("reo1.json"), "reo1");
    EXPECT_EQ(path_stem("/a/b/archive.tar.gz"), "archive.tar");
}

TEST(PathStem, LeavesANameWithNoExtensionAlone) {
    EXPECT_EQ(path_stem("/tmp/reo1"), "reo1");
    EXPECT_EQ(path_stem("reo1"), "reo1");
}

TEST(PathStem, ALeadingDotIsNotAnExtension) {
    EXPECT_EQ(path_stem("/tmp/.hidden"), ".hidden");
}

TEST(PathStem, HandlesAnEmptyPathAndADirectoryOnlyPath) {
    EXPECT_EQ(path_stem(""), "");
    EXPECT_EQ(path_stem("/tmp/"), "");
}

TEST(StreamLogPath, JoinsDirectoryStemAndExtension) {
    EXPECT_EQ(stream_log_path("/tmp", "reo1"), "/tmp/reo1.log");
    EXPECT_EQ(stream_log_path("/storage/sd/logs", "back-door"), "/storage/sd/logs/back-door.log");
}

TEST(StreamLogPath, DoesNotDoubleATrailingSlash) {
    EXPECT_EQ(stream_log_path("/tmp/", "reo1"), "/tmp/reo1.log");
}

TEST(StreamLogPath, DifferentStreamsGetDifferentFiles) {
    // The whole point: no two streams share a file, so rotation never collides.
    EXPECT_NE(stream_log_path("/tmp", "pixley"), stream_log_path("/tmp", "reo1"));
}
