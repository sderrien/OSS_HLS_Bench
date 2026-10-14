#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex g_stream_mu;
std::unordered_map<void *, std::deque<std::uint8_t>> g_stream_bytes;
}

#if defined(__clang__)
using ohls_u128 = unsigned _BitInt(128);
using ohls_u256 = unsigned _BitInt(256);
#else
struct ohls_u128 { unsigned long long words[2]; };
struct ohls_u256 { unsigned long long words[4]; };
#endif

extern "C" void ohls_stream_write_bytes(void *chan, const void *data, unsigned nbytes) {
  if (chan == nullptr || data == nullptr) {
    std::fprintf(stderr, "ohls_stream_write_bytes: null argument\n");
    std::abort();
  }
  const auto *src = static_cast<const std::uint8_t *>(data);
  std::lock_guard<std::mutex> lock(g_stream_mu);
  auto &q = g_stream_bytes[chan];
  for (unsigned i = 0; i < nbytes; ++i) {
    q.push_back(src[i]);
  }
}

extern "C" void ohls_stream_read_bytes(void *chan, void *data, unsigned nbytes) {
  if (chan == nullptr || data == nullptr) {
    std::fprintf(stderr, "ohls_stream_read_bytes: null argument\n");
    std::abort();
  }
  auto *dst = static_cast<std::uint8_t *>(data);
  std::lock_guard<std::mutex> lock(g_stream_mu);
  auto it = g_stream_bytes.find(chan);
  if (it == g_stream_bytes.end() || it->second.size() < nbytes) {
    std::fprintf(stderr, "ohls_stream_read_bytes: underflow on channel %p (need %u bytes)\n", chan, nbytes);
    std::abort();
  }
  auto &q = it->second;
  for (unsigned i = 0; i < nbytes; ++i) {
    dst[i] = q.front();
    q.pop_front();
  }
}

extern "C" unsigned ohls_stream_available_bytes(void *chan) {
  if (chan == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_stream_mu);
  auto it = g_stream_bytes.find(chan);
  if (it == g_stream_bytes.end()) {
    return 0;
  }
  return static_cast<unsigned>(it->second.size());
}

extern "C" std::uint8_t __ohls_stream_read_i8(void *chan) {
  std::uint8_t v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" std::uint16_t __ohls_stream_read_i16(void *chan) {
  std::uint16_t v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" std::uint32_t __ohls_stream_read_i32(void *chan) {
  std::uint32_t v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" std::uint64_t __ohls_stream_read_i64(void *chan) {
  std::uint64_t v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" ohls_u128 __ohls_stream_read_i128(void *chan) {
  ohls_u128 v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" ohls_u256 __ohls_stream_read_i256(void *chan) {
  ohls_u256 v{};
  ohls_stream_read_bytes(chan, &v, sizeof(v));
  return v;
}

extern "C" void __ohls_stream_write_i8(void *chan, std::uint8_t value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void __ohls_stream_write_i16(void *chan, std::uint16_t value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void __ohls_stream_write_i32(void *chan, std::uint32_t value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void __ohls_stream_write_i64(void *chan, std::uint64_t value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void __ohls_stream_write_i128(void *chan, ohls_u128 value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void __ohls_stream_write_i256(void *chan, ohls_u256 value) {
  ohls_stream_write_bytes(chan, &value, sizeof(value));
}

extern "C" void ohls_stream_write_bytes_guarded(void *chan, const void *data,
                                                 unsigned nbytes, bool guard) {
  if (!guard)
    return;
  ohls_stream_write_bytes(chan, data, nbytes);
}

extern "C" void ohls_stream_read_bytes_guarded(void *chan, void *data,
                                                unsigned nbytes, bool guard) {
  if (!guard)
    return;
  ohls_stream_read_bytes(chan, data, nbytes);
}

extern "C" std::uint8_t __ohls_stream_read_i8_guarded(void *chan, bool guard) {
  return guard ? __ohls_stream_read_i8(chan) : 0;
}

extern "C" std::uint16_t __ohls_stream_read_i16_guarded(void *chan, bool guard) {
  return guard ? __ohls_stream_read_i16(chan) : 0;
}

extern "C" std::uint32_t __ohls_stream_read_i32_guarded(void *chan, bool guard) {
  return guard ? __ohls_stream_read_i32(chan) : 0;
}

extern "C" std::uint64_t __ohls_stream_read_i64_guarded(void *chan, bool guard) {
  return guard ? __ohls_stream_read_i64(chan) : 0;
}

extern "C" ohls_u128 __ohls_stream_read_i128_guarded(void *chan, bool guard) {
  if (guard) return __ohls_stream_read_i128(chan);
  return ohls_u128{};
}

extern "C" ohls_u256 __ohls_stream_read_i256_guarded(void *chan, bool guard) {
  if (guard) return __ohls_stream_read_i256(chan);
  return ohls_u256{};
}

extern "C" void __ohls_stream_write_i8_guarded(void *chan, std::uint8_t value, bool guard) {
  if (guard) __ohls_stream_write_i8(chan, value);
}

extern "C" void __ohls_stream_write_i16_guarded(void *chan, std::uint16_t value, bool guard) {
  if (guard) __ohls_stream_write_i16(chan, value);
}

extern "C" void __ohls_stream_write_i32_guarded(void *chan, std::uint32_t value, bool guard) {
  if (guard) __ohls_stream_write_i32(chan, value);
}

extern "C" void __ohls_stream_write_i64_guarded(void *chan, std::uint64_t value, bool guard) {
  if (guard) __ohls_stream_write_i64(chan, value);
}

extern "C" void __ohls_stream_write_i128_guarded(void *chan, ohls_u128 value, bool guard) {
  if (guard) __ohls_stream_write_i128(chan, value);
}

extern "C" void __ohls_stream_write_i256_guarded(void *chan, ohls_u256 value, bool guard) {
  if (guard) __ohls_stream_write_i256(chan, value);
}
