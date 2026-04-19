#include "server.h"
#include "secrets.h"
#include "devcfg.h"
#include "memory.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

static WebServer      s_http(80);
static SayCallback    s_onSay;
static String         s_status = "Booting...";
static String         s_lastUser;
static String         s_lastReply;
// Incremented every time `serverSetReply()` gets called with non-empty
// reply text. The browser polls /state and fetches the new reply when it
// sees this counter advance — the HTTP /say endpoint no longer blocks
// waiting for the LLM, so the reply arrives asynchronously via polling.
static uint32_t       s_replyId = 0;

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
  <div class="card">
    <h3>MEMORY</h3>
    <p class="hint">Store encrypted chat memory on <b>Arweave</b> (via the Irys bundler) and replay it on boot so Daemon remembers you across reboots and flashes. Each completed exchange (one user turn + one Daemon reply) is AES-256-GCM encrypted with a key derived from your wallet — only you can decrypt. Tagged with <code>App-Name=daemon</code> and your pubkey so the device can pull them back in one GraphQL query.</p>
    <p class="hint"><b>Cost: $0.00</b> for the vast majority of exchanges. Uploads under 100 KiB are free on Irys, and every real chat memo is well under 1 KiB. Larger payloads would be paid in SOL from this wallet.</p>
    <div class="row" style="align-items:center;gap:10px;margin-bottom:6px">
      <button class="toggle" id="arToggle" aria-label="memory on/off"></button>
      <span id="arStatus" style="font-size:12px;color:var(--dim)">off</span>
    </div>
    <p class="hint" id="arStats" style="margin:4px 0 0;font-family:ui-monospace,monospace;font-size:11px"></p>
  </div>
  <div class="card">
    <h3>HEARTBEAT</h3>
    <p class="hint">Run a prompt on a schedule. Daemon fetches the reply in the background and speaks it aloud. Each tick is a normal chat call so the wallet pays the usual USDC.</p>
    <div class="row" style="align-items:center;gap:10px;margin-bottom:8px">
      <button class="toggle" id="hbToggle" aria-label="heartbeat on/off"></button>
      <span id="hbStatus" style="font-size:12px;color:var(--dim)">off</span>
    </div>
    <label>PROMPT</label>
    <textarea id="hbPrompt" rows="3" placeholder="e.g. What's the weather in San Francisco right now?"></textarea>
    <label>INTERVAL (minutes)</label>
    <input id="hbInterval" type="number" min="1" max="1440" placeholder="5"/>
    <div class="row" style="margin-top:10px;justify-content:flex-end">
      <button class="btn" id="saveHeartbeat">save heartbeat</button>
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
// Known API parameters we can infer from the description even when the
// skill file doesn't declare a Params: block. Grouped by trigger keywords:
// if any of the keywords appear in the description, ALL fields in that
// group are added as optional hints — snake_case AND camelCase — so the
// LLM sees both conventions and picks whichever the docs imply.
const PARAM_GROUPS = [
  {
    triggers: ['username', 'user id', 'user_id', 'handle', 'screen name', 'twitter', 'x.com'],
    fields:   [['username','string'],['userName','string'],
               ['user_id','string'],['userId','string'],
               ['handle','string'],['screen_name','string']],
  },
  {
    triggers: ['address', 'wallet', 'pubkey', 'vasp'],
    fields:   [['address','string'],['wallet','string'],['chain','string']],
  },
  {
    triggers: ['token', 'mint', 'symbol', 'coin'],
    fields:   [['token','string'],['mint','string'],['symbol','string']],
  },
  {
    triggers: ['search', 'query', 'lookup', 'find'],
    fields:   [['q','string'],['query','string']],
  },
  {
    triggers: ['prompt', 'generate', 'generation', 'image', 'video'],
    fields:   [['prompt','string'],['model','string'],['aspect_ratio','string']],
  },
  {
    triggers: ['paginat', 'cursor', 'next page', 'limit'],
    fields:   [['cursor','string'],['limit','number']],
  },
  {
    triggers: ['replies', 'reply', 'include'],
    fields:   [['include_replies','string'],['includeReplies','string']],
  },
  {
    triggers: ['url', 'link', 'website'],
    fields:   [['url','string']],
  },
];

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

  // If the skill didn't declare any params, scan the description for
  // trigger keywords and add the associated field hints as optional
  // params. We include both snake_case and camelCase spellings of every
  // inferred field so the LLM can pick whichever the endpoint expects.
  if(params.length === 0){
    const lower = `${name} ${description} ${path}`.toLowerCase();
    const seen = new Set();
    for(const group of PARAM_GROUPS){
      const hit = group.triggers.some(t => lower.includes(t));
      if(!hit) continue;
      for(const [fname, ftype] of group.fields){
        if(seen.has(fname)) continue;
        seen.add(fname);
        params.push({
          name: fname, type: ftype, required: false,
          description: `${fname} (inferred)`,
        });
      }
    }
  }

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
// The LLM now runs on a worker task so /say returns immediately. The
// browser polls /state periodically, and when it notices `replyId` has
// advanced past the last one we saw, it logs Daemon's reply.
let lastReplyId = 0;
async function refresh(){
  try{
    const j = await (await fetch('/state')).json();
    $('#status').textContent = j.status || '';
    if(j.replyId !== undefined && j.replyId !== lastReplyId && j.reply){
      lastReplyId = j.replyId;
      addLog('daemon', j.reply);
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
    // /say is now fire-and-forget (202 Accepted). Actual reply arrives
    // via the /state poll loop above.
    await fetch('/say', {method:'POST', headers:{'Content-Type':'text/plain'}, body:text});
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

function renderMemory(stats){
  const on = !!cfg.arweaveEnabled;
  $('#arToggle').classList.toggle('on', on);
  $('#arStatus').textContent = on ? 'on' : 'off';
  if(stats){
    const arAgo = stats.arweaveLastMs > 0 ? Math.round(stats.arweaveLastMs / 1000) + ' s uptime' : 'never';
    const txHtml = stats.arweaveLastTxId
      ? `<a href="https://gateway.irys.xyz/${stats.arweaveLastTxId}" target="_blank" style="color:var(--accent);font-family:ui-monospace,monospace">${stats.arweaveLastTxId.substring(0,10)}…</a>`
      : '<span style="color:var(--dim)">—</span>';
    $('#arStats').innerHTML =
      `stored: <b>${stats.stored}</b>  ·  uploaded this session: <b>${stats.arweaveWritten}</b>  ·  failed: <b>${stats.arweaveFailed}</b>  ·  last: ${arAgo}<br>latest tx: ${txHtml}`;
  } else {
    $('#arStats').textContent = '';
  }
}

async function refreshMemoryStats(){
  try{
    const r = await fetch('/memory');
    const j = await r.json();
    renderMemory(j);
  }catch(e){}
}
setInterval(() => { if(cfg.arweaveEnabled) refreshMemoryStats(); }, 5000);

$('#arToggle').onclick = async () => {
  cfg.arweaveEnabled = !cfg.arweaveEnabled;
  renderMemory();
  await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body: JSON.stringify({arweaveEnabled: cfg.arweaveEnabled})});
  if(cfg.arweaveEnabled) refreshMemoryStats();
};

function renderHeartbeat(){
  const on = !!cfg.heartbeatEnabled;
  $('#hbToggle').classList.toggle('on', on);
  $('#hbStatus').textContent = on ? 'on' : 'off';
  $('#hbPrompt').value = cfg.heartbeatPrompt || '';
  $('#hbInterval').value = cfg.heartbeatIntervalMin || 5;
}

async function loadCfg(){
  const r = await fetch('/config'); const j = await r.json();
  cfg = Object.assign(cfg, j);
  $('#model').value = cfg.model;
  $('#personality').value = cfg.personality || '';
  renderMemory();
  if(cfg.memoryEnabled) refreshMemoryStats();
  renderHeartbeat();
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

$('#hbToggle').onclick = async () => {
  cfg.heartbeatEnabled = !cfg.heartbeatEnabled;
  renderHeartbeat();
  await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body: JSON.stringify({heartbeatEnabled: cfg.heartbeatEnabled})});
};
$('#saveHeartbeat').onclick = async () => {
  const prompt = $('#hbPrompt').value.trim();
  const interval = Math.max(1, Math.min(1440, parseInt($('#hbInterval').value) || 5));
  cfg.heartbeatPrompt = prompt;
  cfg.heartbeatIntervalMin = interval;
  await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body: JSON.stringify({heartbeatPrompt: prompt, heartbeatIntervalMin: interval})});
  $('#status').textContent = 'heartbeat saved';
  setTimeout(()=>refresh(), 1500);
};

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
    <p class="hint">Paste a skill definition or drop a .md file. Must have at least an <code>Endpoint:</code> line. Description is used to infer sensible parameter names for the LLM.</p>
    <textarea id="skillText" placeholder="Endpoint: POST https://example.com/api/call&#10;Price: $0.01&#10;&#10;Description of what this service does…"></textarea>
    <div class="row" style="margin-top:10px;gap:6px;justify-content:flex-end">
      <button class="btn" id="commit">add &amp; enable</button>
    </div>
    <div id="err"></div>
    <div id="prev"></div>
  </div>`;
  document.body.appendChild(mask);
  const close = ()=>mask.remove();
  mask.querySelector('#close').onclick = close;
  mask.addEventListener('click', e=>{if(e.target===mask) close();});

  const ta = mask.querySelector('#skillText');
  const errEl = mask.querySelector('#err');
  const prevEl = mask.querySelector('#prev');
  let preview = null;

  const reparse = () => {
    errEl.innerHTML = '';
    const text = ta.value.trim();
    if(!text){ preview = null; prevEl.innerHTML = ''; return; }
    try{
      preview = parseSkill(text);
      prevEl.innerHTML = `<label style="margin-top:12px">SERVICE NAME</label>
        <input id="nm" value="${preview.name.replace(/"/g,'&quot;')}"/>
        <div class="preview" style="margin-top:10px">${JSON.stringify(preview,null,2)}</div>`;
    }catch(ex){
      preview = null;
      prevEl.innerHTML = '';
      errEl.innerHTML = `<div class="error">${ex.message}</div>`;
    }
  };

  // Live parse as the user types or pastes; also on drag-drop of an .md file.
  ta.addEventListener('input', reparse);
  ta.addEventListener('dragover', e=>e.preventDefault());
  ta.addEventListener('drop', async e => {
    e.preventDefault();
    const f = e.dataTransfer.files[0]; if(!f) return;
    ta.value = await f.text();
    reparse();
  });

  mask.querySelector('#commit').onclick = async () => {
    reparse();                                          // make sure preview is fresh
    if(!preview){
      errEl.innerHTML = '<div class="error">nothing to save — paste a skill first</div>';
      return;
    }
    try{
      const nmEl = mask.querySelector('#nm');
      const nm = (nmEl && nmEl.value ? nmEl.value : preview.name).trim();
      if(nm){ preview.name = nm; preview.endpoints.forEach(e=>e.name=nm); }
      cfg.customServices = [...(cfg.customServices||[]).filter(s=>s.id!==preview.id), preview];
      if(!cfg.servicesEnabled.includes(preview.id)) cfg.servicesEnabled.push(preview.id);
      const body = JSON.stringify({customServices:cfg.customServices, servicesEnabled:cfg.servicesEnabled});
      const r = await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'}, body});
      if(!r.ok) throw new Error('server returned '+r.status);
      const j = await r.json().catch(()=>({}));
      if(j.ok === false) throw new Error('server rejected the save');
      close();
      renderServices();
    }catch(ex){
      errEl.innerHTML = `<div class="error">save failed: ${ex.message||ex}</div>`;
    }
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

static String jsonEscape(const String &in) {
  String out = in;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  out.replace("\t", "\\t");
  return out;
}

static void handleState() {
  // The browser polls this ~every 1.5 s. We now ship the last reply +
  // replyId counter so the UI picks up the assistant's answer
  // asynchronously instead of blocking inside /say.
  String j = "{";
  j += "\"status\":\""  + jsonEscape(s_status)    + "\",";
  j += "\"user\":\""    + jsonEscape(s_lastUser)  + "\",";
  j += "\"reply\":\""   + jsonEscape(s_lastReply) + "\",";
  j += "\"replyId\":"   + String(s_replyId);
  j += "}";
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
  // Handler returns immediately — the LLM round-trip runs on the worker
  // task. Browser picks up the reply via /state polling.
  if (s_onSay) s_onSay(text);
  s_http.send(202, "application/json", "{\"accepted\":true}");
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

  // Escape the heartbeat prompt the same way.
  String hb = devcfgHeartbeatPrompt();
  hb.replace("\\", "\\\\");
  hb.replace("\"", "\\\"");
  hb.replace("\n", "\\n");
  hb.replace("\r", "\\r");
  hb.replace("\t", "\\t");

  String out = "{";
  out += "\"model\":\""         + devcfgLlmModel()         + "\",";
  out += "\"personality\":\""   + p                         + "\",";
  out += "\"servicesEnabled\":" + devcfgServicesEnabled()   + ",";
  out += "\"customServices\":"  + devcfgCustomServices()    + ",";
  out += "\"heartbeatEnabled\":"    + String(devcfgHeartbeatEnabled() ? "true" : "false") + ",";
  out += "\"heartbeatPrompt\":\""   + hb                        + "\",";
  out += "\"heartbeatIntervalMin\":" + String(devcfgHeartbeatIntervalMin()) + ",";
  out += "\"memoryEnabled\":"       + String(devcfgMemoryEnabled() ? "true" : "false") + ",";
  out += "\"arweaveEnabled\":"      + String(devcfgArweaveEnabled() ? "true" : "false");
  out += "}";
  s_http.send(200, "application/json", out);
}

static void handleConfigPost() {
  if (!s_http.hasArg("plain")) {
    s_http.send(400, "application/json",
                "{\"ok\":false,\"error\":\"no body\"}");
    return;
  }
  String body = s_http.arg("plain");
  Serial.printf("http: /config POST (%u bytes)\n", (unsigned)body.length());

  // Custom services include a bazaar-like schema that can nest a few levels
  // deeper than the ArduinoJson default; bump the limit so deserialization
  // doesn't silently fail on reasonable skill files.
  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, body, DeserializationOption::NestingLimit(24));
  if (err) {
    Serial.printf("http: /config bad json (%s)\n", err.c_str());
    String msg = String("{\"ok\":false,\"error\":\"invalid JSON: ") +
                 err.c_str() + "\"}";
    s_http.send(400, "application/json", msg);
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
    Serial.printf("http: stored customServices (%u bytes)\n", (unsigned)ser.length());
  }
  if (doc["heartbeatEnabled"].is<bool>())
    devcfgSetHeartbeatEnabled(doc["heartbeatEnabled"].as<bool>());
  if (doc["heartbeatPrompt"].is<const char*>())
    devcfgSetHeartbeatPrompt(doc["heartbeatPrompt"].as<String>());
  if (doc["heartbeatIntervalMin"].is<int>())
    devcfgSetHeartbeatIntervalMin((uint32_t)doc["heartbeatIntervalMin"].as<int>());
  if (doc["memoryEnabled"].is<bool>())
    devcfgSetMemoryEnabled(doc["memoryEnabled"].as<bool>());
  if (doc["arweaveEnabled"].is<bool>())
    devcfgSetArweaveEnabled(doc["arweaveEnabled"].as<bool>());
  s_http.send(200, "application/json", "{\"ok\":true}");
}

static void handleMemoryGet() {
  MemoryStats st;
  memoryGetStats(st);
  // Report seconds-since-last-write so the browser can show "X seconds ago".
  uint32_t memAge = (st.lastWriteMs  == 0) ? 0 : ((millis() - st.lastWriteMs)  / 1000);
  uint32_t arAge  = (st.arweaveLastMs == 0) ? 0 : ((millis() - st.arweaveLastMs) / 1000);
  String out = "{";
  out += "\"enabled\":"        + String(st.enabled        ? "true" : "false") + ",";
  out += "\"keyReady\":"       + String(st.keyReady       ? "true" : "false") + ",";
  out += "\"stored\":"         + String(st.stored)        + ",";
  out += "\"pending\":"        + String(st.pending)       + ",";
  out += "\"written\":"        + String(st.written)       + ",";
  out += "\"failed\":"         + String(st.failed)        + ",";
  out += "\"lastWriteMs\":"    + String(memAge)           + ",";
  out += "\"arweaveEnabled\":" + String(st.arweaveEnabled ? "true" : "false") + ",";
  out += "\"arweaveWritten\":" + String(st.arweaveWritten)+ ",";
  out += "\"arweaveFailed\":"  + String(st.arweaveFailed) + ",";
  out += "\"arweaveLastMs\":"  + String(arAge)            + ",";
  out += "\"arweaveLastTxId\":\"" + st.arweaveLastTxId     + "\"";
  out += "}";
  s_http.send(200, "application/json", out);
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
  s_http.on("/memory", HTTP_GET,  handleMemoryGet);
  s_http.begin();
  Serial.println("http: listening on :80");
}

void serverLoop()                       { s_http.handleClient(); }
void serverSetStatus(const String &s)   { s_status = s; }
void serverSetReply(const String &user, const String &reply) {
  s_lastUser  = user;
  s_lastReply = reply;
  // Bump the counter only when we have an actual reply so the browser
  // doesn't re-render empty placeholders. Clearing to "" between turns
  // is still useful for the UI state but shouldn't trigger a redraw.
  if (reply.length() > 0) s_replyId++;
}
String serverLocalIP()                  { return WiFi.localIP().toString(); }
