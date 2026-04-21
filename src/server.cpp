#include "server.h"
#include "secrets.h"
#include "devcfg.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

static WebServer      s_http(80);
static SayCallback    s_onSay;
static String         s_status = "Booting...";
static String         s_lastUser;
static String         s_lastReply;
// Bumped every time a new reply is published so the web UI's poller can
// tell which state messages are "new" and append them to the chat log.
static volatile uint32_t s_replySeq = 0;

// Worker-task plumbing: the /say handler enqueues the utterance and returns
// immediately, so the main loop (creature animation, touch, voice, wifi)
// never stalls on the 5–20 s AI round trip. The worker task below pulls
// utterances off the queue and runs the user-provided callback.
static QueueHandle_t  s_sayQueue = nullptr;

static void sayWorkerTask(void *) {
  for (;;) {
    char *buf = nullptr;
    if (xQueueReceive(s_sayQueue, &buf, portMAX_DELAY) == pdTRUE && buf) {
      if (s_onSay) s_onSay(String(buf));
      free(buf);
    }
  }
}

static void ensureSayWorker() {
  if (s_sayQueue) return;
  s_sayQueue = xQueueCreate(/*depth=*/4, sizeof(char *));
  // HTTPS + mbedTLS + Ed25519 need generous stack. 24 KB is roomy.
  // Pin to core 1 to keep TCP/IP (core 0) and audio-friendly work off
  // each other.
  xTaskCreatePinnedToCore(sayWorkerTask, "say-worker",
                          /*stack=*/ 24576, nullptr,
                          /*prio=*/ 1, nullptr, /*core=*/ 1);
}

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
 :root{color-scheme:dark;--bg:#000;--surface:#0a0f1a;--border:#1a2440;
   --accent:#0f5dfd;--accent2:#07bfff;--text:#e6f4ff;--dim:#6a7d99;
   --red:#f87171;--green:#34d399}
 *{box-sizing:border-box}
 html,body{margin:0;background:var(--bg);color:var(--text);
   font-family:-apple-system,Segoe UI,Roboto,sans-serif;min-height:100%}
 body{max-width:520px;margin:0 auto;padding:16px 16px 80px}
 h1{margin:0;font-size:18px;color:var(--accent);letter-spacing:3px;text-align:center}
 .status{opacity:.75;font-size:12px;min-height:16px;text-align:center;margin:4px 0 12px}
 .tabs{display:flex;gap:4px;background:var(--surface);border:1px solid var(--border);
   border-radius:12px;padding:4px;margin-bottom:16px}
 .tab{flex:1;padding:10px;background:none;border:none;color:var(--dim);
   font-size:12px;font-weight:600;letter-spacing:1px;border-radius:8px;cursor:pointer}
 .tab.active{background:var(--accent);color:#000}
 .panel{display:none}
 .panel.active{display:block}
 .card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:12px}
 .card h3{margin:0 0 8px;font-size:12px;color:var(--accent);letter-spacing:1px}
 .card .hint{font-size:11px;color:var(--dim);margin:0 0 8px;line-height:1.4}

 /* chat */
 .mic{display:block;width:180px;height:180px;margin:12px auto;border-radius:50%;border:none;cursor:pointer;
   background:radial-gradient(circle at 35% 35%,#0cf,#06b 55%,#024 100%);
   color:#fff;font-size:18px;letter-spacing:2px;
   box-shadow:0 0 30px #06f,0 0 80px #06f inset;transition:transform .1s}
 .mic:active{transform:scale(.96)}
 .mic.listening{animation:pulse 1s infinite;
   background:radial-gradient(circle at 35% 35%,#f77,#b22 55%,#400 100%);
   box-shadow:0 0 30px #f44,0 0 80px #f44 inset}
 @keyframes pulse{50%{filter:brightness(1.3)}}
 .row{display:flex;gap:8px}
 input,textarea,select{width:100%;padding:10px;border-radius:8px;border:1px solid var(--border);
   background:#040810;color:var(--text);font-size:14px;font-family:inherit}
 textarea{resize:vertical;min-height:120px;line-height:1.45}
 .btn{padding:10px 16px;border-radius:8px;border:none;cursor:pointer;
   background:var(--accent);color:#000;font-weight:600;font-size:13px;letter-spacing:.5px}
 .btn.ghost{background:transparent;border:1px solid var(--border);color:var(--text);font-weight:500}
 .btn.danger{background:var(--red);color:#000}
 .log{margin-top:12px;font-size:13px;line-height:1.45}
 .log>div{padding:6px 0;border-bottom:1px solid var(--border)}
 .log .you{color:var(--accent2)}
 .log .daemon{color:var(--text)}

 /* services */
 .filters{display:flex;gap:6px;overflow-x:auto;padding-bottom:8px;margin-bottom:8px;scrollbar-width:none}
 .filters::-webkit-scrollbar{display:none}
 .chip{flex-shrink:0;padding:6px 10px;border-radius:999px;border:1px solid var(--border);
   background:var(--surface);color:var(--dim);font-size:11px;cursor:pointer;font-weight:500}
 .chip.on{background:var(--accent);border-color:var(--accent);color:#000}
 .svcrow{display:flex;align-items:flex-start;gap:10px;padding:10px 0;border-top:1px solid var(--border)}
 .svcrow:first-child{border-top:0}
 .toggle{flex-shrink:0;width:38px;height:22px;border-radius:999px;background:var(--border);
   border:none;cursor:pointer;position:relative;padding:0}
 .toggle.on{background:var(--accent)}
 .toggle::before{content:"";position:absolute;top:2px;left:2px;width:18px;height:18px;
   border-radius:50%;background:#fff;transition:transform .15s}
 .toggle.on::before{transform:translateX(16px)}
 .svcname{font-size:13px;font-weight:600;margin:0;color:var(--text)}
 .svcdesc{font-size:11px;color:var(--dim);margin:2px 0 6px;line-height:1.45}
 .meta{display:flex;gap:6px;flex-wrap:wrap;font-size:10px;color:var(--dim);align-items:center}
 .meta .cat{padding:2px 6px;border-radius:4px;font-weight:500}
 .badge-custom{padding:2px 6px;border-radius:4px;background:rgba(15,93,253,.15);color:var(--accent);font-weight:600;font-size:9px}
 .rm{margin-left:auto;background:none;border:none;color:var(--red);font-size:10px;cursor:pointer}

 /* modal */
 .mask{position:fixed;inset:0;background:rgba(0,0,0,.92);z-index:10;display:flex;align-items:flex-start;
   justify-content:center;padding:40px 16px;overflow-y:auto}
 .mask .card{width:100%;max-width:500px}
 .preview{background:#040810;border:1px solid var(--border);border-radius:8px;padding:10px;
   font-family:ui-monospace,monospace;font-size:11px;color:var(--text);white-space:pre-wrap;word-break:break-word}
 .error{background:rgba(248,113,113,.1);border:1px solid rgba(248,113,113,.25);
   border-radius:8px;padding:8px 10px;color:var(--red);font-size:12px}
 label{font-size:11px;color:var(--dim);font-weight:500;display:block;margin:10px 0 4px}
</style></head>
<body>
<h1>DAEMON</h1>
<div class="status" id="status">loading…</div>
<div class="tabs">
  <button class="tab active" data-tab="chat">CHAT</button>
  <button class="tab" data-tab="settings">SETTINGS</button>
  <button class="tab" data-tab="services">SERVICES</button>
</div>

<div class="panel active" id="panel-chat">
  <button class="mic" id="talk">TAP TO TALK</button>
  <div class="row">
    <input id="textin" placeholder="or type here, press enter"/>
    <button class="btn" id="send">send</button>
  </div>
  <div class="log" id="log"></div>
</div>

<div class="panel" id="panel-settings">
  <div class="card">
    <h3>LLM MODEL</h3>
    <p class="hint">Google/Gemini models work directly right now. Other providers route through the x402 gateway and need the wallet to pay per request — plumbing coming in the next firmware build.</p>
    <select id="model"></select>
  </div>
  <div class="card">
    <h3>PERSONALITY</h3>
    <p class="hint">System prompt injected before every reply. Leave blank to use the built-in default. Wallet / price context is always appended automatically.</p>
    <textarea id="personality" placeholder="(default Daemon persona)"></textarea>
    <div class="row" style="margin-top:10px;justify-content:flex-end">
      <button class="btn ghost" id="resetPersona">reset default</button>
      <button class="btn" id="saveSettings">save</button>
    </div>
  </div>
</div>

<div class="panel" id="panel-services">
  <div class="card">
    <h3>X402 SERVICES</h3>
    <p class="hint">Enable services the model can call as tools. Each call is paid in USDC from Daemon's wallet via the x402 protocol.</p>
    <div class="row" style="gap:6px;margin-bottom:8px">
      <input id="svcSearch" placeholder="search services…"/>
      <button class="btn" id="addSvc">+ add</button>
    </div>
    <div class="filters" id="filters"></div>
    <div id="svclist"></div>
  </div>
</div>

<script>
// ────────── static catalog (mirrored from the chrome extension) ──────────
const MODELS = [
  {id:'deepseek/deepseek-chat',         name:'DeepSeek V3',         provider:'DeepSeek'},
  {id:'openai/gpt-4o-mini',             name:'GPT-4o Mini',         provider:'OpenAI'},
  {id:'openai/gpt-5.4-nano',            name:'GPT-5.4 Nano',        provider:'OpenAI'},
  {id:'xai/grok-4-fast-reasoning',      name:'Grok 4 Fast',         provider:'xAI'},
  {id:'google/gemini-2.5-flash',        name:'Gemini 2.5 Flash',    provider:'Google'},
  {id:'xai/grok-3-mini',                name:'Grok 3 Mini',         provider:'xAI'},
  {id:'openai/gpt-5.4-mini',            name:'GPT-5.4 Mini',        provider:'OpenAI'},
  {id:'nvidia/kimi-k2.5',               name:'Kimi K2.5',           provider:'Moonshot'},
  {id:'anthropic/claude-haiku-4.5',     name:'Claude Haiku 4.5',    provider:'Anthropic'},
  {id:'deepseek/deepseek-reasoner',     name:'DeepSeek Reasoner',   provider:'DeepSeek'},
  {id:'google/gemini-2.5-pro',          name:'Gemini 2.5 Pro',      provider:'Google'},
  {id:'openai/gpt-5.4',                 name:'GPT-5.4',             provider:'OpenAI'},
  {id:'openai/gpt-4o',                  name:'GPT-4o',              provider:'OpenAI'},
  {id:'anthropic/claude-sonnet-4.6',    name:'Claude Sonnet 4.6',   provider:'Anthropic'},
  {id:'xai/grok-3',                     name:'Grok 3',              provider:'xAI'},
  {id:'google/gemini-3.1-pro',          name:'Gemini 3.1 Pro',      provider:'Google'},
  {id:'anthropic/claude-opus-4.6',      name:'Claude Opus 4.6',     provider:'Anthropic'},
];

const CATS = {
  ai:            {label:'AI / ML',     color:'#a78bfa'},
  'crypto-data': {label:'Crypto Data', color:'#60a5fa'},
  defi:          {label:'DeFi',        color:'#34d399'},
  security:      {label:'Security',    color:'#f87171'},
  infra:         {label:'Infra',       color:'#fbbf24'},
  agent:         {label:'Agents',      color:'#f472b6'},
  data:          {label:'Data',        color:'#38bdf8'},
};

const BUILTIN = [
  {id:'solsignal', name:'SolSignal', category:'security', baseUrl:'https://solsignal-api.onrender.com', priceRange:'$0.01', status:'live',
    description:'Solana token safety scanner — DexScreener, RugCheck, GoPlus & Jupiter simulation into one SAFE/AVOID verdict.',
    endpoints:[{id:'scan',method:'GET',path:'/api/scan',params:[{name:'token',required:true}]}]},
  {id:'sentinel', name:'Sentinel', category:'security', baseUrl:'https://sentinel-awms.onrender.com', priceRange:'$0.005 – $0.025', status:'live',
    description:'Trust verification — protocol trust scoring, token safety, DeFi risk, OFAC screening.',
    endpoints:[{id:'trust-score',method:'GET',path:'/api/trust-score',params:[]},{id:'ofac-check',method:'GET',path:'/api/ofac-check',params:[]}]},
  {id:'cybera', name:'CYBERA Compliance', category:'security', baseUrl:'https://compliance-api-ruddy.vercel.app', priceRange:'$0.01', status:'live',
    description:'VASP address ID (20K+ addresses, 29 chains), risk scoring, sanctions & mixer screening.',
    endpoints:[{id:'identify',method:'GET',path:'/api/identify',params:[]}]},
  {id:'defi-signal', name:'DeFi Signal', category:'defi', baseUrl:'https://defi-signal-agent-production.up.railway.app', priceRange:'$0.01 – $0.10', status:'live',
    description:'New pool risk scoring (0–10), Solana whale alerts (>$100K), and Dune-enriched on-chain intel.',
    endpoints:[{id:'pool-risk',method:'GET',path:'/api/pool-risk',params:[]},{id:'whale-alerts',method:'GET',path:'/api/whale-alerts',params:[]}]},
  {id:'deepblue', name:'DeepBlue Trading', category:'defi', baseUrl:'https://api.deepbluebase.xyz', priceRange:'$0.01 – $0.05', status:'live',
    description:'AI crypto intelligence — live signals, prediction analytics, whale tracking.',
    endpoints:[{id:'signals',method:'GET',path:'/api/signals',params:[]},{id:'whales',method:'GET',path:'/api/whales',params:[]}]},
  {id:'moonmaker', name:'MoonMaker', category:'defi', baseUrl:'https://api.moonmaker.cc', priceRange:'$0.02 – $0.10', status:'live',
    description:'AI-native crypto data — signals, market context, DeFi regime detection, ETF flows, DEX alpha.',
    endpoints:[{id:'market-context',method:'GET',path:'/api/market-context',params:[]},{id:'dex-alpha',method:'GET',path:'/api/dex-alpha',params:[]}]},
  {id:'kerdos', name:'Kerdos Market Intel', category:'defi', baseUrl:'https://nonvisceral-eloisa-mousily.ngrok-free.dev', priceRange:'$0.01 – $0.05', status:'live',
    description:'Crypto sentiment scoring, BTC/ETH regime direction, funding rates, liquidation risk.',
    endpoints:[{id:'sentiment',method:'GET',path:'/api/sentiment',params:[]},{id:'funding-rates',method:'GET',path:'/api/funding-rates',params:[]}]},
  {id:'xquik', name:'Xquik', category:'data', baseUrl:'https://xquik.com', priceRange:'$0.001 – $0.01', status:'live',
    description:'Real-time X (Twitter) data API — tweet search, user profiles, trends.',
    endpoints:[{id:'search',method:'GET',path:'/api/search',params:[{name:'q',required:true}]},{id:'trends',method:'GET',path:'/api/trends',params:[]}]},
  {id:'agentfeed', name:'AgentFeed', category:'data', baseUrl:'https://agentfeed.io', priceRange:'$0.001 – $0.02', status:'live',
    description:'Real-time structured data feeds — market data, news, and social signals.',
    endpoints:[{id:'feed',method:'GET',path:'/api/feed',params:[{name:'type',required:true}]}]},
  {id:'gpu-bridge', name:'GPU-Bridge', category:'ai', baseUrl:'https://gpubridge.xyz', priceRange:'$0.00002 – $0.001', status:'live',
    description:'30-service GPU inference — LLM, image gen, embeddings, STT, TTS, PDF processing.',
    endpoints:[{id:'inference',method:'POST',path:'/api/inference',params:[{name:'model',required:true},{name:'prompt',required:true}]}]},
  {id:'x402engine', name:'x402engine', category:'infra', baseUrl:'https://x402engine.app', priceRange:'$0.001 – $0.50', status:'live',
    description:'74 endpoints: 44 LLMs, image/video gen, crypto data, web search, code execution, TTS, IPFS.',
    endpoints:[{id:'web-search',method:'GET',path:'/api/search',params:[{name:'q',required:true}]}]},
];

// ────────── skill (.md) parser — ported from the extension ──────────
function parseSkill(text){
  const lines = text.split('\n').map(l=>l.trim()).filter(Boolean);
  const p = {}; const desc = []; const params = []; let inParams = false;
  const pats = {skill:/^Skill:\s*(.+)/i,docs:/^Docs:\s*(.+)/i,
    endpoint:/^Endpoint:\s*(GET|POST|PUT|PATCH|DELETE)\s+(.+)/i,
    price:/^Price:\s*(.+)/i,model:/^Model:\s*(.+)/i,
    name:/^Name:\s*(.+)/i,category:/^Category:\s*(.+)/i,
    params:/^Params?:\s*(.+)/i};
  const parseParam = (s)=>{
    const m = s.match(/^(\w+)\s*\((\w+)(?:,\s*(required|optional))?\)\s*[-–—:]?\s*(.+)?/);
    if(!m) return null;
    return {name:m[1],type:m[2]==='number'?'number':'string',
      required:m[3]==='required',description:(m[4]||m[1]).trim()};
  };
  for(const line of lines){
    let matched = false;
    for(const [k,re] of Object.entries(pats)){
      const m = line.match(re);
      if(m){
        if(k==='endpoint'){p.method=m[1].toUpperCase();p.endpointUrl=m[2].trim();}
        else if(k==='params'){inParams=true;const pp=parseParam(m[1].trim());if(pp)params.push(pp);}
        else p[k]=m[1].trim();
        matched=true; break;
      }
    }
    if(!matched){
      if(inParams){const pp=parseParam(line); if(pp){params.push(pp); continue;} inParams=false;}
      desc.push(line);
    }
  }
  if(!p.endpointUrl) throw new Error('Missing "Endpoint:" line. Format: Endpoint: POST https://example.com/api/call');
  let baseUrl, path;
  try{const u=new URL(p.endpointUrl);baseUrl=`${u.protocol}//${u.host}`;path=u.pathname;}
  catch{const i=p.endpointUrl.indexOf('/',p.endpointUrl.indexOf('//')+2);
    baseUrl=i>0?p.endpointUrl.slice(0,i):p.endpointUrl; path=i>0?p.endpointUrl.slice(i):'/';}
  const description = desc.filter(l=>!l.startsWith('Use x402')&&!l.startsWith('Ask me')).join(' ').trim()
    || `x402 service at ${baseUrl}`;
  const allText = `${p.name||''} ${description} ${p.endpointUrl}`.toLowerCase();
  const guess = (()=>{
    const kw = {ai:['image','video','llm','generation','inference','model','ai','ml','gpu','tts','stt'],
      'crypto-data':['token','price','chart','market cap'],
      defi:['defi','swap','pool','yield','liquidity','trading','signal','whale','funding'],
      security:['security','scan','rug','trust','risk','ofac','compliance','safety'],
      infra:['infra','rpc','storage','ipfs','compute','engine'],
      agent:['agent','autonomous','bot'],
      data:['data','search','twitter','trend','news','feed','social','web search']};
    let best='data',score=0;
    for(const [c,ws] of Object.entries(kw)){
      const s = ws.filter(w=>allText.includes(w)).length;
      if(s>score){best=c;score=s;}
    }
    return best;
  })();
  const category = p.category ? p.category.toLowerCase() : guess;
  let deriveName = ()=>{
    try{const u = new URL(p.endpointUrl);
      const skip = new Set(['api','v1','v2','service','invoke','call','actions','run']);
      const parts = u.pathname.split('/').filter(Boolean).filter(x=>!skip.has(x.toLowerCase())&&x.length>1);
      const raw = parts.length>0 ? parts[parts.length-1] :
        u.hostname.replace(/^(www|api|registry)\./,'').split('.')[0];
      return raw.replace(/[-_]/g,' ').split(' ').map(w=>w[0].toUpperCase()+w.slice(1)).join(' ');
    }catch{return 'Custom Service';}
  };
  const name = p.name || deriveName();
  const id = `custom_${baseUrl.replace(/[^a-z0-9]/gi,'_')}_${path.replace(/[^a-z0-9]/gi,'_')}`.toLowerCase();
  const endpointId = path.split('/').filter(Boolean).pop() || 'call';
  return {id, name, category, baseUrl, priceRange: p.price||'varies', status:'live', isCustom:true,
    description, endpoints:[{id:endpointId, method:p.method==='GET'?'GET':'POST', path, params}]};
}

// ────────── state ──────────
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
let cfg = {model:'', personality:'', servicesEnabled:[], customServices:[]};
let filter = 'all';

// ────────── tabs ──────────
$$('.tab').forEach(t => t.onclick = () => {
  $$('.tab').forEach(x=>x.classList.remove('active'));
  $$('.panel').forEach(x=>x.classList.remove('active'));
  t.classList.add('active');
  $('#panel-'+t.dataset.tab).classList.add('active');
});

// ────────── status poll ──────────
let lastSeq = 0;
async function refresh(){
  try{ const j = await (await fetch('/state')).json();
    $('#status').textContent = j.status || '';
    // When the seq bumps the board has a new reply for us. Append it
    // and remember where we are so we don't re-render the same line.
    if (typeof j.seq === 'number' && j.seq > lastSeq) {
      lastSeq = j.seq;
      if (j.reply) addLog('daemon', j.reply);
    }
  }catch(e){}
}
setInterval(refresh, 1500); refresh();

// ────────── chat ──────────
function addLog(role, text){
  const el = document.createElement('div');
  el.className = role;
  el.innerHTML = `<b>${role==='you'?'you':'daemon'}:</b> ${text.replace(/</g,'&lt;')}`;
  $('#log').prepend(el);
}
async function sendUtterance(text){
  text = (text||'').trim(); if(!text) return;
  addLog('you', text); $('#status').textContent='thinking…';
  try{
    // Fire and forget — the board acks immediately and runs aiAsk on a
    // background task. refresh() will pick up the reply via /state when
    // the seq counter bumps.
    await fetch('/say', {method:'POST', headers:{'Content-Type':'text/plain'}, body:text});
    // Kick off a faster poll while we're waiting so the reply appears
    // sooner than the next 1.5 s tick.
    refresh();
  }catch(e){ $('#status').textContent='error: '+e; }
}
$('#send').onclick = () => { const t = $('#textin').value; $('#textin').value=''; sendUtterance(t); };
$('#textin').addEventListener('keydown', e => { if(e.key==='Enter') $('#send').click(); });

// mic
const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
const mic = $('#talk');
if(!SR){ mic.disabled = true; mic.textContent='no mic support'; mic.style.opacity=.5; }
else{
  let rec = null, active = false;
  mic.onclick = () => {
    if(active){ rec && rec.stop(); return; }
    rec = new SR(); rec.lang='en-US'; rec.interimResults=false; rec.maxAlternatives=1;
    rec.onstart = ()=>{active=true;mic.classList.add('listening');mic.textContent='LISTENING…';};
    rec.onerror = e=>{$('#status').textContent='mic error: '+e.error;};
    rec.onend = ()=>{active=false;mic.classList.remove('listening');mic.textContent='TAP TO TALK';};
    rec.onresult = e=>sendUtterance(e.results[0][0].transcript);
    rec.start();
  };
}

// ────────── settings ──────────
function populateModels(){
  const sel = $('#model'); sel.innerHTML = '';
  const groups = {};
  for(const m of MODELS){ (groups[m.provider] ||= []).push(m); }
  for(const [provider, list] of Object.entries(groups)){
    const og = document.createElement('optgroup');
    og.label = provider;
    for(const m of list){
      const opt = document.createElement('option');
      opt.value = m.id;
      opt.textContent = m.name + (provider==='Google' ? '' : ' • (x402 req.)');
      og.appendChild(opt);
    }
    sel.appendChild(og);
  }
}
populateModels();

async function loadCfg(){
  const r = await fetch('/config'); const j = await r.json();
  cfg = Object.assign(cfg, j);
  $('#model').value = cfg.model;
  $('#personality').value = cfg.personality || '';
  renderServices();
}

$('#saveSettings').onclick = async () => {
  cfg.model = $('#model').value;
  cfg.personality = $('#personality').value.trim();
  await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body: JSON.stringify({model:cfg.model, personality:cfg.personality})});
  $('#status').textContent = 'settings saved';
  setTimeout(()=>refresh(), 1500);
};
$('#resetPersona').onclick = () => { $('#personality').value = ''; };

// ────────── services ──────────
function svcCatChip(cat){
  const m = CATS[cat] || {label:'Custom',color:'#a78bfa'};
  return `<span class="cat" style="background:${m.color}22;color:${m.color}">${m.label}</span>`;
}

function renderFilters(){
  const c = $('#filters'); c.innerHTML = '';
  const mk = (id, label) => {
    const b = document.createElement('button');
    b.className = 'chip'+(filter===id?' on':'');
    b.textContent = label;
    b.onclick = () => { filter = filter===id ? 'all' : id; renderServices(); };
    c.appendChild(b);
  };
  mk('all', 'All');
  for(const [id, meta] of Object.entries(CATS)) mk(id, meta.label);
}

function allServices(){ return [...BUILTIN, ...(cfg.customServices||[])]; }

function renderServices(){
  renderFilters();
  const q = ($('#svcSearch').value||'').toLowerCase();
  const list = allServices().filter(s => {
    if(filter!=='all' && s.category!==filter) return false;
    if(q && !(s.name.toLowerCase().includes(q)||s.description.toLowerCase().includes(q))) return false;
    return true;
  });
  const box = $('#svclist'); box.innerHTML = '';
  for(const s of list){
    const row = document.createElement('div'); row.className = 'svcrow';
    const on = cfg.servicesEnabled.includes(s.id);
    row.innerHTML = `
      <button class="toggle ${on?'on':''}" data-id="${s.id}"></button>
      <div style="flex:1;min-width:0">
        <h4 class="svcname">${s.name}${s.isCustom?' <span class="badge-custom">custom</span>':''}</h4>
        <p class="svcdesc">${s.description}</p>
        <div class="meta">${svcCatChip(s.category)}
          <span>${s.endpoints.length} tool${s.endpoints.length!==1?'s':''}</span>
          <span style="margin-left:auto;font-family:ui-monospace,monospace">${s.priceRange}</span>
          ${s.isCustom?`<button class="rm" data-rm="${s.id}">remove</button>`:''}
        </div>
      </div>`;
    box.appendChild(row);
  }
  // wire toggles
  box.querySelectorAll('.toggle').forEach(b => b.onclick = async () => {
    const id = b.dataset.id;
    const idx = cfg.servicesEnabled.indexOf(id);
    if(idx>=0) cfg.servicesEnabled.splice(idx,1); else cfg.servicesEnabled.push(id);
    await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({servicesEnabled: cfg.servicesEnabled})});
    renderServices();
  });
  box.querySelectorAll('[data-rm]').forEach(b => b.onclick = async () => {
    const id = b.dataset.rm;
    cfg.customServices = cfg.customServices.filter(s=>s.id!==id);
    cfg.servicesEnabled = cfg.servicesEnabled.filter(x=>x!==id);
    await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({customServices:cfg.customServices, servicesEnabled:cfg.servicesEnabled})});
    renderServices();
  });
}
$('#svcSearch').addEventListener('input', renderServices);

// ────────── add-service modal ──────────
$('#addSvc').onclick = () => {
  const mask = document.createElement('div'); mask.className='mask';
  mask.innerHTML = `<div class="card">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
      <h3 style="margin:0">ADD X402 SERVICE</h3>
      <button class="btn ghost" id="close">close</button>
    </div>
    <p class="hint">Paste a skill definition or drop a .md file. Format:
    <code>Endpoint: POST https://…/api/call</code>,
    <code>Price: $0.01</code>, plus a description.</p>
    <textarea id="skillText" placeholder="Endpoint: POST https://example.com/api/call&#10;Price: $0.01&#10;&#10;Description of what this service does…"></textarea>
    <div class="row" style="margin-top:10px;gap:6px">
      <button class="btn ghost" id="parseBtn">parse</button>
      <button class="btn" id="commit" disabled>add &amp; enable</button>
    </div>
    <div id="err"></div>
    <div id="prev"></div>
  </div>`;
  document.body.appendChild(mask);
  const close = ()=>mask.remove();
  mask.querySelector('#close').onclick = close;
  mask.addEventListener('click', e=>{if(e.target===mask) close();});
  const ta = mask.querySelector('#skillText');
  ta.addEventListener('dragover', e=>e.preventDefault());
  ta.addEventListener('drop', async e => {
    e.preventDefault();
    const f = e.dataTransfer.files[0]; if(!f) return;
    ta.value = await f.text();
  });
  let preview = null;
  mask.querySelector('#parseBtn').onclick = () => {
    mask.querySelector('#err').innerHTML = '';
    mask.querySelector('#prev').innerHTML = '';
    try{
      preview = parseSkill(ta.value);
      const p = mask.querySelector('#prev');
      p.innerHTML = `<label>SERVICE NAME</label>
        <input id="nm" value="${preview.name.replace(/"/g,'&quot;')}"/>
        <div class="preview" style="margin-top:10px">${JSON.stringify(preview, null, 2)}</div>`;
      mask.querySelector('#commit').disabled = false;
    }catch(ex){
      mask.querySelector('#err').innerHTML = `<div class="error">${ex.message}</div>`;
    }
  };
  mask.querySelector('#commit').onclick = async () => {
    if(!preview) return;
    const nm = mask.querySelector('#nm').value.trim();
    if(nm) { preview.name = nm; preview.endpoints.forEach(e=>e.name=nm); }
    cfg.customServices = [...(cfg.customServices||[]).filter(s=>s.id!==preview.id), preview];
    if(!cfg.servicesEnabled.includes(preview.id)) cfg.servicesEnabled.push(preview.id);
    await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({customServices:cfg.customServices, servicesEnabled:cfg.servicesEnabled})});
    close();
    renderServices();
  };
};

// ────────── boot ──────────
loadCfg();
</script>
</body></html>
)HTML";

static void handleRoot() {
  s_http.send_P(200, "text/html", PAGE_HTML);
}

// Minimal JSON-string escaper — handles the characters plain chat text can
// plausibly contain (quotes, backslashes, control chars).
static String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { /* drop other control bytes */ }
    else out += c;
  }
  return out;
}

static void handleState() {
  String j;
  j.reserve(128 + s_lastUser.length() + s_lastReply.length());
  j  = "{\"status\":\"";    j += jsonEscape(s_status);     j += "\",";
  j += "\"seq\":";          j += String(s_replySeq);        j += ",";
  j += "\"lastUser\":\"";   j += jsonEscape(s_lastUser);   j += "\",";
  j += "\"reply\":\"";      j += jsonEscape(s_lastReply);  j += "\"}";
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
  // Dispatch to the worker task and acknowledge immediately. The main
  // loop keeps animating, touch stays responsive, and the web UI polls
  // /state for the reply.
  ensureSayWorker();
  char *buf = strdup(text.c_str());
  if (!buf) {
    s_http.send(503, "application/json", "{\"accepted\":false,\"error\":\"oom\"}");
    return;
  }
  if (xQueueSend(s_sayQueue, &buf, 0) != pdTRUE) {
    free(buf);
    s_http.send(503, "application/json", "{\"accepted\":false,\"error\":\"busy\"}");
    return;
  }
  // Record the pending utterance so /state reflects "you said …" right
  // away, and blank the last reply so the poller waits for the new one.
  s_lastUser  = text;
  s_lastReply = "";
  s_http.send(200, "application/json", "{\"accepted\":true}");
}

// ---------------------------------------------------------------------------
// /config — GET returns the full settings blob; POST updates any subset.
// JSON only. Keys:
//   model           "google/gemini-3.1-pro", etc.
//   personality     free-form system prompt (empty = use built-in default)
//   servicesEnabled string[] of enabled service ids
//   customServices  object[] of X402Service records added via .md parser
// ---------------------------------------------------------------------------
static void handleConfigGet() {
  String p = devcfgPersonality();
  p.replace("\\", "\\\\");
  p.replace("\"", "\\\"");
  p.replace("\n", "\\n");
  p.replace("\r", "\\r");
  p.replace("\t", "\\t");

  String out = "{";
  out += "\"model\":\""         + devcfgLlmModel()         + "\",";
  out += "\"personality\":\""   + p                         + "\",";
  out += "\"servicesEnabled\":" + devcfgServicesEnabled()   + ",";
  out += "\"customServices\":"  + devcfgCustomServices();
  out += "}";
  s_http.send(200, "application/json", out);
}

static void handleConfigPost() {
  if (!s_http.hasArg("plain")) { s_http.send(400, "text/plain", "no body"); return; }
  String body = s_http.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    s_http.send(400, "text/plain", "invalid JSON");
    return;
  }
  if (doc["model"].is<const char*>())       devcfgSetLlmModel(doc["model"].as<String>());
  if (doc["personality"].is<const char*>()) devcfgSetPersonality(doc["personality"].as<String>());
  if (doc["servicesEnabled"].is<JsonArray>()) {
    String ser; serializeJson(doc["servicesEnabled"], ser);
    devcfgSetServicesEnabled(ser);
  }
  if (doc["customServices"].is<JsonArray>()) {
    String ser; serializeJson(doc["customServices"], ser);
    devcfgSetCustomServices(ser);
  }
  s_http.send(200, "application/json", "{\"ok\":true}");
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

// Core connect: tries the given SSID/password and returns success/failure.
// Does not touch NVS; caller decides whether to persist.
static bool connectTo(const String &ssid, const String &password) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("daemon");
  WiFi.disconnect(true, true);
  delay(100);

  Serial.printf("wifi: connecting to '%s' ", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t start = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(300);
    wl_status_t st = WiFi.status();
    if (st != last) { Serial.printf("[%d]", (int)st); last = st; }
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

bool serverBeginWifi() {
  logNearbyNetworks();

  // 1. Try user-provided credentials from NVS (set via the settings UI).
  String ssid = devcfgWifiSSID();
  String pass = devcfgWifiPassword();
  if (ssid.length() > 0) {
    if (connectTo(ssid, pass)) return true;
    Serial.println("wifi: stored creds failed, trying secrets.h fallback");
  }

  // 2. Fall back to compile-time defaults in secrets.h.
  return connectTo(WIFI_SSID, WIFI_PASSWORD);
}

bool serverWifiConnect(const String &ssid, const String &password) {
  bool ok = connectTo(ssid, password);
  if (ok) devcfgSetWifi(ssid, password);
  return ok;
}

void serverWifiDisconnect() {
  Serial.println("wifi: user-initiated disconnect");
  WiFi.disconnect(true, true);
}

void serverBeginHttp(SayCallback onSay) {
  s_onSay = std::move(onSay);
  s_http.on("/",       HTTP_GET,  handleRoot);
  s_http.on("/state",  HTTP_GET,  handleState);
  s_http.on("/say",    HTTP_POST, handleSay);
  s_http.on("/config", HTTP_GET,  handleConfigGet);
  s_http.on("/config", HTTP_POST, handleConfigPost);
  s_http.begin();
  Serial.println("http: listening on :80");
}

void serverLoop()                       { s_http.handleClient(); }
void serverSetStatus(const String &s)   { s_status = s; }
void serverSetReply(const String &user, const String &reply) {
  s_lastUser  = user;
  s_lastReply = reply;
  s_replySeq++;                    // tell /state pollers this is a new reply
}
String serverLocalIP()                  { return WiFi.localIP().toString(); }
