#include <gtest/gtest.h>

#include <algorithm>

#include "rdmachine/transfer_pacer.h"

using namespace rdm;

namespace {

constexpr uint64_t kChunk = 128 * 1024;

// A link of `link_bps` with a fixed one-way delay, and a receiver that
// acknowledges at once: sends as fast as the pacer allows for `us`, returns
// the bytes delivered. Queueing on the link shows up as longer delays.
uint64_t Simulate(TransferPacer& p, double link_bps, uint64_t rtt_us, uint64_t us, uint64_t start = 0,
             uint64_t congestion_every_us = 0) {
  struct Pending {
    uint64_t sent_at, done_at, bytes;
  };
  std::vector<Pending> pending;
  uint64_t link_free = start, in_flight = 0, delivered = 0, next_congestion = start + congestion_every_us;
  for (uint64_t t = start; t < start + us; t += 1000) {
    // Acknowledgements that are due.
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->done_at <= t) {
        p.OnAck(t, t - it->sent_at);
        in_flight -= it->bytes;
        delivered += it->bytes;
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
    if (congestion_every_us && t >= next_congestion) {
      p.OnCongestion(t);
      next_congestion += congestion_every_us;
    }
    while (p.CanSend(t, kChunk, in_flight)) {
      p.OnSent(t, kChunk);
      link_free = std::max(link_free, t) + static_cast<uint64_t>(kChunk * 8 / link_bps * 1e6);
      pending.push_back({t, link_free + rtt_us, kChunk});
      in_flight += kChunk;
    }
  }
  return delivered;
}

}  // namespace

TEST(TransferPacer, StartsAtItsStartingRate) {
  TransferPacer p;
  EXPECT_DOUBLE_EQ(p.rate_bps(), 8e6);
  // The first chunk goes at once; the next only after the bucket refills.
  EXPECT_TRUE(p.CanSend(0, kChunk, 0));
  p.OnSent(0, kChunk);
  EXPECT_FALSE(p.CanSend(1000, kChunk, kChunk));
  EXPECT_GT(p.NextSendUs(1000, kChunk), 1000u);
}

TEST(TransferPacer, SpeedsUpOnAFastLink) {
  TransferPacer p;
  const uint64_t delivered = Simulate(p, 1e9, 1000, 5'000'000);
  EXPECT_GT(p.rate_bps(), 200e6);
  EXPECT_GT(delivered, 100u << 20);  // > 100 MiB in 5 s on a gigabit link
}

TEST(TransferPacer, SettlesNearASlowLinksRate) {
  TransferPacer p;
  Simulate(p, 10e6, 20'000, 20'000'000);
  // It may probe a little above, but no standing multiple of the link.
  EXPECT_LT(p.rate_bps(), 25e6);
  EXPECT_GT(p.rate_bps(), 3e6);
}

TEST(TransferPacer, KeepsTheQueueShortOnASlowLink) {
  TransferPacer p;
  Simulate(p, 5e6, 30'000, 10'000'000);
  // What the window lets out never takes much more than a round trip and
  // its slack to drain.
  EXPECT_LT(static_cast<double>(p.Window()) * 8 / 5e6, 1.0);
}

TEST(TransferPacer, BacksOffWhenThePictureSkipsFrames) {
  TransferPacer a, b;
  Simulate(a, 1e9, 1000, 3'000'000);
  Simulate(b, 1e9, 1000, 3'000'000, 0, 50'000);
  EXPECT_LT(b.rate_bps(), a.rate_bps() / 10);
  EXPECT_GE(b.rate_bps(), 256e3);  // never below the floor
}

TEST(TransferPacer, AFullWindowWaitsForAcknowledgements) {
  TransferPacer p;
  EXPECT_FALSE(p.CanSend(10'000'000, kChunk, p.Window()));
}
