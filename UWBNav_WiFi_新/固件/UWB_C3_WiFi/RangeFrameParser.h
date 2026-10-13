#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// V1.2协议：2字节帧头、2字节版本、1字节对端角色；
// N组记录（4字节大端ID + 2字节大端厘米距离），最后2字节CRC。
class RangeFrameParser {
 public:
  uint8_t frame[67] = {};
  explicit RangeFrameParser(uint8_t count) : frameLength(7 + 6 * count) {}
  static uint16_t crc(const uint8_t *p, size_t n) {
    // 反射CCITT多项式，初值0、最终异或0；已与手册及用户样本核对。
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
      discardFirst(); // 校验失败：逐字节重新寻找帧头，不输出错误距离。
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
