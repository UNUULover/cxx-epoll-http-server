#include <getopt.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ws/logger.h"
#include "ws/server.h"

namespace {

ws::Server* g_server = nullptr;

// 信号处理函数里只做两件异步信号安全的事：改原子标志、往 eventfd 写 8 字节。
// Server::stop() 内部就是「atomic store + write(eventfd)」，不用 mutex。
void onSignal(int sig) {
  (void)sig;
  if (g_server) g_server->stop();
}

void printUsage(const char* prog) {
  printf(
      "usage: %s [options]\n"
      "  -p, --port N        listen port (default 8080, 0 = pick a free port)\n"
      "  -t, --threads N     sub-reactor IO threads; 0 = single-threaded reactor (default 0)\n"
      "  -d, --doc-root DIR  static file root (default ./www)\n"
      "  -i, --idle N        idle connection timeout in seconds, 0 = disabled (default 60)\n"
      "  -m, --max-conn N    max concurrent connections (default 20000)\n"
      "  -l, --log FILE      log file, '-' = stdout (default -)\n"
      "  -v, --log-level LV  trace|debug|info|warn|error (default info)\n"
      "  -h, --help          show this help\n",
      prog);
}

bool parseLevel(const std::string& s, ws::LogLevel* out) {
  if (s == "trace") *out = ws::LOG_TRACE;
  else if (s == "debug") *out = ws::LOG_DEBUG;
  else if (s == "info") *out = ws::LOG_INFO;
  else if (s == "warn") *out = ws::LOG_WARN;
  else if (s == "error") *out = ws::LOG_ERROR;
  else return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ws::Server::Options opts;
  std::string log_file = "-";
  ws::LogLevel level = ws::LOG_INFO;

  static const struct option long_opts[] = {
      {"port", required_argument, nullptr, 'p'},
      {"threads", required_argument, nullptr, 't'},
      {"doc-root", required_argument, nullptr, 'd'},
      {"idle", required_argument, nullptr, 'i'},
      {"max-conn", required_argument, nullptr, 'm'},
      {"log", required_argument, nullptr, 'l'},
      {"log-level", required_argument, nullptr, 'v'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  int c = 0;
  while ((c = getopt_long(argc, argv, "p:t:d:i:m:l:v:h", long_opts, nullptr)) != -1) {
    switch (c) {
      case 'p': opts.port = static_cast<uint16_t>(atoi(optarg)); break;
      case 't': opts.io_threads = atoi(optarg); break;
      case 'd': opts.doc_root = optarg; break;
      case 'i': opts.idle_timeout_s = atoi(optarg); break;
      case 'm': opts.max_connections = atoi(optarg); break;
      case 'l': log_file = optarg; break;
      case 'v':
        if (!parseLevel(optarg, &level)) {
          fprintf(stderr, "unknown log level: %s\n", optarg);
          return 1;
        }
        break;
      case 'h': printUsage(argv[0]); return 0;
      default: printUsage(argv[0]); return 1;
    }
  }

  if (opts.io_threads < 0) {
    fprintf(stderr, "--threads must be >= 0\n");
    return 1;
  }

  ws::AsyncLogger::instance().start(log_file, 1000);
  ws::AsyncLogger::instance().setLevel(level);

  ws::Server server(opts);
  g_server = &server;

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);  // 写已关闭的连接会收到 SIGPIPE，忽略掉由 write 返回 EPIPE 处理

  if (!server.start()) {
    ws::AsyncLogger::instance().stop();
    return 1;
  }

  ws::AsyncLogger::instance().stop();
  return 0;
}
