#include "server.h"
#include "secrets.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer      s_http(80);
static SayCallback    s_onSay;
static String         s_status = "Booting...";
static String         s_lastUser;
static String         s_lastReply;

// ---------------------------------------------------------------------------
// Static HTML page (served at /)
// ---------------------------------------------------------------------------
// Mic button uses the browser's Web Speech API (works great in Chrome).
// There's a text-input fallback for browsers that don't support it.
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Daemon</title>
<style>
 :root { color-scheme: dark; }
 html,body{margin:0;height:100%;background:#000;color:#cfefff;
   font-family:-apple-system,Segoe UI,Roboto,sans-serif;text-align:center}
 body{display:flex;flex-direction:column;align-items:center;
   justify-content:center;padding:24px;gap:16px}
 h1{margin:0;font-size:20px;color:#49f;letter-spacing:2px}
 .status{opacity:.8;font-size:13px;min-height:18px}
 button.big{
   width:220px;height:220px;border-radius:50%;border:none;cursor:pointer;
   background:radial-gradient(circle at 35% 35%,#0cf,#06b 55%,#024 100%);
   color:#fff;font-size:22px;letter-spacing:2px;
   box-shadow:0 0 40px #06f,0 0 100px #06f inset;transition:transform .1s}
 button.big:active{transform:scale(.96)}
 button.big.listening{animation:pulse 1s infinite;
   background:radial-gradient(circle at 35% 35%,#f77,#b22 55%,#400 100%);
   box-shadow:0 0 40px #f44,0 0 100px #f44 inset}
 @keyframes pulse{50%{filter:brightness(1.3)}}
 .row{display:flex;gap:8px;width:100%;max-width:420px}
 input{flex:1;padding:12px;border-radius:10px;border:1px solid #26c;
   background:#021;color:#cff;font-size:16px}
 button.small{padding:12px 16px;border-radius:10px;border:none;
   background:#06b;color:#fff;font-size:16px;cursor:pointer}
 .log{width:100%;max-width:420px;text-align:left;font-size:14px;
   line-height:1.4;opacity:.9;margin-top:8px}
 .log .you{color:#8ef}
 .log .daemon{color:#f9f}
</style></head>
<body>
<h1>DAEMON</h1>
<div class="status" id="status">loading…</div>
<button class="big" id="talk">TAP TO TALK</button>
<div class="row">
  <input id="textin" placeholder="or type here and press send"/>
  <button class="small" id="send">send</button>
</div>
<div class="log" id="log"></div>
<script>
const $ = id => document.getElementById(id);
const statusEl = $('status'), logEl = $('log'), btn = $('talk');
const inp = $('textin'), sendBtn = $('send');

function setStatus(s){ statusEl.textContent = s; }
function addLog(role, text){
  const line = document.createElement('div');
  line.className = role;
  line.innerHTML = '<b>' + (role==='you' ? 'you' : 'daemon') + ':</b> ' + text;
  logEl.prepend(line);
}

async function refresh(){
  try{
    const r = await fetch('/state');
    const j = await r.json();
    setStatus(j.status || '');
  }catch(e){}
}
setInterval(refresh, 1500); refresh();

async function sendUtterance(text){
  text = (text||'').trim();
  if(!text) return;
  addLog('you', text);
  setStatus('thinking…');
  try{
    const r = await fetch('/say', {method:'POST', headers:{'Content-Type':'text/plain'}, body:text});
    const j = await r.json();
    if(j.reply){ addLog('daemon', j.reply); }
    setStatus('idle');
  }catch(e){ setStatus('error: '+e); }
}

sendBtn.onclick = () => { const t = inp.value; inp.value=''; sendUtterance(t); };
inp.addEventListener('keydown', e => { if(e.key==='Enter') sendBtn.click(); });

// --- Web Speech API mic ---
const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
if(!SR){
  btn.disabled = true;
  btn.textContent = 'no mic in this browser';
  btn.style.opacity = .5;
} else {
  let rec = null, active = false;
  btn.onclick = () => {
    if(active){ rec && rec.stop(); return; }
    rec = new SR();
    rec.lang = 'en-US';
    rec.interimResults = false;
    rec.maxAlternatives = 1;
    rec.onstart = () => { active=true; btn.classList.add('listening'); btn.textContent='LISTENING…'; };
    rec.onerror = e => { setStatus('mic error: '+e.error); };
    rec.onend   = () => { active=false; btn.classList.remove('listening'); btn.textContent='TAP TO TALK'; };
    rec.onresult= e => {
      const t = e.results[0][0].transcript;
      sendUtterance(t);
    };
    rec.start();
  };
}
</script>
</body></html>
)HTML";

static void handleRoot() {
  s_http.send_P(200, "text/html", PAGE_HTML);
}

static void handleState() {
  String j = "{\"status\":\"" + s_status + "\"}";
  s_http.send(200, "application/json", j);
}

static void handleSay() {
  if (!s_http.hasArg("plain")) {
    s_http.send(400, "text/plain", "expected text/plain body");
    return;
  }
  String text = s_http.arg("plain");
  text.trim();
  if (text.length() == 0) {
    s_http.send(400, "text/plain", "empty");
    return;
  }
  // Hand off to the main app; it runs synchronously here, then we emit the
  // reply. Not ideal for true concurrency but fine for a single user.
  s_lastUser = text;
  s_lastReply = "";
  if (s_onSay) s_onSay(text);

  String j = "{\"reply\":";
  // naive JSON string escape (enough for plain text)
  String esc = s_lastReply;
  esc.replace("\\", "\\\\");
  esc.replace("\"", "\\\"");
  esc.replace("\n", " ");
  j += "\"" + esc + "\"}";
  s_http.send(200, "application/json", j);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
// Scan once and log what's in range. Handy for debugging mis-typed SSIDs or
// 5-GHz-only hotspots (the ESP32-S3 radio is 2.4 GHz only).
static void logNearbyNetworks() {
  int n = WiFi.scanNetworks();
  Serial.printf("wifi: %d networks in range\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  [%2d] %-32s  rssi=%d  ch=%d  enc=%d\n",
                  i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i), WiFi.encryptionType(i));
  }
  WiFi.scanDelete();
}

bool serverBeginWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("daemon");
  WiFi.disconnect(true, true);
  delay(100);

  logNearbyNetworks();

  Serial.printf("wifi: connecting to '%s' ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(300);
    wl_status_t st = WiFi.status();
    if (st != last) {
      Serial.printf("[%d]", (int)st);
      last = st;
    }
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("wifi: connect failed (status=%d)\n", (int)WiFi.status());
    return false;
  }
  Serial.print("wifi: got IP "); Serial.println(WiFi.localIP());
  return true;
}

void serverBeginHttp(SayCallback onSay) {
  s_onSay = std::move(onSay);
  s_http.on("/",      HTTP_GET,  handleRoot);
  s_http.on("/state", HTTP_GET,  handleState);
  s_http.on("/say",   HTTP_POST, handleSay);
  s_http.begin();
  Serial.println("http: listening on :80");
}

void serverLoop()                       { s_http.handleClient(); }
void serverSetStatus(const String &s)   { s_status = s; }
void serverSetReply(const String &user, const String &reply) {
  s_lastUser = user;
  s_lastReply = reply;
}
String serverLocalIP()                  { return WiFi.localIP().toString(); }
