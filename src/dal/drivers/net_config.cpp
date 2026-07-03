// NetConfig + config console (USB-serial + optional password-gated TCP).
//
// Compiled only under NOCT_DMX_ETHERNET (the AtomS3-PoE env). The console is
// deliberately non-disruptive:
//   * the USB side does NOTHING unless a host is connected ((bool)Serial), so
//     it never writes into a disconnected CDC and blocks the loop;
//   * a short Tx timeout means even a connected-but-stalled host can't stall
//     the broadcast;
//   * USB runs as TinyUSB CDC (ARDUINO_USB_MODE=0) so opening the port doesn't
//     hardware-reset the chip - connecting a laptop is invisible.
//
// Network console (TCP port 2323): the SAME command set as USB, gated behind a
// plaintext password held in NVS. It stays OFF until a password is set from the
// USB console with `netpass <pw>`. Plaintext over raw TCP is not encrypted - run
// the Director on a trusted / control VLAN. One session at a time; a wrong
// password disconnects + briefly locks out; an idle session auto-drops.

#include "net_config.h"

#if defined(NOCT_DMX_ETHERNET)

#include <Arduino.h>
#include <Preferences.h>
#include <Ethernet.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "ethernet_dmx_adapter.h"
#include "espnow_broadcast_driver.h"
#include "repeater_census.h"     // headless-repeater online count
#include "modes/persistence.h"   // Director source_id (DirectorID) get/set

namespace nocturnation {
namespace dal {

namespace {

const char* kNvs = "noctnet";

constexpr uint16_t kConsolePort = 2323;
constexpr uint32_t kIdleMs      = 300000;   // 5 min: drop a forgotten session
constexpr uint32_t kLockoutMs   = 3000;     // pause after a bad password

uint32_t pack(const uint8_t v[4]) {
    return (uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16) |
           (uint32_t(v[2]) <<  8) |  uint32_t(v[3]);
}
void unpack(uint32_t x, uint8_t v[4]) {
    v[0] = x >> 24; v[1] = x >> 16; v[2] = x >> 8; v[3] = x;
}

// 27 active channels per block: 23 FX channels + the raw RGB direct-control
// set (ch24-27) the dev added for stage-LD use (ch27 enable high = static
// wash straight from ch24-26, bypassing the FX engine).
const char* const kRoles[27] = {
    "Master",   "Strobe",    "Pulse R",   "Pulse G",  "Pulse B",  "Pulse Trig",
    "P Attack", "P Sustain", "P Release", "P Prob",
    "Wash A R", "Wash A G",  "Wash A B",  "Wash B R", "Wash B G", "Wash B B",
    "W Cycle",  "W Inten",   "W Attack",  "W Release","W TTL lo", "W TTL hi", "W PulseR",
    "Raw R",    "Raw G",     "Raw B",     "Raw Enable",
};

// A console session: where its I/O goes, its line buffer, and its live-view
// state. Two exist - the USB CDC and the (single) TCP client - so each keeps
// its own prompt + `mon` independently. Stream covers both Serial and
// EthernetClient (read/write/available).
struct Session {
    Stream*  io       = nullptr;
    char     line[96];
    size_t   len      = 0;
    bool     mon      = false;
    uint32_t mon_last = 0;
};

Session  s_usb;
Session  s_tcp;
Session* g_cur = nullptr;   // session currently being serviced (drives pln())

bool      s_loaded = false;
NetConfig s_cfg;

char s_pass[33] = {0};      // network-console password (empty = disabled)

// arduino-libraries/Ethernet leaves the ESP32 core's pure-virtual
// Server::begin(uint16_t) unimplemented in EthernetServer (it only declares
// begin()), which makes EthernetServer abstract on ESP32. This thin shim
// supplies the missing override (delegating to the no-arg begin) so the server
// can be instantiated.
class ConsoleServer : public EthernetServer {
public:
    explicit ConsoleServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t /*port*/ = 0) override { EthernetServer::begin(); }
};

// TCP server state. Constructor only stores the port; begin() (which touches
// the W5500) is deferred to service_tcp() once the link is up.
ConsoleServer  s_server(kConsolePort);
bool           s_server_started = false;
EthernetClient s_client;
enum class Tcp { Closed, Auth, Open };
Tcp      s_tcp_state     = Tcp::Closed;
uint32_t s_tcp_last_ms   = 0;
uint32_t s_lockout_until = 0;

void pln(const char* s) {
    g_cur->io->print(s);
    if (g_cur->mon) g_cur->io->print("\033[K");   // clear to EOL (steadies live view)
    g_cur->io->println();
}

bool has_password() { return s_pass[0] != 0; }

void load_password() {
    Preferences pr; pr.begin(kNvs, true);
    String p = pr.getString("pass", "");
    pr.end();
    std::strncpy(s_pass, p.c_str(), sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = 0;
}
void set_password(const char* p) {
    std::strncpy(s_pass, p, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = 0;
    Preferences pr; pr.begin(kNvs, false); pr.putString("pass", s_pass); pr.end();
}
void clear_password() {
    s_pass[0] = 0;
    Preferences pr; pr.begin(kNvs, false); pr.remove("pass"); pr.end();
    if (s_client) s_client.stop();
    s_tcp_state = Tcp::Closed;
}

void print_help() {
    pln("");
    pln("NocturNation DMX Director - console");
    pln("  show              one-shot status + channel values");
    pln("  mon               live status (press a key to stop)");
    pln("  channel <1|6|11>  ESP-NOW fleet channel (applies + saves now)");
    pln("  dirid [<hex>]     show/set Director ID (ch1 00-3F, ch11 40-FE; applies + saves now)");
    pln("  dhcp              use DHCP            (save + reboot to apply)");
    pln("  static <ip> <mask> <gw>              static IP (save + reboot)");
    pln("  sacn <n>          sACN universe      (save + reboot)");
    pln("  artnet <n>        Art-Net universe   (save + reboot)");
    pln("  netpass <pw|off>  set/disable the network console password (port 2323)");
    pln("  irtest [pin]      fire a visible IR burst on a GPIO (default 2; front camera)");
    pln("  save              persist config to flash");
    pln("  reboot            restart");
}

void print_status() {
    char buf[96];
    EthernetDmxAdapter& a = ethernet_dmx_adapter_instance();
    IPAddress ip = Ethernet.localIP();

    pln("");
    pln("== NocturNation DMX Director ==");
    std::snprintf(buf, sizeof(buf), "  w5500   : %s", a.hardware_present() ? "OK" : "NOT FOUND"); pln(buf);
    std::snprintf(buf, sizeof(buf), "  link    : %s", a.link_up() ? "UP" : "DOWN"); pln(buf);
    std::snprintf(buf, sizeof(buf), "  ip      : %u.%u.%u.%u  (%s)",
                  ip[0], ip[1], ip[2], ip[3], s_cfg.use_dhcp ? "dhcp" : "static"); pln(buf);
    std::snprintf(buf, sizeof(buf), "  espnow  : channel %u", s_cfg.wifi_channel); pln(buf);
    std::snprintf(buf, sizeof(buf), "  dir id  : 0x%02X (%s)",
                  (unsigned)esp_now_broadcast_driver_instance()->source_id(),
                  s_cfg.wifi_channel == 1  ? "community"
                : s_cfg.wifi_channel == 11 ? "performance"
                :                            "mac-derived"); pln(buf);

    // Headless repeaters relaying this Director's signal. Count + a line
    // each for the ones heard within the census window (uid, channel,
    // frames relayed, uptime).
    {
        RepeaterCensus& cen = repeater_census_instance();
        const uint32_t now = millis();
        std::snprintf(buf, sizeof(buf), "  repeatrs: %u online",
                      (unsigned)cen.count_online(now)); pln(buf);
        for (size_t i = 0; i < RepeaterCensus::capacity(); ++i) {
            const RepeaterCensus::Entry& e = cen.entries()[i];
            if (!e.used) continue;
            if ((now - e.last_seen_ms) > RepeaterCensus::kOnlineWindowMs) continue;
            std::snprintf(buf, sizeof(buf),
                          "    %02X%02X%02X ch%-2u relayed %lu  up %us",
                          e.uid[0], e.uid[1], e.uid[2], (unsigned)e.channel,
                          (unsigned long)e.relayed, (unsigned)e.uptime_s);
            pln(buf);
        }
    }

    std::snprintf(buf, sizeof(buf), "  sacn    : universe %u   artnet: universe %u",
                  s_cfg.sacn_universe, s_cfg.artnet_universe); pln(buf);
    std::snprintf(buf, sizeof(buf), "  frames  : %lu   bytes: %lu",
                  (unsigned long)a.parser().frame_count(),
                  (unsigned long)a.bytes_read()); pln(buf);

    uint8_t ch[27] = {0};
    a.parser().copy_dmx_channels(ch, 27);
    pln("  -- broadcast block (DMX ch 1-27) --");
    for (int i = 0; i < 27; ++i) {
        char bar[18];
        int n = ch[i] / 16;                    // 0..15 hashes
        for (int b = 0; b < n; ++b) bar[b] = '#';
        bar[n] = 0;
        std::snprintf(buf, sizeof(buf), "  %2d %-10s %3u %s", i + 1, kRoles[i], ch[i], bar);
        pln(buf);
    }
}

bool parse_ip(const char* tok, uint8_t out[4]) {
    unsigned a, b, c, d;
    if (!tok || std::sscanf(tok, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
    return true;
}

void handle(Session& s, char* line) {
    char* cmd = std::strtok(line, " \t");
    if (!cmd) return;

    if (!std::strcmp(cmd, "show") || !std::strcmp(cmd, "?")) {
        print_status();
    } else if (!std::strcmp(cmd, "help")) {
        print_help();
    } else if (!std::strcmp(cmd, "mon")) {
        s.mon = true; s.mon_last = 0;
        s.io->print("\033[2J\033[?25l");   // clear screen + hide cursor for in-place redraw
    } else if (!std::strcmp(cmd, "channel")) {
        char* t = std::strtok(nullptr, " \t");
        int n = t ? std::atoi(t) : 0;
        if (n == 1 || n == 6 || n == 11) {
            s_cfg.wifi_channel = (uint8_t)n;
            // Persist just the channel, then apply it live so the fleet move
            // is immediate (one deliberate command; a brief blip is inherent
            // to changing channels, not a side effect of connecting).
            Preferences pr; pr.begin(kNvs, false);
            pr.putUChar("chan", s_cfg.wifi_channel); pr.end();
            esp_now_broadcast_driver_instance()->stop_broadcast();
            esp_now_broadcast_driver_instance()->start_broadcast(s_cfg.wifi_channel);
            char b[48]; std::snprintf(b, sizeof(b), "channel -> %d (saved + live)", n); pln(b);
        } else {
            pln("channel must be 1, 6 or 11");
        }
    } else if (!std::strcmp(cmd, "dirid")) {
        // Director ID == the ESP-NOW source_id this Director stamps on every
        // frame; Lumes TOFU-lock to it. On ch1 it's the community-range id
        // (persistence "mst_src_id"), on ch11 the performance-range id
        // ("mst_pid"); ch6 is MAC-derived and not settable. Input is hex
        // (matching the C:nn / P:nn lock labels). Saved to NVS and applied
        // live by re-deriving through start_broadcast - same path the
        // channel command uses. Changing it makes currently-locked Lumes
        // drop and re-lock to the new id.
        char* t = std::strtok(nullptr, " \t");
        const uint8_t chn = s_cfg.wifi_channel;
        if (!t) {
            char b[80];
            std::snprintf(b, sizeof(b), "dir id = 0x%02X  (ch%u: %s)",
                          (unsigned)esp_now_broadcast_driver_instance()->source_id(),
                          chn,
                          chn == 1  ? "community 00-3F"
                        : chn == 11 ? "performance 40-FE"
                        :             "mac-derived, not settable");
            pln(b);
        } else if (chn == 6) {
            pln("ch6 Director ID is MAC-derived and not settable");
        } else {
            long v = std::strtol(t, nullptr, 16);
            const bool ok = (chn == 1)  ? (v >= 0x00 && v <= 0x3F)
                                        : (v >= 0x40 && v <= 0xFE);
            if (!ok) {
                pln(chn == 1 ? "ch1 Director ID must be hex 00-3F (community)"
                             : "ch11 Director ID must be hex 40-FE (performance)");
            } else {
                if (chn == 1) modes::persistence::save_director_source_id((uint8_t)v);
                else          modes::persistence::save_director_perf_source_id((uint8_t)v);
                esp_now_broadcast_driver_instance()->stop_broadcast();
                esp_now_broadcast_driver_instance()->start_broadcast(chn);
                char b[56];
                std::snprintf(b, sizeof(b), "dir id -> 0x%02X (saved + live)", (unsigned)(uint8_t)v);
                pln(b);
            }
        }
    } else if (!std::strcmp(cmd, "dhcp")) {
        s_cfg.use_dhcp = true;
        pln("dhcp set (save + reboot to apply)");
    } else if (!std::strcmp(cmd, "static")) {
        char* a = std::strtok(nullptr, " \t");
        char* m = std::strtok(nullptr, " \t");
        char* g = std::strtok(nullptr, " \t");
        if (parse_ip(a, s_cfg.ip) && parse_ip(m, s_cfg.mask) && parse_ip(g, s_cfg.gw)) {
            s_cfg.use_dhcp = false;
            pln("static set (save + reboot to apply)");
        } else {
            pln("usage: static <ip> <mask> <gw>");
        }
    } else if (!std::strcmp(cmd, "sacn")) {
        char* t = std::strtok(nullptr, " \t");
        if (t) { s_cfg.sacn_universe = (uint16_t)std::atoi(t); pln("sacn set (save + reboot)"); }
        else   { pln("usage: sacn <universe>"); }
    } else if (!std::strcmp(cmd, "artnet")) {
        char* t = std::strtok(nullptr, " \t");
        if (t) { s_cfg.artnet_universe = (uint16_t)std::atoi(t); pln("artnet set (save + reboot)"); }
        else   { pln("usage: artnet <universe>"); }
    } else if (!std::strcmp(cmd, "netpass")) {
        char* t = std::strtok(nullptr, " \t");
        if (!t) {
            pln(has_password() ? "netpass: set (hidden) - 'netpass off' to disable"
                               : "netpass: not set (network console off)");
        } else if (!std::strcmp(t, "off") || !std::strcmp(t, "clear")) {
            clear_password();
            pln("network console disabled");
        } else {
            set_password(t);
            pln("network console password set (TCP port 2323)");
        }
    } else if (!std::strcmp(cmd, "irtest")) {
        // Band-free IR bring-up: drive a raw 38 kHz carrier straight onto a GPIO
        // with LEDC (PWM), bypassing the IR library, so you can sweep candidate
        // pins with a phone's FRONT camera (the rear has an IR-cut filter).
        // Default = the configured external pin (G2 = 2); try `irtest 1` (Grove
        // G1) or `irtest 4` (AtomS3 built-in). 15 on/off flashes; blocks the
        // loop ~2 s - a deliberate diagnostic, not a hot path.
        char* t = std::strtok(nullptr, " \t");
        uint8_t pin = t ? (uint8_t)std::atoi(t) : 2;
        char b[72]; std::snprintf(b, sizeof(b), "IR test: 38kHz carrier x15 on GPIO %u (front camera)", pin); pln(b);
        ledcSetup(0, 38000, 8);        // LEDC ch0, 38 kHz, 8-bit
        ledcAttachPin(pin, 0);
        for (int i = 0; i < 15; ++i) {
            ledcWrite(0, 128);         // ~50% duty = carrier on
            delay(60);
            ledcWrite(0, 0);           // off
            delay(60);
        }
        ledcDetachPin(pin);
        pln("IR test done");
    } else if (!std::strcmp(cmd, "save")) {
        net_config_save(s_cfg);
        pln("saved");
    } else if (!std::strcmp(cmd, "reboot")) {
        pln("rebooting...");
        delay(100);
        ESP.restart();
    } else {
        pln("unknown - type 'help'");
    }
}

// Shared input + live-view pump for one session over its stream. Used for both
// the USB CDC and the authenticated TCP client.
void service_session(Session& s, Stream& io) {
    g_cur = &s;
    s.io  = &io;

    // Live `mon` view: reprint status ~1 Hz until any key is pressed.
    if (s.mon) {
        if (io.available()) {
            s.mon = false;
            while (io.available()) io.read();
            io.print("\033[?25h\r\n> ");   // restore cursor, fresh prompt
        } else {
            const uint32_t now = millis();
            if (now - s.mon_last >= 1000) {
                s.mon_last = now;
                io.print("\033[H");         // home: redraw the dashboard in place
                print_status();
                pln("(press a key to stop)");
                io.print("\033[J");         // clear anything left below
            }
            return;
        }
    }

    while (io.available()) {
        int c = io.read();
        if (c == '\n' || c == '\r') {
            if (s.len > 0) {
                s.line[s.len] = 0;
                io.println();
                handle(s, s.line);
                s.len = 0;
                io.print("> ");
            }
        } else if (c >= 0x20 && c < 0x7F && s.len < sizeof(s.line) - 1) {
            s.line[s.len++] = (char)c;   // printable only (drops telnet IAC / control)
            io.write((char)c);           // local echo
        }
    }
}

void service_usb() {
    static bool was_connected = false;

    // Do nothing unless a host is connected - never write into a disconnected
    // CDC (which could block the loop and stall the broadcast).
    if (!(bool)Serial) { was_connected = false; s_usb.mon = false; return; }

    if (!was_connected) {
        was_connected = true;
        s_usb.io = &Serial; s_usb.len = 0; s_usb.mon = false;
        g_cur = &s_usb;
        print_help();
        print_status();
        Serial.print("> ");
    }
    service_session(s_usb, Serial);
}

void close_tcp() {
    if (s_client) s_client.stop();
    s_tcp_state = Tcp::Closed;
    s_tcp.mon = false; s_tcp.len = 0;
}

void service_tcp() {
    if (!has_password()) return;   // network console off until a password is set

    if (!s_server_started) {
        if (!ethernet_dmx_adapter_instance().link_up()) return;
        s_server.begin();
        s_server_started = true;
    }

    // Tear down a dropped or idle session.
    if (s_tcp_state != Tcp::Closed) {
        if (!s_client.connected()) {
            close_tcp();
        } else if (millis() - s_tcp_last_ms > kIdleMs) {
            s_client.println();
            s_client.println("idle timeout - bye");
            close_tcp();
        }
    }

    if (s_tcp_state == Tcp::Closed) {
        EthernetClient nc = s_server.accept();
        if (nc) {
            if (millis() < s_lockout_until) {
                nc.println("locked out, try again shortly");
                nc.stop();
            } else {
                s_client      = nc;
                s_tcp_state   = Tcp::Auth;
                s_tcp.len     = 0; s_tcp.mon = false; s_tcp.io = &s_client;
                s_tcp_last_ms = millis();
                s_client.print("NocturNation DMX Director\r\npassword: ");
            }
        }
        return;
    }

    if (s_tcp_state == Tcp::Auth) {
        while (s_client.available()) {
            s_tcp_last_ms = millis();
            int c = s_client.read();
            if (c == '\n' || c == '\r') {
                if (s_tcp.len == 0) continue;   // ignore the CRLF second byte
                s_tcp.line[s_tcp.len] = 0;
                const bool ok = (std::strcmp(s_tcp.line, s_pass) == 0);
                s_tcp.len = 0;
                if (ok) {
                    s_tcp_state = Tcp::Open;
                    g_cur = &s_tcp;
                    s_client.print("\r\n");
                    print_help();
                    print_status();
                    s_client.print("> ");
                } else {
                    s_client.println("\r\ndenied");
                    s_client.stop();
                    s_tcp_state     = Tcp::Closed;
                    s_lockout_until = millis() + kLockoutMs;   // slow brute force
                }
                return;
            } else if (c >= 0x20 && c < 0x7F && s_tcp.len < sizeof(s_tcp.line) - 1) {
                s_tcp.line[s_tcp.len++] = (char)c;   // no echo of the password
            }
        }
        return;
    }

    // Tcp::Open
    if (s_client.available()) s_tcp_last_ms = millis();
    service_session(s_tcp, s_client);
}

}  // namespace

NetConfig net_config_load() {
    NetConfig c;
    Preferences pr;
    pr.begin(kNvs, true);
    c.use_dhcp        = pr.getBool  ("dhcp",   c.use_dhcp);
    unpack(pr.getUInt("ip",   pack(c.ip)),   c.ip);
    unpack(pr.getUInt("mask", pack(c.mask)), c.mask);
    unpack(pr.getUInt("gw",   pack(c.gw)),   c.gw);
    c.sacn_universe   = pr.getUShort("sacn",   c.sacn_universe);
    c.artnet_universe = pr.getUShort("artnet", c.artnet_universe);
    c.wifi_channel    = pr.getUChar ("chan",   c.wifi_channel);
    pr.end();
    return c;
}

void net_config_save(const NetConfig& c) {
    Preferences pr;
    pr.begin(kNvs, false);
    pr.putBool  ("dhcp",   c.use_dhcp);
    pr.putUInt  ("ip",   pack(c.ip));
    pr.putUInt  ("mask", pack(c.mask));
    pr.putUInt  ("gw",   pack(c.gw));
    pr.putUShort("sacn",   c.sacn_universe);
    pr.putUShort("artnet", c.artnet_universe);
    pr.putUChar ("chan",   c.wifi_channel);
    pr.end();
}

void net_console_poll() {
    if (!s_loaded) {
        s_cfg = net_config_load();
        load_password();
        Serial.setTxTimeoutMs(20);   // never block the loop on a stalled host
        s_loaded = true;
    }
    service_usb();   // gated on (bool)Serial internally
    service_tcp();   // runs regardless of USB - the whole point
}

}  // namespace dal
}  // namespace nocturnation

#endif  // NOCT_DMX_ETHERNET
