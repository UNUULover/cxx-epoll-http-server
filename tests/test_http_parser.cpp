#include <gtest/gtest.h>

#include <string>

#include "ws/buffer.h"
#include "ws/http_parser.h"

using namespace ws;

namespace {

// 把一段字节喂给解析器，返回结果。
HttpParser::Ret feed(HttpParser* p, Buffer* b, const std::string& data) {
  b->append(data.data(), data.size());
  return p->parse(b);
}

}  // namespace

TEST(HttpParser, SimpleGet) {
  HttpParser p;
  Buffer b;
  const std::string raw =
      "GET /index.html HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "User-Agent: wrk\r\n"
      "\r\n";

  ASSERT_EQ(feed(&p, &b, raw), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().method, "GET");
  EXPECT_EQ(p.request().path, "/index.html");
  EXPECT_EQ(p.request().version, "HTTP/1.1");
  EXPECT_EQ(p.request().getHeader("host"), "example.com");
  EXPECT_EQ(p.request().getHeader("user-agent"), "wrk");
  EXPECT_TRUE(p.request().body.empty());
}

// 状态机的核心价值：请求被拆成多个 TCP 段陆续到达时，能从上次停下的状态继续，
// 而不是每次从头重扫。这里逐字节喂，必须一路 INCOMPLETE，最后才 OK。
TEST(HttpParser, ByteByByteDeliveryResumesFromState) {
  HttpParser p;
  Buffer b;
  const std::string raw =
      "GET /a HTTP/1.1\r\n"
      "Host: h\r\n"
      "\r\n";

  for (size_t i = 0; i + 1 < raw.size(); ++i) {
    ASSERT_EQ(feed(&p, &b, raw.substr(i, 1)), HttpParser::Ret::INCOMPLETE)
        << "unexpected result after byte " << i;
  }
  EXPECT_EQ(feed(&p, &b, raw.substr(raw.size() - 1, 1)), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/a");
}

// 请求行到了、头部还没到完。
TEST(HttpParser, PartialHeaders) {
  HttpParser p;
  Buffer b;
  // 第一段停在「Host 这一行的 CRLF 还没到」的位置。
  EXPECT_EQ(feed(&p, &b, "GET /x HTTP/1.1\r\nHost: abc"), HttpParser::Ret::INCOMPLETE);
  // 第二段补上 Host 的 CRLF 和结束头部的空行。
  EXPECT_EQ(feed(&p, &b, "\r\n\r\n"), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().getHeader("host"), "abc");
}

// 头部的 CRLF 到了、但结束头部的空行还没到，仍然不能算完成。
TEST(HttpParser, MissingBlankLineTerminator) {
  HttpParser p;
  Buffer b;
  EXPECT_EQ(feed(&p, &b, "GET /x HTTP/1.1\r\nHost: abc\r\n"), HttpParser::Ret::INCOMPLETE);
  EXPECT_EQ(feed(&p, &b, "\r\n"), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().getHeader("host"), "abc");
}

TEST(HttpParser, PostWithContentLength) {
  HttpParser p;
  Buffer b;
  const std::string raw =
      "POST /echo HTTP/1.1\r\n"
      "Host: h\r\n"
      "Content-Length: 11\r\n"
      "\r\n"
      "hello world";

  ASSERT_EQ(feed(&p, &b, raw), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().method, "POST");
  EXPECT_EQ(p.request().body, "hello world");
}

// body 被切开：Content-Length 说 11 字节，先只到 4 字节，必须等齐了才算完成。
TEST(HttpParser, BodyArrivesInPieces) {
  HttpParser p;
  Buffer b;
  ASSERT_EQ(feed(&p, &b, "POST /echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhell"),
            HttpParser::Ret::INCOMPLETE);
  ASSERT_EQ(feed(&p, &b, "o wor"), HttpParser::Ret::INCOMPLETE);
  ASSERT_EQ(feed(&p, &b, "ld"), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().body, "hello world");
}

TEST(HttpParser, QueryStringIsSplitOff) {
  HttpParser p;
  Buffer b;
  ASSERT_EQ(feed(&p, &b, "GET /search?q=c%2B%2B&page=2 HTTP/1.1\r\n\r\n"),
            HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/search");
  EXPECT_EQ(p.request().query, "q=c%2B%2B&page=2");
}

TEST(HttpParser, HeaderKeysLowercasedAndValuesTrimmed) {
  HttpParser p;
  Buffer b;
  const std::string raw =
      "GET / HTTP/1.1\r\n"
      "Content-TYPE:   text/plain   \r\n"
      "X-Thing:\tv\r\n"
      "\r\n";
  ASSERT_EQ(feed(&p, &b, raw), HttpParser::Ret::OK);
  EXPECT_TRUE(p.request().hasHeader("content-type"));
  EXPECT_EQ(p.request().getHeader("content-type"), "text/plain");
  EXPECT_EQ(p.request().getHeader("x-thing"), "v");
}

// keep-alive 的判定规则：1.1 默认保持，1.0 默认断开。
TEST(HttpParser, KeepAliveSemantics) {
  {
    HttpParser p;
    Buffer b;
    ASSERT_EQ(feed(&p, &b, "GET / HTTP/1.1\r\n\r\n"), HttpParser::Ret::OK);
    EXPECT_TRUE(p.request().keepAlive());
  }
  {
    HttpParser p;
    Buffer b;
    ASSERT_EQ(feed(&p, &b, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"),
              HttpParser::Ret::OK);
    EXPECT_FALSE(p.request().keepAlive());
  }
  {
    HttpParser p;
    Buffer b;
    ASSERT_EQ(feed(&p, &b, "GET / HTTP/1.1\r\nConnection: Keep-Alive\r\n\r\n"),
              HttpParser::Ret::OK);
    EXPECT_TRUE(p.request().keepAlive());  // 大小写不敏感
  }
  {
    HttpParser p;
    Buffer b;
    ASSERT_EQ(feed(&p, &b, "GET / HTTP/1.0\r\n\r\n"), HttpParser::Ret::OK);
    EXPECT_FALSE(p.request().keepAlive());
  }
  {
    HttpParser p;
    Buffer b;
    ASSERT_EQ(feed(&p, &b, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
              HttpParser::Ret::OK);
    EXPECT_TRUE(p.request().keepAlive());
  }
}

// pipelining：一个缓冲区里塞了两个请求，reset() 之后第二个还能接着解析。
TEST(HttpParser, PipelinedRequestsWithReset) {
  HttpParser p;
  Buffer b;
  ASSERT_EQ(feed(&p, &b, "GET /one HTTP/1.1\r\n\r\n"), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/one");
  EXPECT_EQ(b.readableBytes(), 0u);

  p.reset();
  ASSERT_EQ(feed(&p, &b, "GET /two HTTP/1.1\r\n\r\n"), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/two");
}

TEST(HttpParser, TwoRequestsInOneBufferConsumeOnlyFirst) {
  HttpParser p;
  Buffer b;
  const std::string both =
      "GET /one HTTP/1.1\r\n\r\n"
      "GET /two HTTP/1.1\r\n\r\n";
  ASSERT_EQ(feed(&p, &b, both), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/one");
  p.reset();
  ASSERT_EQ(p.parse(&b), HttpParser::Ret::OK);
  EXPECT_EQ(p.request().path, "/two");
  EXPECT_EQ(b.readableBytes(), 0u);
}

TEST(HttpParser, MalformedRequestLineIsError) {
  {
    HttpParser p;
    Buffer b;
    EXPECT_EQ(feed(&p, &b, "GARBAGE\r\n\r\n"), HttpParser::Ret::ERROR);
  }
  {
    HttpParser p;
    Buffer b;
    EXPECT_EQ(feed(&p, &b, "GET /only-two-parts\r\n\r\n"), HttpParser::Ret::ERROR);
  }
  {
    HttpParser p;
    Buffer b;
    EXPECT_EQ(feed(&p, &b, "GET / HTTP/9.9\r\n\r\n"), HttpParser::Ret::ERROR);
  }
}

// 超长请求行要在读完之前就被判死，不能让对端把内存灌满。
TEST(HttpParser, OversizedRequestLineIsError) {
  HttpParser p;
  Buffer b;
  std::string huge = "GET /" + std::string(HttpParser::kMaxRequestLine + 64, 'a');
  EXPECT_EQ(feed(&p, &b, huge), HttpParser::Ret::ERROR);
}

TEST(HttpParser, MalformedHeaderIsError) {
  HttpParser p;
  Buffer b;
  EXPECT_EQ(feed(&p, &b, "GET / HTTP/1.1\r\nNoColonHere\r\n\r\n"), HttpParser::Ret::ERROR);
}

TEST(HttpParser, NegativeContentLengthIsError) {
  HttpParser p;
  Buffer b;
  EXPECT_EQ(feed(&p, &b, "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n"),
            HttpParser::Ret::ERROR);
}

// 目前只支持定长 body，chunked 明确报错（由上层回 501 而不是静默解析错）。
TEST(HttpParser, ChunkedIsRejected) {
  HttpParser p;
  Buffer b;
  EXPECT_EQ(feed(&p, &b, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
            HttpParser::Ret::ERROR);
}

TEST(HttpResponse, SerializesWithContentLength) {
  HttpResponse r = HttpResponse::make(200, "hi", "text/plain");
  const std::string out = r.toString();
  EXPECT_NE(out.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(out.find("Content-Length: 2\r\n"), std::string::npos);
  EXPECT_NE(out.find("\r\n\r\nhi"), std::string::npos);
}

TEST(HttpResponse, ExplicitContentLengthIsNotDuplicated) {
  HttpResponse r = HttpResponse::make(200, "");
  r.setHeader("Content-Length", "123");
  const std::string out = r.toString();
  EXPECT_EQ(out.find("Content-Length: 123"), out.rfind("Content-Length: 123"));
}
