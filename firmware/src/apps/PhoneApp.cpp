// =============================================================================
//  Phone app: open NAV-1 on the iPhone - QR code + address of the
//  web page (WebService). With the NAV-1 hotspot on (Settings > Hotspot) the QR
//  code first joins the hotspot, then opens the page. Hotspot and Bluetooth state
//  shown at the bottom. Product UI only.
// =============================================================================
#include "App.h"

#include <Arduino.h>
#include "../services/WebService.h"
#include "../services/WifiService.h"
#include "../services/BleService.h"
#include "../services/Settings.h"

namespace {

struct Ui {
  lv_obj_t *qrCard = nullptr, *qr = nullptr, *qrHint = nullptr;
  lv_obj_t *info = nullptr, *ble = nullptr, *ap = nullptr;
  String shownQr, shownInfo;
};
Ui s_ui;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  // QR code on a white card (needs contrast to scan)
  s_ui.qrCard = lv_obj_create(content);
  lv_obj_remove_style_all(s_ui.qrCard);
  lv_obj_set_pos(s_ui.qrCard, 90, 20);
  lv_obj_set_size(s_ui.qrCard, 300, 300);
  lv_obj_set_style_bg_color(s_ui.qrCard, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_ui.qrCard, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_ui.qrCard, 16, 0);
  lv_obj_set_scrollable(s_ui.qrCard, false);
  s_ui.qr = lv_qrcode_create(s_ui.qrCard);
  lv_qrcode_set_size(s_ui.qr, 250);
  lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
  lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
  lv_obj_center(s_ui.qr);
  s_ui.qrHint = label(s_ui.qrCard, &lv_font_montserrat_20, lv_color_hex(0x333333));
  lv_obj_set_width(s_ui.qrHint, 260);
  lv_obj_set_style_text_align(s_ui.qrHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(s_ui.qrHint);

  lv_obj_t *right = Apps::card(content, 12, 340, 456, 340);
  lv_obj_t *t = label(right, &lv_font_montserrat_28, lv_color_white(), LV_SYMBOL_CALL "  NAV-1 on iPhone");
  lv_obj_set_pos(t, 4, 4);
  s_ui.info = label(right, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.info, 4, 56);
  lv_obj_set_width(s_ui.info, 430);
  s_ui.ap = label(right, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB));
  lv_obj_align(s_ui.ap, LV_ALIGN_BOTTOM_LEFT, 4, -24);
  s_ui.ble = label(right, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB));
  lv_obj_align(s_ui.ble, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

void update() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  // What the QR code shows: hotspot on -> join its Wi-Fi first, then (phone on it) the page;
  // otherwise the page on the shared Wi-Fi.
  String qr, hint, info;
  const String url = WebService::url();
  if (WifiService::hotspotOn()) {
    const String ap = WebService::hotspotUrl();
    if (WifiService::hotspotClients() == 0) {
      qr = "WIFI:T:WPA;S:" + WifiService::hotspotSsid() + ";P:" + WifiService::hotspotPass() + ";;";
      info = "1. Scan the code with the iPhone\n    camera to join Wi-Fi '" + WifiService::hotspotSsid() +
             "'\n    (password " + WifiService::hotspotPass() + ")\n2. The code then changes: scan it\n    again, or open " + ap;
    } else {
      qr = ap;
      info = "Phone connected to the hotspot.\n\nScan the code, or open in Safari:\n    " + ap + "\n\nShare > Add to Home Screen";
    }
  } else if (url.length()) {
    qr = url;
    info = "1. Connect the iPhone to the Wi-Fi\n    '" + WifiService::ssid() + "'\n2. Scan the code with the camera,\n    or open in Safari:\n    " +
           url + "\n    " + WebService::localUrl() + "\n3. Share > Add to Home Screen";
  } else {
    hint = "Not on Wi-Fi";
    info = "Connect NAV-1 to a Wi-Fi network\n(Settings > Wi-Fi), or switch on the\nNAV-1 hotspot (Settings > Hotspot)\nfor a direct link to the iPhone.";
  }
  if (qr != s_ui.shownQr) {
    s_ui.shownQr = qr;
    if (qr.length()) lv_qrcode_update(s_ui.qr, qr.c_str(), qr.length());
    lv_obj_set_hidden(s_ui.qr, !qr.length());
    lv_label_set_text(s_ui.qrHint, hint.c_str());
  }
  if (info != s_ui.shownInfo) { s_ui.shownInfo = info; lv_label_set_text(s_ui.info, info.c_str()); }
  Apps::setText(s_ui.ap, WifiService::hotspotOn() ? LV_SYMBOL_SHUFFLE "  NAV-1 hotspot on  (Settings > Hotspot)"
                                                  : LV_SYMBOL_SHUFFLE "  NAV-1 hotspot off  (Settings > Hotspot)");

  char b[80];
  if (!BleService::enabled()) snprintf(b, sizeof(b), LV_SYMBOL_BLUETOOTH "  Bluetooth off");
  else snprintf(b, sizeof(b), LV_SYMBOL_BLUETOOTH "  Bluetooth: %s", BleService::connectedCount() ? "phone connected" : "visible");
  Apps::setText(s_ui.ble, b);
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl PHONE_APP = { "Phone", create, update, destroy };
