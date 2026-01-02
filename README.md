# ESP32 ArtNet → DMX (GPIO21)

ArtNet-zu-DMX Konverter auf Basis eines **ESP32 Dev Kit V2** mit Webinterface,  
Config-Portal (Captive Portal), persistenten Einstellungen und Factory-Reset.

Das Projekt empfängt **ArtNet (UDP)** über WLAN/LAN und gibt **DMX512** über einen  
**SN75176 (RS485)** auf **GPIO21** aus.

---

## ✨ Features

- ArtNet Empfang (**UDP Port 6454**)
- DMX512 Ausgabe über **GPIO21**
- Modernes Webinterface (dark)
- Config-Mode mit **Access Point + Captive Portal**
- WLAN-Scan mit Signalqualität
- DHCP oder statische IP
- **DMX Universe frei einstellbar**
- Statusanzeige: ArtNet aktiv / DMX aktiv
- Persistente Speicherung (EEPROM)
- **Factory Reset**
  - Webinterface
  - Serial (`reset`)
  - **BOOT-Taste 5 Sekunden halten (laufend)**

---

## 🧩 Hardware

### Getestet mit
- **ESP32 Dev Kit V2**
- **SN75176** (RS485 / DMX)

### DMX Anschluss (SN75176 DIP-8)

| Pin | Funktion | Anschluss |
|----|---------|----------|
| VCC | +5 V | 5 V |
| GND | Masse | GND |
| DI | Daten IN | **GPIO21** |
| DE | Driver Enable | **5 V** |
| /RE | Receiver Enable | **5 V** |
| RO | Daten OUT | nicht belegt |
| A / B | DMX | DMX-Leitung |

**Hinweise**
- DE & /RE fest auf 5 V → reiner Sendebetrieb  
- DMX-GND und ESP-GND verbinden  
- 120 Ω Terminierung nur am Leitungsende  
- Falls kein Licht: **A/B tauschen**

---

## 🧰 Software / Versionen

### Arduino
- **Arduino IDE ≥ 2.x**

### ESP32 Board Package
- **esp32 by Espressif Systems**
- **Getestet mit:** `ESP32 Core 3.3.5`
- Board: **ESP32 Dev Module**

### Libraries

| Library | Version |
|------|--------|
| **esp_dmx** | **4.1.0** |
| **ArtnetWifi** | 1.6.1 |

---

## 🚀 Betriebsmodi

### Normal-Mode
- ESP verbindet sich mit dem konfigurierten WLAN
- ArtNet → DMX aktiv
- Webinterface über die vergebene IP erreichbar

### Config-Mode (AP + Captive Portal)

```
SSID:     ESP-Artnet
Passwort: ArtnetDMX512
IP:       http://192.168.1.4
```

---

## 🎛 ArtNet Einstellungen

- **Protokoll:** ArtNet
- **Port:** UDP **6454**
- **Universe:** wie im Webinterface

---

## ♻ Factory Reset

- Webinterface: „Werkseinstellungen“
- Serial Monitor:
```
reset
```
- BOOT-Taste: **5 Sekunden halten**

---

## ℹ️ Kurzinfo

```
ESP32 Core:  3.3.5
esp_dmx:     4.1.0
DMX TX:      GPIO21
ArtNet Port: 6454
```

---

## 📄 Lizenz

Dieses Projekt ist für private und experimentelle Nutzung gedacht.  
Keine Haftung für Schäden oder Fehlfunktionen.

---

## 🙌 Credits

- esp_dmx Library
- ArtnetWifi Library
- Espressif ESP32 Framework
