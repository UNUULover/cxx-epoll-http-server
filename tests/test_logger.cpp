#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "ws/logger.h"

using namespace ws;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

size_t countLines(const std::string& s) {
  size_t n = 0;
  for (char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

}  // namespace

// 双缓冲的正确性底线：无论缓冲区换手多少次，入队的每一行都必须落盘，一行不丢。
// 这里故意写足够多的行（远超 64KB 的单缓冲区容量）来逼出多次 swap。
TEST(AsyncLogger, EveryQueuedLineReachesDisk) {
  const std::string path = "/tmp/ws_logger_test.txt";
  std::remove(path.c_str());

  auto& logger = AsyncLogger::instance();
  logger.setLevel(LOG_INFO);
  logger.start(path, 20);

  const int kLines = 5000;
  for (int i = 0; i < kLines; ++i) {
    LOG_INFO("line %d payload %s", i, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  }
  logger.stop();  // stop() 会 join 落盘线程并 flush 残留

  const std::string content = readFile(path);
  EXPECT_EQ(countLines(content), static_cast<size_t>(kLines));
  EXPECT_NE(content.find("line 0 "), std::string::npos);
  EXPECT_NE(content.find("line 4999 "), std::string::npos);
  EXPECT_GT(logger.bytesWritten(), 0u);
  EXPECT_GE(logger.enqueuedBytes(), logger.bytesWritten());

  // 前缀（时间戳 + 级别 + 文件:行）必须真的落进输出。
  // 这条是在回归一个真实 bug：前缀格式化进了 head，却忘了拷贝到输出缓冲，
  // 结果每行开头都是缓冲区里的陈旧字节，级别和文件名全都丢了。
  EXPECT_EQ(content.compare(4, 1, "-"), 0) << "line must start with a YYYY-MM-DD timestamp";
  EXPECT_NE(content.find("INFO"), std::string::npos) << "level name missing from log lines";
  EXPECT_NE(content.find("test_logger.cpp:"), std::string::npos) << "source location missing";
}

TEST(AsyncLogger, LevelFilterSuppressesLowerLevels) {
  const std::string path = "/tmp/ws_logger_level.txt";
  std::remove(path.c_str());

  auto& logger = AsyncLogger::instance();
  logger.setLevel(LOG_ERROR);
  logger.start(path, 20);

  LOG_INFO("this must not appear");
  LOG_DEBUG("this must not appear either");
  LOG_ERROR("this must appear");

  logger.setLevel(LOG_INFO);
  logger.stop();

  const std::string content = readFile(path);
  EXPECT_EQ(content.find("this must not appear"), std::string::npos);
  EXPECT_NE(content.find("this must appear"), std::string::npos);
  EXPECT_NE(content.find("ERROR"), std::string::npos);
}

// 格式化参数和位置都要对得上，否则日志在排查问题时反而会误导人。
TEST(AsyncLogger, FormatsArgumentsAndSourceLocation) {
  const std::string path = "/tmp/ws_logger_format.txt";
  std::remove(path.c_str());

  auto& logger = AsyncLogger::instance();
  logger.setLevel(LOG_INFO);
  logger.start(path, 20);

  LOG_WARN("client=%s port=%d ratio=%.2f", "10.0.0.7", 8080, 0.5);
  logger.stop();

  const std::string content = readFile(path);
  EXPECT_NE(content.find("client=10.0.0.7 port=8080 ratio=0.50"), std::string::npos);
  EXPECT_NE(content.find("WARN"), std::string::npos);
  EXPECT_NE(content.find("test_logger.cpp"), std::string::npos);
}

TEST(AsyncLogger, LongLineIsTruncatedNotOverflowed) {
  const std::string path = "/tmp/ws_logger_long.txt";
  std::remove(path.c_str());

  auto& logger = AsyncLogger::instance();
  logger.setLevel(LOG_INFO);
  logger.start(path, 20);

  const std::string huge(20000, 'z');
  LOG_INFO("%s", huge.c_str());
  logger.stop();

  const std::string content = readFile(path);
  EXPECT_GT(content.size(), 1000u);
  EXPECT_NE(content.find("zzzz"), std::string::npos);
  // 栈上的格式化缓冲有上限，超长行必须被截断而不是踩坏内存。
  EXPECT_LT(content.size(), huge.size());
}
