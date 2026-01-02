/* ===================== KURZANLEITUNG / QUICK START =====================

ESP32 ArtNet -> DMX (GPIO21) mit Config-Portal

1) DMX Hardware (SN75176 / RS485)
   - DI  <- GPIO21 (DMX TX)
   - DE  -> 5V  (Sender aktiv)
   - /RE -> 5V  (Receiver aus)
   - RO  -> offen
   - VCC -> 5V, GND -> GND
   - A/B -> DMX Leitung (ggf. A/B tauschen wenn nichts reagiert)
   - DMX-GND und ESP-GND verbinden, 120Ω nur am Leitungsende.

2) ArtNet
   - Protokoll: ArtNet
   - UDP Port: 6454
   - Universe: im Webinterface "DMX Universe" (Standard: 0)
   - Ziel-IP: IP des ESP32 (siehe Statuszeile im Webinterface / Serial)

3) CONFIG-MODE (AP + Captive Portal)
   - Startet automatisch wenn WLAN nicht verbindet, nach Factory Reset,
     bei aktivem "Persistenter Config-Mode" oder wenn BOOT beim Start gehalten wird.
   - SSID:     ESP-Artnet
   - Passwort: ArtnetDMX512
   - IP/Web:   http://192.168.1.4/

4) Webinterface
   - WLAN: Scan -> SSID wählen -> Passwort
   - Netzwerk: DHCP oder Static (bei Static IP/Gateway/Subnet eingeben)
   - DMX Universe setzen
   - "Speichern & Neustart" (Browser versucht automatisch zu reconnecten)

5) Factory Reset (Werkseinstellungen)
   - UI: Button "Werkseinstellungen"
   - Serial Monitor (115200): "reset" + Enter
   - BOOT Taste im Betrieb 5 Sekunden halten (wenn aktiviert)
   -> Danach startet Gerät im Config-Mode (AP/Captive Portal).

Tipp bei "kein Licht":
   - Universe 0/1 prüfen, A/B tauschen, GND prüfen.

========================================================================= */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

#include <esp_dmx.h>
#include "ArtnetWifi.h"

/* ================= DMX ================= */
static const dmx_port_t DMX_PORT = DMX_NUM_1;
static const int DMX_TX_PIN = 21;
static uint8_t dmxBuffer[DMX_PACKET_SIZE];
static uint32_t lastDmxSendMs = 0;
static const uint32_t DMX_REFRESH_MS = 25; // ~40 Hz

/* ================= ArtNet ================= */
ArtnetWifi artnet;
static uint16_t startUniverse = 0;

/* Activity tracking */
static volatile uint32_t g_artnetFramesTotal = 0;
static uint32_t g_lastArtnetMs = 0;
static uint32_t g_lastDmxMs = 0;

/* periodic serial stats */
static uint32_t g_lastStatsMs = 0;
static uint32_t g_lastFramesSnapshot = 0;

/* ================= Hardware ================= */
static const int BOOT_PIN = 0; // BOOT Button
// BOOT long-press reset
static uint32_t bootPressStartMs = 0;
static bool bootResetTriggered = false;
static const uint32_t BOOT_RESET_TIME_MS = 5000; // 5 Sekunden


/* ================= AP / DNS ================= */
static const char* AP_SSID = "ESP-Artnet";
static const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);

/* ================= EEPROM ================= */
static const uint16_t EEPROM_SIZE = 1024;
static const uint32_t CFG_MAGIC = 0xA17E7E55;

struct Config {
  uint32_t magic;

  bool forceConfig;        // persistent config-mode
  char sta_ssid[33];
  char sta_pass[65];

  char ap_pass[65];        // WPA2 requires 8..63 chars

  bool useStatic;
  uint8_t ip[4], gw[4], sn[4];

  uint16_t universe;
} cfg;

static bool configMode = false;

/* ================= Helpers ================= */
static String ipToString(const uint8_t a[4]) {
  return String(a[0]) + "." + String(a[1]) + "." + String(a[2]) + "." + String(a[3]);
}

static bool parseIPv4(const String& s, uint8_t out[4]) {
  int p1=s.indexOf('.'), p2=s.indexOf('.',p1+1), p3=s.indexOf('.',p2+1);
  if(p1<0||p2<0||p3<0) return false;
  int a=s.substring(0,p1).toInt(), b=s.substring(p1+1,p2).toInt(),
      c=s.substring(p2+1,p3).toInt(), d=s.substring(p3+1).toInt();
  if(a<0||a>255||b<0||b>255||c<0||c>255||d<0||d>255) return false;
  out[0]=a; out[1]=b; out[2]=c; out[3]=d;
  return true;
}

static bool bootHeld() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  uint32_t t=millis();
  while(millis()-t<1200){
    if(digitalRead(BOOT_PIN)==HIGH) return false;
    delay(10);
  }
  return true;
}

/* ================= Config ================= */
static void setDefaults(){
  memset(&cfg,0,sizeof(cfg));
  cfg.magic=CFG_MAGIC;

  cfg.forceConfig = false;

  // Default AP password
  strcpy(cfg.ap_pass,"ArtnetDMX512");

  // Default network (only relevant if you choose static)
  cfg.useStatic=false;

  // Default static IP requested: 192.168.4.1
  cfg.ip[0]=192; cfg.ip[1]=168; cfg.ip[2]=4; cfg.ip[3]=1;

  // Default gateway/subnet
  cfg.gw[0]=192; cfg.gw[1]=168; cfg.gw[2]=0; cfg.gw[3]=1;
  cfg.sn[0]=255; cfg.sn[1]=255; cfg.sn[2]=255; cfg.sn[3]=0;

  // Default universe: 0
  cfg.universe=0;
}

static void loadConfig(){
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0,cfg);
  if(cfg.magic!=CFG_MAGIC){
    Serial.println("[CFG] No valid config found -> set defaults");
    setDefaults();
    EEPROM.put(0,cfg);
    EEPROM.commit();
  }

  // Ensure AP password valid length
  size_t aplen = strnlen(cfg.ap_pass, sizeof(cfg.ap_pass));
  if (aplen < 8 || aplen > 63) {
    Serial.println("[CFG] AP password invalid length -> reset to default");
    strcpy(cfg.ap_pass, "ArtnetDMX512");
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }

  startUniverse=cfg.universe;
  Serial.printf("[CFG] Loaded: forceConfig=%d, useStatic=%d, universe=%u\n",
                (int)cfg.forceConfig, (int)cfg.useStatic, (unsigned)cfg.universe);
  Serial.printf("[CFG] STA SSID='%s'\n", cfg.sta_ssid);
  Serial.printf("[CFG] Static IP=%s GW=%s SN=%s\n",
                ipToString(cfg.ip).c_str(), ipToString(cfg.gw).c_str(), ipToString(cfg.sn).c_str());
}

static void saveConfig(){
  cfg.magic=CFG_MAGIC;
  EEPROM.put(0,cfg);
  EEPROM.commit();
  startUniverse=cfg.universe;
  Serial.println("[CFG] Saved to EEPROM");
}

/* ===== Factory Reset ===== */
static void factoryResetAndReboot(const char* reason){
  Serial.printf("[RESET] Factory reset requested (%s)\n", reason ? reason : "unknown");

  setDefaults();

  // Ensure we come up in config mode so device stays reachable
  cfg.forceConfig = true;
  cfg.sta_ssid[0] = '\0';
  cfg.sta_pass[0] = '\0';

  saveConfig();

  Serial.println("[RESET] Done. Rebooting now...");
  delay(600);
  ESP.restart();
}

/* ================= Web UI ================= */
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="de">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP-Artnet Setup</title>
<style>
  :root{
    --bg:#0b0f14; --muted:#94a3b8; --text:#e8eefc;
    --border:#1f2a3a; --accent:#3b82f6;
    --ok:#34d399; --bad:#f87171;
  }
  *{box-sizing:border-box}
  body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,sans-serif;background:var(--bg);color:var(--text)}
  .wrap{max-width:920px;margin:auto;padding:18px}
  h1{margin:0 0 8px;font-size:22px}
  .statusbar{
    display:flex;gap:12px;flex-wrap:wrap;align-items:center;justify-content:space-between;
    border:1px solid var(--border);background:rgba(255,255,255,.03);
    border-radius:14px;padding:10px 12px;margin:10px 0 14px;
  }
  .badge{font-size:12px;color:var(--muted);display:flex;gap:8px;align-items:center}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;color:var(--text)}
  .dot{display:inline-block;width:10px;height:10px;border-radius:999px;background:#334155;vertical-align:middle}
  .dot.ok{background:var(--ok)}
  .dot.bad{background:var(--bad)}
  .grid{display:grid;grid-template-columns:1fr;gap:12px}
  @media(min-width:900px){.grid{grid-template-columns:1fr 1fr}}
  .card{background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:16px;padding:14px}
  h2{margin:0 0 8px;font-size:15px}
  label{display:block;margin-top:8px;color:var(--muted);font-size:12px}
  input,select{
    width:100%;
    padding:8px 10px;
    border-radius:12px;
    border:1px solid var(--border);
    background:#08101c;
    color:var(--text);
    font-size:14px;
    height:38px;
    outline:none;
  }
  input:focus,select:focus{border-color:rgba(59,130,246,.65);box-shadow:0 0 0 3px rgba(59,130,246,.15)}
  .row{display:grid;grid-template-columns:1fr;gap:10px}
  @media(min-width:640px){.row{grid-template-columns:1fr 1fr}}
  button{
    margin-top:10px;
    padding:9px 12px;
    border-radius:12px;
    border:1px solid var(--border);
    background:linear-gradient(180deg, rgba(59,130,246,.35), rgba(59,130,246,.15));
    color:#fff;font-weight:650;
    cursor:pointer;
    height:38px;
  }
  button.secondary{background:#08101c;color:var(--text)}
  button.danger{
    background:linear-gradient(180deg, rgba(248,113,113,.30), rgba(248,113,113,.12));
    border-color:#3a1f2a;
  }
  .help{color:var(--muted);font-size:12px;margin-top:8px;line-height:1.35}

  /* Overlay */
  #overlay{
    display:none; position:fixed; inset:0; background:rgba(0,0,0,.6);
    backdrop-filter: blur(6px); z-index:9999;
  }
  #overlayCard{
    max-width:520px; margin:12vh auto; background:#0f1720; border:1px solid #1f2a3a;
    border-radius:16px; padding:16px; color:#e8eefc;
    box-shadow:0 12px 40px rgba(0,0,0,.45);
  }
  #overlayTitle{font-weight:800; font-size:16px;}
  #overlayMsg{margin-top:8px; color:#94a3b8; font-size:13px; line-height:1.4;}
  .btnRow{display:flex; gap:10px; margin-top:12px; flex-wrap:wrap;}
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP-Artnet Setup</h1>

  <div class="statusbar">
    <div class="badge">IP: <span id="ipNow" class="mono">…</span></div>
    <div class="badge">DMX Universe: <span id="uniNow" class="mono">…</span></div>
    <div class="badge">Modus: <span id="modeNow" class="mono">…</span></div>
    <div class="badge">ArtNet <span id="artDot" class="dot"></span></div>
    <div class="badge">DMX <span id="dmxDot" class="dot"></span></div>
  </div>

  <div class="grid">

    <div class="card">
      <h2>WLAN</h2>

      <label>Netzwerke</label>
      <div class="row">
        <div><select id="net"></select></div>
        <div><button class="secondary" type="button" onclick="scan()">Scan</button></div>
      </div>

      <label>SSID</label>
      <input id="ssid" autocomplete="off">

      <label>Passwort</label>
      <input id="pass" type="password" autocomplete="new-password">

      <div class="help">Qualität wird aus RSSI abgeleitet: gut / mittel / schlecht.</div>
    </div>

    <div class="card">
      <h2>Netzwerk</h2>

      <label>IP Modus</label>
      <select id="ipmode" onchange="toggleStatic()">
        <option value="dhcp">DHCP</option>
        <option value="static">Static</option>
      </select>

      <div id="staticFields">
        <div class="row">
          <div><label>IP</label><input id="ip" class="mono"></div>
          <div><label>Gateway</label><input id="gw" class="mono"></div>
        </div>
        <label>Subnet</label><input id="sn" class="mono">
      </div>

      <label>DMX Universe</label>
      <input id="uni" type="number" min="0" max="32767">
    </div>

    <div class="card">
      <h2>Access Point</h2>
      <label>AP Passwort (8–63 Zeichen)</label>
      <input id="apPass" type="password" autocomplete="new-password">

      <label>Persistenter Config-Mode</label>
      <select id="forceCfg">
        <option value="0">Aus</option>
        <option value="1">Ein</option>
      </select>

      <button type="button" onclick="save()">Speichern & Neustart</button>

      <div class="btnRow">
        <button class="danger" type="button" onclick="factoryReset()">Werkseinstellungen</button>
      </div>

      <div class="help">
        „Werkseinstellungen“ löscht WLAN/Universe/etc. Danach startet das Gerät im Config-Mode (AP + Captive Portal).<br>IP: 192.168.1.4 SSID: ESP-Artnet Passwort: ArtnetDMX512
      </div>
    </div>

  </div>
</div>

<div id="overlay">
  <div id="overlayCard">
    <div id="overlayTitle">…</div>
    <div id="overlayMsg">…</div>
    <div class="btnRow">
      <button class="secondary" type="button" onclick="cancelReconnect()">Abbrechen</button>
      <button type="button" onclick="openManual()">Manuell öffnen</button>
    </div>
  </div>
</div>

<script>
function toggleStatic(){
  document.getElementById('staticFields').style.display =
    (document.getElementById('ipmode').value==='static') ? 'block' : 'none';
}

function qLabel(rssi){
  if(rssi >= -60) return 'gut';
  if(rssi >= -75) return 'mittel';
  return 'schlecht';
}

function isValidIPv4(str){
  const m = str.trim().match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if(!m) return false;
  for(let i=1;i<=4;i++){
    const n = Number(m[i]);
    if(n<0 || n>255) return false;
  }
  return true;
}

function setDot(id, ok){
  const el = document.getElementById(id);
  el.className = 'dot ' + (ok ? 'ok' : 'bad');
}

async function scan(){
  const r=await fetch('/scan', {cache:'no-store'});
  const j=await r.json();
  const s=document.getElementById('net'); s.innerHTML='';
  if(!j.networks || !j.networks.length){
    const o=document.createElement('option'); o.textContent='Keine Netzwerke gefunden'; s.appendChild(o);
    return;
  }
  j.networks.forEach(n=>{
    const o=document.createElement('option');
    o.value=n.ssid;
    o.textContent=`${n.ssid} (${qLabel(n.rssi)})`;
    s.appendChild(o);
  });
  s.onchange=()=>document.getElementById('ssid').value=s.value;
}

/* ===== Form once (no jumping) ===== */
async function loadFormOnce(){
  const r=await fetch('/cfg', {cache:'no-store'});
  const j=await r.json();

  document.getElementById('ipNow').textContent = j.ip_now || '-';
  document.getElementById('uniNow').textContent = j.universe;
  document.getElementById('modeNow').textContent = j.config_mode ? 'CONFIG' : 'NORMAL';
  setDot('artDot', !!j.artnet_active);
  setDot('dmxDot', !!j.dmx_active);

  document.getElementById('ssid').value = j.sta_cfg_ssid || '';
  document.getElementById('pass').value = j.sta_cfg_pass || '';
  document.getElementById('ipmode').value = j.use_static ? 'static' : 'dhcp';
  document.getElementById('ip').value = j.ip || '';
  document.getElementById('gw').value = j.gw || '';
  document.getElementById('sn').value = j.sn || '';
  document.getElementById('uni').value = j.universe;
  document.getElementById('apPass').value = j.ap_pass || '';
  document.getElementById('forceCfg').value = j.force_config ? '1' : '0';

  toggleStatic();
}

/* ===== Status refresh (only LEDs + statusbar) ===== */
async function refreshStatus(){
  const r=await fetch('/status', {cache:'no-store'});
  const j=await r.json();

  document.getElementById('ipNow').textContent = j.ip_now || '-';
  document.getElementById('uniNow').textContent = j.universe;
  document.getElementById('modeNow').textContent = j.config_mode ? 'CONFIG' : 'NORMAL';
  setDot('artDot', !!j.artnet_active);
  setDot('dmxDot', !!j.dmx_active);
}

// ===== Overlay + reconnect =====
let reconnectTimer = null;
let reconnectTries = 0;
let targetCandidates = [];

function showOverlay(title, msg){
  document.getElementById('overlay').style.display = 'block';
  document.getElementById('overlayTitle').textContent = title || '';
  document.getElementById('overlayMsg').textContent = msg || '';
}

function hideOverlay(){
  document.getElementById('overlay').style.display = 'none';
}

function cancelReconnect(){
  if(reconnectTimer) clearInterval(reconnectTimer);
  reconnectTimer = null;
  hideOverlay();
}

function openManual(){
  const best = targetCandidates[0] || 'http://192.168.4.1/';
  window.open(best, '_blank');
}

async function tryPing(base){
  try{
    const r = await fetch(base + 'status', { cache: 'no-store' });
    return r.ok;
  }catch(e){
    return false;
  }
}

function startReconnect(preferAp){
  reconnectTries = 0;

  const currentBase = window.location.origin + '/';
  const ipmode = document.getElementById('ipmode').value;
  const staticIp = document.getElementById('ip').value.trim();

  targetCandidates = [];
  // Nach Factory Reset ist AP praktisch immer das Ziel
  if (preferAp) targetCandidates.push('http://192.168.4.1/');

  targetCandidates.push(currentBase);
  if (ipmode === 'static' && isValidIPv4(staticIp)) targetCandidates.push('http://' + staticIp + '/');
  if (!preferAp) targetCandidates.push('http://192.168.4.1/');

  reconnectTimer = setInterval(async ()=>{
    reconnectTries++;

    if (reconnectTries < 4) return;

    for(const base of targetCandidates){
      const ok = await tryPing(base);
      if(ok){
        showOverlay('Verbunden!', 'Öffne Interface…');
        clearInterval(reconnectTimer);
        reconnectTimer = null;
        window.location.href = base;
        return;
      }
    }

    if (reconnectTries % 5 === 0) {
      document.getElementById('overlayMsg').textContent = `Noch kein Gerät gefunden… (${reconnectTries}s)`;
    }

    if (reconnectTries > 45) {
      document.getElementById('overlayMsg').textContent = 'Reconnect dauert zu lange. Bitte manuell öffnen (Button).';
      clearInterval(reconnectTimer);
      reconnectTimer = null;
    }
  }, 1000);
}

async function save(){
  const ipmode = document.getElementById('ipmode').value;
  if (ipmode === 'static') {
    const ip = document.getElementById('ip').value;
    const gw = document.getElementById('gw').value;
    const sn = document.getElementById('sn').value;
    if(!isValidIPv4(ip) || !isValidIPv4(gw) || !isValidIPv4(sn)){
      alert('Bitte gültige IP/Gateway/Subnet eingeben (z.B. 255.255.255.0).');
      return;
    }
  }

  const apPass = document.getElementById('apPass').value.trim();
  if (apPass.length > 0 && (apPass.length < 8 || apPass.length > 63)) {
    alert('AP Passwort muss 8–63 Zeichen haben.');
    return;
  }

  const d=new URLSearchParams();
  ['ssid','pass','ipmode','ip','gw','sn','uni','apPass','forceCfg']
    .forEach(k=>d.set(k,document.getElementById(k).value));

  const r = await fetch('/save',{method:'POST',body:d});
  if(r.ok){
    showOverlay('✅ Erfolgreich gespeichert. Reboot now!', 'Warte kurz… versuche Verbindung wiederherzustellen…');
    startReconnect(false);
  } else {
    const txt = await r.text().catch(()=> '');
    alert('Speichern fehlgeschlagen: ' + (txt || r.status));
  }
}

async function factoryReset(){
  if(!confirm('Werkseinstellungen laden? WLAN/Universe/IP werden gelöscht.')) return;

  showOverlay('♻ Werkseinstellungen', 'Setze zurück… Reboot…');

  const r = await fetch('/factoryreset', {method:'POST'});
  if(r.ok){
    // Nach Reset: Gerät startet im Config-Mode, also AP 192.168.4.1 bevorzugen
    startReconnect(true);
  } else {
    const txt = await r.text().catch(()=> '');
    alert('Reset fehlgeschlagen: ' + (txt || r.status));
    hideOverlay();
  }
}

// init
loadFormOnce();
scan();
setInterval(refreshStatus, 1000);
</script>
</body></html>
)HTML";

/* ================= Web utilities ================= */
static void sendNoCache() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

/* ================= Captive helpers ================= */
static void captiveRedirectToRoot() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + String("/"), true);
  server.send(302, "text/plain", "");
}

/* ================= Web Handlers ================= */
static void handleRoot(){
  sendNoCache();
  server.send(200,"text/html; charset=utf-8",FPSTR(INDEX_HTML));
}

static String currentIpString() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (configMode) return WiFi.softAPIP().toString();
  return String("");
}

static void handleCfg(){
  sendNoCache();

  String ipNow = currentIpString();
  uint32_t now = millis();
  bool artnetActive = (now - g_lastArtnetMs) < 500;
  bool dmxActive    = (now - g_lastDmxMs)    < 500;

  String j="{";
  j+="\"sta_cfg_ssid\":\""+String(cfg.sta_ssid)+"\",";
  j+="\"sta_cfg_pass\":\""+String(cfg.sta_pass)+"\",";
  j+="\"use_static\":"+String(cfg.useStatic?"true":"false")+",";
  j+="\"ip\":\""+ipToString(cfg.ip)+"\",";
  j+="\"gw\":\""+ipToString(cfg.gw)+"\",";
  j+="\"sn\":\""+ipToString(cfg.sn)+"\",";
  j+="\"universe\":"+String(cfg.universe)+",";
  j+="\"ap_pass\":\""+String(cfg.ap_pass)+"\",";
  j+="\"force_config\":"+String(cfg.forceConfig?"true":"false")+",";
  j+="\"config_mode\":"+String(configMode?"true":"false")+",";
  j+="\"ip_now\":\""+ipNow+"\",";
  j+="\"artnet_active\":"+String(artnetActive?"true":"false")+",";
  j+="\"dmx_active\":"+String(dmxActive?"true":"false");
  j+="}";
  server.send(200,"application/json; charset=utf-8",j);
}

// status-only endpoint (so form doesn't jump)
static void handleStatus(){
  sendNoCache();

  String ipNow = currentIpString();
  uint32_t now = millis();
  bool artnetActive = (now - g_lastArtnetMs) < 500;
  bool dmxActive    = (now - g_lastDmxMs)    < 500;

  String j="{";
  j+="\"ip_now\":\""+ipNow+"\",";
  j+="\"universe\":"+String(cfg.universe)+",";
  j+="\"config_mode\":"+String(configMode?"true":"false")+",";
  j+="\"artnet_active\":"+String(artnetActive?"true":"false")+",";
  j+="\"dmx_active\":"+String(dmxActive?"true":"false");
  j+="}";
  server.send(200,"application/json; charset=utf-8",j);
}

static void handleScan(){
  sendNoCache();

  Serial.println("[WEB] WiFi scan requested");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  String j="{\"networks\":[";
  bool first=true;
  for(int i=0;i<n;i++){
    String ssid = WiFi.SSID(i);
    if (ssid.length()==0) continue;
    ssid.replace("\\","\\\\"); ssid.replace("\"","\\\"");
    if(!first) j+=",";
    first=false;
    j+="{\"ssid\":\""+ssid+"\",\"rssi\":"+String(WiFi.RSSI(i))+"}";
  }
  j+="]}";
  WiFi.scanDelete();
  server.send(200,"application/json; charset=utf-8",j);
}

static void handleSave(){
  sendNoCache();

  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass"); pass.trim();
  String ipmode = server.arg("ipmode"); ipmode.trim();
  String ip = server.arg("ip"); ip.trim();
  String gw = server.arg("gw"); gw.trim();
  String sn = server.arg("sn"); sn.trim();
  String uni = server.arg("uni"); uni.trim();
  String apP = server.arg("apPass"); apP.trim();
  String fcfg = server.arg("forceCfg"); fcfg.trim();

  long u = uni.toInt();
  if (u < 0) u = 0;
  if (u > 32767) u = 32767;

  if (apP.length() > 0 && (apP.length() < 8 || apP.length() > 63)) {
    Serial.println("[WEB] Save rejected: invalid AP password length");
    server.send(400, "text/plain; charset=utf-8", "Invalid AP password length (8..63)");
    return;
  }

  memset(cfg.sta_ssid, 0, sizeof(cfg.sta_ssid));
  memset(cfg.sta_pass, 0, sizeof(cfg.sta_pass));
  ssid.toCharArray(cfg.sta_ssid, sizeof(cfg.sta_ssid));
  pass.toCharArray(cfg.sta_pass, sizeof(cfg.sta_pass));

  cfg.useStatic = (ipmode == "static");
  if (cfg.useStatic) {
    uint8_t t[4];

    if(!parseIPv4(ip, t)) { server.send(400,"text/plain; charset=utf-8","Invalid IP"); return; }
    memcpy(cfg.ip, t, 4);

    if(!parseIPv4(gw, t)) { server.send(400,"text/plain; charset=utf-8","Invalid Gateway"); return; }
    memcpy(cfg.gw, t, 4);

    if(!parseIPv4(sn, t)) { server.send(400,"text/plain; charset=utf-8","Invalid Subnet"); return; }
    memcpy(cfg.sn, t, 4);
  }

  cfg.universe = (uint16_t)u;
  cfg.forceConfig = (fcfg == "1");

  if (apP.length() > 0) {
    memset(cfg.ap_pass, 0, sizeof(cfg.ap_pass));
    apP.toCharArray(cfg.ap_pass, sizeof(cfg.ap_pass));
  }

  Serial.println("[WEB] Saving config...");
  Serial.printf("[WEB] STA SSID='%s', useStatic=%d, universe=%u, forceConfig=%d\n",
                cfg.sta_ssid, (int)cfg.useStatic, (unsigned)cfg.universe, (int)cfg.forceConfig);

  saveConfig();

  server.send(200,"text/plain; charset=utf-8","OK");

  Serial.println("[SYS] Reboot requested by web (save)");
  delay(500);
  ESP.restart();
}

static void handleFactoryReset(){
  sendNoCache();
  server.send(200, "text/plain; charset=utf-8", "OK");
  // Reset after responding so browser gets 200
  delay(200);
  factoryResetAndReboot("web");
}

/* ================= ArtNet Callback ================= */
void onArtNetFrame(uint16_t uni, uint16_t len, uint8_t, uint8_t* data){
  if(uni != startUniverse) return;

  g_artnetFramesTotal++;
  g_lastArtnetMs = millis();

  dmxBuffer[0] = 0;
  uint16_t channels = min<uint16_t>(len, 512);
  for(uint16_t i=0;i<512;i++){
    dmxBuffer[i+1] = (i<channels) ? data[i] : 0;
  }
}

/* ================= WiFi / Mode selection ================= */
static void startSTAIfPossible(bool allowConnect) {
  if (!allowConnect) return;

  if (cfg.sta_ssid[0] == '\0') {
    Serial.println("[WIFI] No STA SSID stored");
    return;
  }

  WiFi.mode(WIFI_STA);

  if (cfg.useStatic) {
    IPAddress ip(cfg.ip[0],cfg.ip[1],cfg.ip[2],cfg.ip[3]);
    IPAddress gw(cfg.gw[0],cfg.gw[1],cfg.gw[2],cfg.gw[3]);
    IPAddress sn(cfg.sn[0],cfg.sn[1],cfg.sn[2],cfg.sn[3]);
    WiFi.config(ip, gw, sn);
    Serial.printf("[WIFI] STA static config: IP=%s GW=%s SN=%s\n",
                  ipToString(cfg.ip).c_str(), ipToString(cfg.gw).c_str(), ipToString(cfg.sn).c_str());
  } else {
    Serial.println("[WIFI] STA DHCP mode");
  }

  Serial.printf("[WIFI] Connecting to '%s' ...\n", cfg.sta_ssid);
  WiFi.begin(cfg.sta_ssid, cfg.sta_pass);

  uint32_t start = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start < 10000){
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected. IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[WIFI] STA connect failed");
  }
}

static void startConfigAP() {
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID, cfg.ap_pass);
  Serial.printf("[AP] softAP start: %s (pass len=%u) -> %s\n",
                AP_SSID, (unsigned)strlen(cfg.ap_pass), ok ? "OK" : "FAIL");
  Serial.printf("[AP] AP IP=%s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("[DNS] Captive DNS started");
}

/* ================= Serial command handling ================= */
static String g_serialLine;

static void handleSerialCommands(){
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String cmd = g_serialLine;
      g_serialLine = "";
      cmd.trim();
      cmd.toLowerCase();

      if (cmd.length() == 0) return;

      Serial.printf("[SER] Command: '%s'\n", cmd.c_str());

      if (cmd == "reset" || cmd == "factoryreset" || cmd == "factory") {
        factoryResetAndReboot("serial");
        return;
      } else {
        Serial.println("[SER] Unknown command. Try: reset");
      }
    } else {
      if (g_serialLine.length() < 120) g_serialLine += c;
    }
  }
}

/* ================= Setup / Loop ================= */
void setup(){
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(150);
  Serial.println("\n=== ESP-Artnet boot ===");
  Serial.println("[SER] Type 'reset' + Enter for factory reset.");

  loadConfig();

  bool forcedByButton = bootHeld();
  Serial.printf("[BOOT] Button forced config mode: %s\n", forcedByButton ? "YES" : "NO");

  bool allowStaConnect = (!cfg.forceConfig && !forcedByButton);
  startSTAIfPossible(allowStaConnect);

  configMode = forcedByButton || cfg.forceConfig || (WiFi.status() != WL_CONNECTED);
  Serial.printf("[MODE] configMode=%d (forceConfig=%d, staConnected=%d)\n",
                (int)configMode, (int)cfg.forceConfig, (WiFi.status()==WL_CONNECTED));

  if (configMode) {
    startConfigAP();

    server.on("/generate_204", HTTP_ANY, [](){ captiveRedirectToRoot(); });
    server.on("/gen_204", HTTP_ANY, [](){ captiveRedirectToRoot(); });
    server.on("/hotspot-detect.html", HTTP_ANY, [](){ captiveRedirectToRoot(); });
    server.on("/ncsi.txt", HTTP_ANY, [](){ captiveRedirectToRoot(); });
    server.on("/connecttest.txt", HTTP_ANY, [](){ captiveRedirectToRoot(); });
    server.on("/fwlink", HTTP_ANY, [](){ captiveRedirectToRoot(); });

    server.onNotFound([](){ captiveRedirectToRoot(); });
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/cfg", HTTP_GET, handleCfg);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/factoryreset", HTTP_POST, handleFactoryReset);
  server.begin();

  Serial.println("[WEB] Server started on port 80");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WEB] Open: http://%s/\n", WiFi.localIP().toString().c_str());
  }
  if (configMode) {
    Serial.printf("[WEB] Open (AP): http://%s/\n", WiFi.softAPIP().toString().c_str());
  }

  artnet.setArtDmxCallback(onArtNetFrame);
  artnet.begin();
  Serial.printf("[ARTNET] Listener started (UDP 6454), universe=%u\n", (unsigned)startUniverse);

  memset(dmxBuffer, 0, sizeof(dmxBuffer));
  dmxBuffer[0] = 0;

  dmx_config_t c = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {};
  bool ok = dmx_driver_install(DMX_PORT, &c, personalities, 0);
  Serial.printf("[DMX] driver_install: %s\n", ok ? "OK" : "FAIL");
  dmx_set_pin(DMX_PORT, DMX_TX_PIN, -1, -1);
  Serial.printf("[DMX] TX pin=%d\n", DMX_TX_PIN);

  startUniverse = cfg.universe;
  Serial.printf("[READY] Universe=%u, DMX refresh=%ums\n", (unsigned)startUniverse, (unsigned)DMX_REFRESH_MS);
}

void loop(){
  // ===== BOOT long-press factory reset =====
if (digitalRead(BOOT_PIN) == LOW) { // gedrückt
  if (bootPressStartMs == 0) {
    bootPressStartMs = millis();
    Serial.println("[BOOT] Button pressed");
  } else if (!bootResetTriggered &&
             (millis() - bootPressStartMs >= BOOT_RESET_TIME_MS)) {

    bootResetTriggered = true;
    Serial.println("[BOOT] Long press detected -> Factory Reset");

    factoryResetAndReboot("boot-long-press");
  }
} else {
  // losgelassen -> zurücksetzen
  if (bootPressStartMs != 0) {
    Serial.println("[BOOT] Button released");
  }
  bootPressStartMs = 0;
  bootResetTriggered = false;
}

  handleSerialCommands();

  if(configMode) dnsServer.processNextRequest();
  server.handleClient();
  artnet.read();

  uint32_t now = millis();

  if(now - lastDmxSendMs >= DMX_REFRESH_MS){
    lastDmxSendMs = now;
    g_lastDmxMs = now;

    dmx_write(DMX_PORT, dmxBuffer, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);
  }

  if (now - g_lastStatsMs >= 5000) {
    g_lastStatsMs = now;
    uint32_t frames = g_artnetFramesTotal;
    uint32_t delta = frames - g_lastFramesSnapshot;
    g_lastFramesSnapshot = frames;

    String ipNow = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP().toString()
                 : (configMode ? WiFi.softAPIP().toString() : String("-"));

    Serial.printf("[STATS] ArtNet frames last 5s: %lu | total: %lu | IP: %s | Mode: %s | Universe: %u\n",
                  (unsigned long)delta, (unsigned long)frames,
                  ipNow.c_str(),
                  (configMode ? "CONFIG" : "NORMAL"),
                  (unsigned)cfg.universe);
  }
}
