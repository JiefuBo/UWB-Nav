#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Manual V1.2: 2-byte header, 2-byte version, 1-byte peer role,
// N * (4-byte big-endian peer ID + 2-byte big-endian range), 2-byte CRC.
class RangeFrameParser {
 public:
  uint8_t frame[67] = {};
  explicit RangeFrameParser(uint8_t count) : frameLength(7 + 6 * count) {}
  static uint16_t crc(const uint8_t *p, size_t n) {
    // Reflected CCITT polynomial, init=0, xorout=0.
    // Matches both manual examples and supplied live samples.
    uint16_t c = 0;
    while (n--) {
      c ^= *p++;
      for (uint8_t i = 0; i < 8; ++i)
        c = (c & 1) ? (c >> 1) ^ 0x8408 : (c >> 1);
    }
    return c;
  }
  template <typename TextOutput>
  bool push(uint8_t b, TextOutput emitText) {
    if (frameLength < 13 || frameLength > sizeof(frame)) return false;
    frame[used++] = b;
    while (used) {
      if (frame[0] != 0x45 || (used >= 2 && frame[1] != 0x91)) {
        emitText(frame[0]);
        discardFirst();
        continue;
      }
      if (used < frameLength) return false;
      const uint16_t wireCRC = (uint16_t(frame[frameLength - 2]) << 8)
                              | frame[frameLength - 1];
      if (frame[4] <= 1 && crc(frame, frameLength - 2) == wireCRC) {
        used = 0;
        return true;
      }
      discardFirst(); // Bad frame: resynchronise instead of printing a range.
    }
    return false;
  }
 private:
  size_t used = 0;
  size_t frameLength;
  void discardFirst() {
    --used;
    memmove(frame, frame + 1, used);
  }
};
