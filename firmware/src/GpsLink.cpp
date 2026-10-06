#include "GpsLink.h"

#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

void GpsLink::begin(HardwareSerial &port, int rxPin, uint32_t baud, size_t rxBufferSize) {
  _port = &port;
  _port->setRxBufferSize(rxBufferSize);                 // must precede begin()
  _port->begin(baud, SERIAL_8N1, rxPin, -1);            // RX only
}

void GpsLink::update() {
  if (!_port) return;
  int n = _port->available();
  while (n-- > 0) {
    const int c = _port->read();
    if (c < 0) break;
    _stats.bytes++;
    _lastByteMs = millis();
    _gotByte = true;
    if (_echo) _echo->write((uint8_t)c);
    if (handleUbxByte((uint8_t)c)) continue;
    handleByte((char)c);
  }
}

// UBX frame: B5 62 <class> <id> <len lo> <len hi> <payload> <ck_a> <ck_b>, Fletcher-8 checksum over
// class..payload. Only starts between NMEA sentences; inside a frame every byte (also '$') is payload.
bool GpsLink::handleUbxByte(uint8_t c) {
  switch (_ubxState) {
    case 0:
      if (c != 0xB5 || _inSentence) return false;
      _ubxState = 1;
      return true;
    case 1:
      if (c != 0x62) { _ubxState = 0; return false; }
      _ubxState = 2;
      _ubxPos = 0;
      _ubxCkA = _ubxCkB = 0;
      return true;
    case 2:                                             // class, id, length
      _ubxHdr[_ubxPos++] = c;
      _ubxCkA += c; _ubxCkB += _ubxCkA;
      if (_ubxPos == 4) {
        _ubxLen = _ubxHdr[2] | (_ubxHdr[3] << 8);
        if (_ubxLen > sizeof(_ubxBuild.payload)) { _stats.ubxBad++; _ubxState = 0; return true; }
        _ubxPos = 0;
        _ubxState = _ubxLen ? 3 : 4;
      }
      return true;
    case 3:                                             // payload
      _ubxBuild.payload[_ubxPos++] = c;
      _ubxCkA += c; _ubxCkB += _ubxCkA;
      if (_ubxPos == _ubxLen) _ubxState = 4;
      return true;
    case 4:
      _ubxRxA = c;
      _ubxState = 5;
      return true;
    default:                                            // 5: second checksum byte
      _ubxState = 0;
      if (_ubxRxA != _ubxCkA || c != _ubxCkB) { _stats.ubxBad++; return true; }
      _ubx.cls = _ubxHdr[0];
      _ubx.id = _ubxHdr[1];
      _ubx.len = _ubxLen;
      memcpy(_ubx.payload, _ubxBuild.payload, _ubxLen);
      _ubx.atMs = millis();
      _ubx.seq++;
      _stats.ubxFrames++;
      if (_ubxHandler) _ubxHandler(_ubx.cls, _ubx.id, _ubx.payload, _ubx.len, _ubxCtx);
      return true;
  }
}

const char *GpsLink::sendUbxOnce(int txPin, uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  if (!_port) return "GPS UART not started";
  if (txPin < 0 || len > 256 || (len && !payload)) return "bad arguments";
  const gpio_num_t pin = (gpio_num_t)txPin;

  // 1. Idle check: the GPS RX line is held HIGH by the board's pull-up (R5). If it reads LOW even so
  //    (against our weak internal pull-down), something else drives it: send nothing.
  esp_rom_gpio_pad_select_gpio(txPin);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  gpio_pullup_dis(pin);
  gpio_pulldown_en(pin);
  delayMicroseconds(500);
  const int level = gpio_get_level(pin);
  gpio_pulldown_dis(pin);
  if (level != 1) return "pin reads LOW with the pull-down on - not an idle GPS RX line, nothing sent";

  uint8_t f[8 + 256];
  f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = len & 0xFF; f[5] = len >> 8;
  if (len) memcpy(f + 6, payload, len);
  uint8_t a = 0, b = 0;
  for (uint16_t i = 2; i < 6 + len; i++) { a += f[i]; b += a; }
  f[6 + len] = a; f[7 + len] = b;

  // 2. Open-drain (only ever pulls LOW; HIGH comes from the pull-up), weakest drive, level HIGH before
  //    the output is enabled; then UART1 TX (GPS_UART = Serial1) is routed to the pin. The Arduino
  //    UART keeps TX unattached (-1): this bypasses its pin bookkeeping on purpose.
  gpio_set_level(pin, 1);
  gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_0);
  esp_rom_gpio_connect_out_signal(txPin, U1TXD_OUT_IDX, false, false);
  _port->write(f, 8 + len);
  _port->flush();                                       // until the last stop bit is out

  // 3. Release: plain GPIO (level HIGH, open-drain = released), then input only.
  esp_rom_gpio_connect_out_signal(txPin, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  return "";
}

void GpsLink::handleByte(char c) {
  if (c == '$') {                                       // start of a sentence (resync point)
    if (_inSentence && _len > 0) _stats.truncated++;    // the previous one never ended
    _inSentence = true;
    _overflow = false;
    _len = 0;
  }
  if (!_inSentence) return;                             // noise before the first '$'

  if (c == '\r') return;
  if (c == '\n') {
    finishLine();
    _inSentence = false;
    return;
  }
  if (_len < kLineMax) {
    _line[_len++] = c;
  } else if (!_overflow) {
    _overflow = true;
    _stats.overflows++;
  }
}

void GpsLink::finishLine() {
  if (_overflow || _len == 0) return;
  _line[_len] = '\0';

  if (!checksumValid(_line)) {
    _stats.badChecksum++;
    return;
  }
  _stats.sentences++;
  _lastSentenceMs = millis();
  // The u-blox receiver prints "$GNTXT,01,01,02,u-blox AG - www.u-blox.com*4E" when it powers
  // up. Seen after our own start-up it means the module rebooted (supply dip, loose wiring):
  // it loses its fix and must reacquire. Found in the 2026-09-28 outdoor log.
  if (_len > 6 && !strncmp(_line + 3, "TXT", 3) && strstr(_line, "u-blox AG")) {
    _stats.banners++;
    if ((int32_t)(_expectRestartUntil - millis()) > 0) {   // a reset NAV-1 ordered (GpsConfig)
      _expectRestartUntil = 0;
      _stats.commandedRestarts++;
    } else if (millis() > 5000) {
      _stats.receiverRestarts++;
      _stats.lastRestartMs = millis();
    }
  }
  _gotSentence = true;
  memcpy(_last, _line, _len + 1);
  if (_handler) _handler(_line, _handlerCtx);
}

// "$<body>*hh": hh = XOR of every byte of <body>, as two hex digits.
bool GpsLink::checksumValid(const char *s) {
  if (s[0] != '$') return false;
  uint8_t sum = 0;
  const char *p = s + 1;
  for (; *p && *p != '*'; ++p) sum ^= (uint8_t)*p;
  if (*p != '*' || !isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2]) || p[3] != '\0') return false;
  const char hex[3] = { p[1], p[2], '\0' };
  return sum == (uint8_t)strtoul(hex, nullptr, 16);
}

bool GpsLink::linkUp(uint32_t timeoutMs) const {
  return _gotByte && (millis() - _lastByteMs) < timeoutMs;
}

uint32_t GpsLink::msSinceLastByte() const {
  return _gotByte ? millis() - _lastByteMs : UINT32_MAX;
}

uint32_t GpsLink::msSinceLastSentence() const {
  return _gotSentence ? millis() - _lastSentenceMs : UINT32_MAX;
}
