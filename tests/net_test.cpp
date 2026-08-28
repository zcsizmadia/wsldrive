#include "net/frame_channel.hpp"
#include "net/socket.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>

namespace wsld::net {
namespace {

TEST(Endpoint, ParseAndFormat) {
  auto tcp = Endpoint::parse("tcp://127.0.0.1:7788");
  ASSERT_TRUE(tcp.has_value());
  EXPECT_EQ(tcp->kind, Endpoint::Kind::Tcp);
  EXPECT_EQ(tcp->host, "127.0.0.1");
  EXPECT_EQ(tcp->port, 7788u);
  EXPECT_EQ(tcp->to_string(), "tcp://127.0.0.1:7788");

  auto v6 = Endpoint::parse("tcp://[::1]:80");
  ASSERT_TRUE(v6.has_value());
  EXPECT_EQ(v6->host, "::1");
  EXPECT_EQ(v6->to_string(), "tcp://[::1]:80");

  auto vs = Endpoint::parse("vsock://host:5000");
  ASSERT_TRUE(vs.has_value());
  EXPECT_EQ(vs->kind, Endpoint::Kind::Vsock);
  EXPECT_EQ(vs->cid, 2u);
  EXPECT_EQ(vs->to_string(), "vsock://host:5000");
  EXPECT_EQ(Endpoint::parse("vsock://any:1")->cid, 0xFFFFFFFFu);
  EXPECT_EQ(Endpoint::parse("vsock://7:1")->cid, 7u);

  auto hv = Endpoint::parse("hv://5000");
  ASSERT_TRUE(hv.has_value());
  EXPECT_EQ(hv->kind, Endpoint::Kind::HyperV);
  EXPECT_TRUE(hv->vm_id.empty());
  EXPECT_EQ(hv->port, 5000u);
  auto hv2 = Endpoint::parse("hv://{12345678-1234-1234-1234-123456789abc}:9");
  ASSERT_TRUE(hv2.has_value());
  EXPECT_EQ(hv2->vm_id, "{12345678-1234-1234-1234-123456789abc}");
  EXPECT_EQ(hv2->port, 9u);

  EXPECT_FALSE(Endpoint::parse("").has_value());
  EXPECT_FALSE(Endpoint::parse("tcp://").has_value());
  EXPECT_FALSE(Endpoint::parse("tcp://host").has_value());
  EXPECT_FALSE(Endpoint::parse("tcp://host:99999").has_value());
  EXPECT_FALSE(Endpoint::parse("tcp://host:abc").has_value());
  EXPECT_FALSE(Endpoint::parse("udp://h:1").has_value());
  EXPECT_FALSE(Endpoint::parse("vsock://x").has_value());
}

TEST(Socket, TcpLoopbackFrames) {
  auto listener = Listener::bind(*Endpoint::parse("tcp://127.0.0.1:0"));
  ASSERT_TRUE(listener.has_value()) << last_socket_error();
  ASSERT_NE(listener->local().port, 0u);

  std::thread server([&] {
    auto s = listener->accept();
    ASSERT_TRUE(s.has_value());
    FrameChannel ch(std::move(*s));
    for (;;) {
      auto f = ch.receive();
      if (!f) {
        EXPECT_EQ(f.error(), Errc::ConnectionClosed);
        break;
      }
      // Echo with the type bumped by one.
      const auto echo_type = static_cast<proto::MsgType>(static_cast<std::uint16_t>(f->header.type) + 1);
      ASSERT_TRUE(ch.send(echo_type, f->header.request_id, f->payload).has_value());
    }
  });

  auto sock = connect(listener->local());
  ASSERT_TRUE(sock.has_value()) << last_socket_error();
  FrameChannel ch(std::move(*sock));

  // Small frame.
  const std::string hello = "hello";
  ASSERT_TRUE(ch.send(proto::MsgType::Ping, 7, std::as_bytes(std::span{hello})).has_value());
  auto f = ch.receive();
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->header.type, proto::MsgType::Pong);
  EXPECT_EQ(f->header.request_id, 7u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(f->payload.data()), f->payload.size()), hello);

  // Empty frame.
  ASSERT_TRUE(ch.send(proto::MsgType::Ping, 8, {}).has_value());
  f = ch.receive();
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->payload.size(), 0u);

  // Large frame (8 MiB) exercises partial sends/receives.
  std::vector<std::byte> big(8u << 20);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i * 2654435761u >> 13);
  ASSERT_TRUE(ch.send(proto::MsgType::Snapshot, 9, big).has_value());
  f = ch.receive();
  ASSERT_TRUE(f.has_value());
  ASSERT_EQ(f->payload.size(), big.size());
  EXPECT_TRUE(std::equal(big.begin(), big.end(), f->payload.begin()));

  // Pre-built multi-frame buffer via send_raw.
  std::vector<std::byte> two;
  proto::write_frame(two, proto::MsgType::Ping, 10, [](proto::Writer& w) { w.u32(1); });
  proto::write_frame(two, proto::MsgType::Ping, 11, [](proto::Writer& w) { w.u32(2); });
  ASSERT_TRUE(ch.send_raw(two).has_value());
  f = ch.receive();
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->header.request_id, 10u);
  f = ch.receive();
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->header.request_id, 11u);

  EXPECT_EQ(ch.stats().frames_sent, 4u);  // 3 send() + 1 send_raw() (the two-frame buffer counts once)
  EXPECT_EQ(ch.stats().frames_received, 5u);
  EXPECT_GT(ch.stats().bytes_received, big.size());

  ch.shutdown();
  ch.close();
  server.join();
}

TEST(Socket, ConnectRefused) {
  auto listener = Listener::bind(*Endpoint::parse("tcp://127.0.0.1:0"));
  ASSERT_TRUE(listener.has_value());
  const Endpoint ep = listener->local();
  listener->close();
  // On WSL2 with localhostForwarding the Windows-side proxy may still accept a
  // connection to a just-closed loopback port; there the refusal never happens.
  if (connect(ep).has_value()) GTEST_SKIP() << "loopback proxy accepted a closed port (WSL2 localhostForwarding)";
  EXPECT_FALSE(connect(ep).has_value());
}

TEST(Socket, TooLargeFrameRejected) {
  auto listener = Listener::bind(*Endpoint::parse("tcp://127.0.0.1:0"));
  ASSERT_TRUE(listener.has_value());
  auto sock = connect(listener->local());
  ASSERT_TRUE(sock.has_value());
  FrameChannel ch(std::move(*sock));
  std::vector<std::byte> huge(proto::kMaxPayload + 1);
  EXPECT_EQ(ch.send(proto::MsgType::Snapshot, 1, huge).error(), Errc::TooLarge);
}

}  // namespace
}  // namespace wsld::net
