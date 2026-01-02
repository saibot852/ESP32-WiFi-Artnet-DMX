1. Überblick

Dieses Projekt ist ein ArtNet-zu-DMX-Konverter auf Basis eines ESP32 Dev Kit V2.
Der ESP32 empfängt ArtNet (UDP) über WLAN/LAN und gibt die Daten als DMX512 über einen SN75176 (RS485) aus.

2. Software-Voraussetzungen
Arduino IDE

Arduino IDE ≥ 2.x empfohlen

ESP32 Board Package

esp32 by Espressif Systems

Getestet mit:
ESP32 Core Version 3.3.5

Board-Auswahl:

ESP32 Dev Module
(ESP32 Dev Kit V2 kompatibel)

⚠️ Hinweis:
Andere Core-Versionen (älter oder neuer) können API-Änderungen enthalten.
Der Code ist explizit auf ESP32 Core 3.3.5 abgestimmt.

Verwendete Libraries
Library	Version
esp_dmx	4.1.0
ArtnetWifi	1.6.1
WiFi / WebServer	aus ESP32 Core
EEPROM	aus ESP32 Core
3. Hardware-Anschluss
ESP32 → SN75176 (DIP-8)
SN75176	Funktion	Anschluss
VCC	+5 V	5 V
GND	Masse	GND
DI	Daten IN	GPIO21
DE	Driver Enable	5 V
/RE	Receiver Enable	5 V
RO	Daten OUT	nicht belegt
A / B	DMX	DMX-Leitung
Hinweise

DE & /RE dauerhaft auf 5 V → reiner DMX-Sendebetrieb

DMX-GND und ESP-GND verbinden

120 Ω Terminierung nur am Leitungsende

Falls kein Licht: A/B tauschen

4. Betriebsmodi
Normal-Mode

ESP verbindet sich mit dem konfigurierten WLAN

ArtNet → DMX aktiv

Webinterface über die vergebene IP erreichbar

Config-Mode (AP + Captive Portal)

Der ESP startet im Config-Mode, wenn:

kein WLAN verbunden werden kann

die BOOT-Taste beim Start gedrückt wird

„Persistenter Config-Mode“ aktiviert ist

nach einem Factory Reset

Zugangsdaten:

SSID: ESP-Artnet

Passwort: ArtnetDMX512

IP / Web: http://192.168.1.4

📌 Das Captive Portal öffnet sich meist automatisch, sonst IP manuell im Browser öffnen.

5. Webinterface
Statusanzeige

Aktuelle IP

DMX Universe

Modus: NORMAL / CONFIG

ArtNet LED: grün = Daten empfangen

DMX LED: grün = DMX wird gesendet

WLAN konfigurieren

Scan klicken

Netzwerk auswählen (gut / mittel / schlecht)

WLAN-Passwort eingeben

IP-Einstellungen

DHCP: automatische IP (empfohlen)

Static: IP / Gateway / Subnet manuell
(IPv4-Eingaben werden geprüft)

DMX Universe

Feld „DMX Universe“

Standard: 0

Muss exakt mit dem ArtNet-Sender übereinstimmen

Speichern

„Speichern & Neustart“

Meldung: Erfolgreich gespeichert. Reboot now!

Browser versucht automatisch wieder zu verbinden

6. ArtNet-Sender konfigurieren

Protokoll: ArtNet

Port: UDP 6454

Ziel-IP: IP des ESP32

Universe: wie im Webinterface

Kompatibel mit:

Node-RED

DMX-Software

Lichtpulten

7. Factory Reset (Werkseinstellungen)
Webinterface

Button „Werkseinstellungen“

Serial Monitor

Baudrate 115200

reset

BOOT-Taste

BOOT im laufenden Betrieb 5 Sekunden halten

Nach Reset:

Config-Mode aktiv

IP: 192.168.1.4

SSID: ESP-Artnet

Passwort: ArtnetDMX512

DMX Universe: 0

8. Fehlersuche

Kein Licht

Universe prüfen (0 / 1 testen)

A/B vertauscht

DMX-GND fehlt

Falsche Ziel-IP

Webinterface nicht erreichbar

BOOT beim Start gedrückt halten

oder Serial reset

9. Kurzinfo
ESP32 Core:  3.3.5
esp_dmx:     4.1.0

Config-Mode:
IP        192.168.1.4
SSID      ESP-Artnet
Passwort  ArtnetDMX512

ArtNet:
UDP Port  6454
DMX TX    GPIO21