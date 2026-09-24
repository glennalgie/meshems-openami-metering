#ifdef ENABLE_ETHERNET
#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <esp_mac.h>
#include <core/pins.h>
#include <comms/ethernet.h>

// ---------------------------------------------------------------------------
// MAC address
// ---------------------------------------------------------------------------
// Derived at runtime from the ESP32 eFuse MAC so every board is unique on the
// segment. This replaces a hardcoded DE:AD:BE:EF:FE:ED, which is the default
// shipped in most W5500 examples — put two such boards on one LAN and the DHCP
// server hands out one lease for both, producing failures that look random.
// A stale lease for that address on the switch can also break a single board.
//
// Scheme matches the EVPE deployment (EVPE/src/platform/network/ethernet.cpp:107)
// so both product lines allocate addresses the same way:
//   02      locally administered (bit 1 of the MSB set, not a vendor OUI)
//   42      NESL marker
//   00      reserved
//   low 24 bits of the eFuse MAC — unique per chip
static byte eth_mac[6] = { 0x02, 0x42, 0x00, 0x00, 0x00, 0x00 };

static bool ethernet_ok = false;

static void derive_unique_mac() {
    // Read the factory MAC in correct byte order. Do NOT use
    // ESP.getEfuseMac(): it returns the 48 bits REVERSED, so its low three
    // bytes are the Espressif OUI (e.g. 24:58:7C), which is identical on every
    // board from that batch. EVPE's version takes those low bytes and therefore
    // produces the same "unique" MAC on every unit — a bug this avoids.
    // esp_read_mac() hands back 24:58:7C:DA:7A:18 in printed order, so bytes
    // [3..5] are the genuinely per-chip portion.
    uint8_t chip_mac[6] = {0};
    esp_read_mac(chip_mac, ESP_MAC_WIFI_STA);

    eth_mac[0] = 0x02;          // locally administered, not a vendor OUI
    eth_mac[1] = 0x42;          // NESL marker
    eth_mac[2] = 0x00;          // reserved
    eth_mac[3] = chip_mac[3];   // per-chip
    eth_mac[4] = chip_mac[4];
    eth_mac[5] = chip_mac[5];

    Serial.printf("ethernet: MAC %02X:%02X:%02X:%02X:%02X:%02X (chip %02X:%02X:%02X:%02X:%02X:%02X)\n",
                  eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5],
                  chip_mac[0], chip_mac[1], chip_mac[2],
                  chip_mac[3], chip_mac[4], chip_mac[5]);
}

bool ethernet_connected() {
    return ethernet_ok
        && Ethernet.linkStatus() == LinkON
        && Ethernet.localIP() != IPAddress(0, 0, 0, 0);
}

String get_eth_ip() {
    if (ethernet_connected()) {
        return Ethernet.localIP().toString();
    }
    return "Not Connected";
}

// Print the full lease. DNS matters as much as the address: the MQTT broker is
// dialled by hostname, so a lease with no DNS server still fails every connect.
static void print_net_config() {
    Serial.printf("ethernet: link    %s\n",
                  Ethernet.linkStatus() == LinkON ? "UP" : "DOWN/UNKNOWN");
    Serial.printf("ethernet: IP      %s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("ethernet: subnet  %s\n", Ethernet.subnetMask().toString().c_str());
    Serial.printf("ethernet: gateway %s\n", Ethernet.gatewayIP().toString().c_str());
    Serial.printf("ethernet: DNS     %s\n", Ethernet.dnsServerIP().toString().c_str());
    if (Ethernet.dnsServerIP() == IPAddress(0, 0, 0, 0)) {
        Serial.println("ethernet: WARNING - no DNS server in lease; hostname lookups WILL fail.");
    }
}

bool setup_ethernet() {
    ethernet_ok = false;

    // W5500 hardware reset. Timing matches the EVPE deployment
    // (EVPE/src/platform/network/ethernet.cpp:89), which runs this same PHY in
    // the field. The datasheet only asks for 500 us low, but the W5500 needs a
    // generous settle after release before it answers on SPI.
    pinMode(ETH_RST, OUTPUT);
    digitalWrite(ETH_RST, LOW);
    delay(100);
    digitalWrite(ETH_RST, HIGH);
    delay(500);

    // Shared SPI bus (SD / OLED / ATM90E32). Every other chip select must
    // already be deasserted — see spi_cs_init() in main.cpp.
    SPI.begin(ETH_CLK, ETH_MISO, ETH_MOSI, -1);

    derive_unique_mac();

    Ethernet.init(ETH_CS);

    // IMPORTANT ORDERING: hardwareStatus() reports a chip type that is only
    // detected inside W5100.init(), which Ethernet.begin() calls. Querying it
    // BEFORE begin() always returns EthernetNoHardware regardless of what is
    // actually wired up. So: attempt DHCP first, and only interrogate the
    // hardware when that fails. EVPE does the same, for the same reason.
    Serial.println("ethernet: DHCP starting...");
    if (Ethernet.begin(eth_mac) != 0) {
        Serial.printf("ethernet: W5500 detected (hardwareStatus=%d)\n",
                      (int)Ethernet.hardwareStatus());
        Serial.println("ethernet: DHCP lease acquired");
        print_net_config();
        ethernet_ok = true;
        return true;
    }

    // ---- DHCP failed: now the chip type is valid, so classify the fault ----
    Serial.println("ethernet: DHCP failed - no lease returned.");

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("ethernet: FAILED - W5500 not responding on SPI.");
        Serial.printf("ethernet:   check ETH_CS=%d / ETH_RST=%d wiring, and that every\n",
                      ETH_CS, ETH_RST);
        Serial.println("ethernet:   other chip select on the shared bus is deasserted.");
        return false;
    }
    Serial.printf("ethernet:   W5500 IS present (hardwareStatus=%d), so this is a\n",
                  (int)Ethernet.hardwareStatus());
    Serial.println("ethernet:   network problem, not a wiring one.");

    if (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("ethernet:   link is DOWN - fix the cable before anything else.");
        return false;
    }

#ifdef ETH_STATIC_FALLBACK
    // Opt-in static fallback, mirroring EVPE's behaviour. Deliberately NOT the
    // default: falling back to an address on the wrong subnet is worse than
    // failing loudly, because it looks like it worked. Enable and set all four
    // values in platformio.ini for a known network.
    Serial.println("ethernet: falling back to static IP configuration...");
    IPAddress ip(ETH_STATIC_IP);
    IPAddress dns(ETH_STATIC_DNS);
    IPAddress gw(ETH_STATIC_GATEWAY);
    IPAddress sn(ETH_STATIC_SUBNET);
    Ethernet.begin(eth_mac, ip, dns, gw, sn);
    if (Ethernet.localIP()[0] == 0) {
        Serial.println("ethernet: FAILED - static configuration did not take either.");
        return false;
    }
    Serial.println("ethernet: static IP configured");
    print_net_config();
    ethernet_ok = true;
    return true;
#else
    Serial.println("ethernet:   no IP and no DNS - outbound connections will all fail.");
    Serial.println("ethernet:   (build with -DETH_STATIC_FALLBACK to use a fixed address)");
    return false;
#endif
}

void loop_ethernet() {
    // Only maintain a link that actually came up. Ethernet.maintain() performs
    // blocking SPI transactions to renew the DHCP lease; calling it with no
    // lease or no cable stalls the main loop on every pass, which starves the
    // meter poll and MQTT. EVPE guards this the same way
    // (EVPE/src/platform/network/ethernet.cpp:181).
    if (!ethernet_ok) {
        return;
    }
    Ethernet.maintain();
}

#endif // ENABLE_ETHERNET
