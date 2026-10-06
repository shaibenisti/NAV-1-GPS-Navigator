// =============================================================================
//  GpsLink  -  Non-blocking NMEA transport layer for the GPS UART
// -----------------------------------------------------------------------------
//  Drains the UART, splits the byte stream into NMEA sentences ("$...*hh"),
//  verifies each checksum and keeps link statistics. It does NOT interpret
//  sentence fields; that is the parser's job (stage 2), which will subscribe
//  through setSentenceHandler().
// =============================================================================
#pragma once

#include <Arduino.h>

class GpsLink {
public:
  // sentence = "$GNRMC,...*4D" (checksum already verified, no CR/LF)
  using SentenceHandler = void (*)(const char *sentence, void *ctx);

  struct Stats {
    uint32_t bytes;          // all bytes received
    uint32_t sentences;      // complete sentences with a valid checksum
    uint32_t badChecksum;    // complete sentences with a wrong/missing checksum
    uint32_t overflows;      // sentences dropped for exceeding the line buffer
    uint32_t truncated;      // sentences cut off by a new '$' (receiver restart / line noise)
    uint32_t receiverRestarts;   // u-blox power-on banner ("u-blox AG") seen after the first 5 s
    uint32_t lastRestartMs;      // millis() of the last one (0 = none)
    uint32_t ubxFrames;          // binary UBX frames with a valid checksum (replies to polls)
    uint32_t ubxBad;             // UBX frames with a wrong checksum / too long (e.g. power-up noise)
    uint32_t banners;            // every u-blox start-up banner seen (incl. at boot and commanded resets)
    uint32_t commandedRestarts;  // banners after a reset NAV-1 ordered (expectRestart): not in receiverRestarts
  };
  // Called for every valid UBX frame (cls, id, payload, len), from update().
  using UbxHandler = void (*)(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len, void *ctx);
  void setUbxHandler(UbxHandler h, void *ctx) { _ubxHandler = h; _ubxCtx = ctx; }
  // The next start-up banner within `windowMs` is a restart NAV-1 ordered (not a supply/wiring problem).
  void expectRestart(uint32_t windowMs) { _expectRestartUntil = millis() + windowMs; }

  // Last complete UBX frame received (class, id, payload). seq increments per frame.
  struct UbxFrame {
    uint32_t seq, atMs;
    uint8_t cls, id;
    uint16_t len;
    uint8_t payload[512];
  };
  const UbxFrame &lastUbx() const { return _ubx; }

  // Diagnostic, one shot (console "gps ubxtest"): sends one UBX message to the module on `txPin`,
  // then releases the pin. The pin is used open-drain at the weakest drive strength (the board pulls
  // it up, docs/HARDWARE.md), and only if it reads HIGH with the internal pull-down on.
  // Returns "" on success, else why nothing was sent.
  const char *sendUbxOnce(int txPin, uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len);

  void begin(HardwareSerial &port, int rxPin, uint32_t baud, size_t rxBufferSize);
  void update();                                   // call every loop(); never blocks

  void setRawEcho(Print *out) { _echo = out; }     // nullptr = off
  void setSentenceHandler(SentenceHandler h, void *ctx) { _handler = h; _handlerCtx = ctx; }

  const Stats &stats() const { return _stats; }
  bool linkUp(uint32_t timeoutMs) const;           // a byte arrived within timeoutMs
  uint32_t msSinceLastByte() const;
  uint32_t msSinceLastSentence() const;
  const char *lastSentence() const { return _last; }

private:
  static constexpr size_t kLineMax = 96;           // NMEA max is 82 chars

  void handleByte(char c);
  bool handleUbxByte(uint8_t c);                   // true = byte belongs to a UBX frame
  void finishLine();
  static bool checksumValid(const char *s);

  HardwareSerial *_port = nullptr;
  Print *_echo = nullptr;
  SentenceHandler _handler = nullptr;
  void *_handlerCtx = nullptr;

  char _line[kLineMax + 1];
  size_t _len = 0;
  bool _inSentence = false;
  bool _overflow = false;

  char _last[kLineMax + 1] = "";
  uint32_t _lastByteMs = 0;
  uint32_t _lastSentenceMs = 0;
  bool _gotByte = false;
  bool _gotSentence = false;
  Stats _stats = {};

  // UBX receive state: 0 = idle, 1 = got 0xB5, 2 = got 0x62, then header / payload / checksum
  uint8_t _ubxState = 0;
  uint8_t _ubxHdr[4] = {};                         // class, id, len lo, len hi
  uint16_t _ubxPos = 0, _ubxLen = 0;
  uint8_t _ubxCkA = 0, _ubxCkB = 0, _ubxRxA = 0;
  UbxFrame _ubxBuild = {}, _ubx = {};
  UbxHandler _ubxHandler = nullptr;
  void *_ubxCtx = nullptr;
  uint32_t _expectRestartUntil = 0;
};
