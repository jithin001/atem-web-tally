/**
 * ATEM Web Tally — https://github.com/YOURNAME/atem-web-tally
 * MIT License · Built by Jithin Mathew (https://jithinmathew.com)
 *
 * - Single connection to the ATEM (atem-connection)
 * - MODES:
 *     readonly  (default) — the server NEVER sends commands to the ATEM.
 *     switcher  — authenticated users may set preview / cut / auto from the
 *                 admin. Requires a password, chosen during setup.
 * - First run serves a setup wizard (/setup.html): model, IP, input names,
 *   tally behavior, mode + password. /api/setup locks after completion.
 * - AUTH: scrypt-hashed password. Login issues an in-memory session token
 *   (12 h). When a password is configured, ALL mutating endpoints require it;
 *   viewing (state, tally pages, WebSocket) is always open.
 * - UDP: tally broadcast :7411 (server→devices), device status :7412
 *   (devices→server, unicast config reply). Devices self-register by MAC.
 */

'use strict';

const crypto = require('crypto');
const dgram = require('dgram');
const fs = require('fs');
const http = require('http');
const path = require('path');
const express = require('express');
const { WebSocketServer } = require('ws');
const { Atem } = require('atem-connection');

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
const CONFIG_PATH = path.join(__dirname, 'config.json');

const DEFAULT_CONFIG = {
  setupComplete: false,
  atemModel: '',
  atemIp: '192.168.1.240',
  cameraCount: 8,            // set by setup (e.g. 4 for ATEM Mini)
  mode: 'readonly',          // 'readonly' | 'switcher'
  auth: null,                // { salt, hash } — scrypt; null = no password
  httpPort: 3000,
  broadcastPort: 7411,
  statusPort: 7412,
  broadcastAddress: '255.255.255.255',
  heartbeatMs: 500,
  lowBatteryPct: 20,
  deviceDefaults: { pgmBright: 255, pvwBright: 60 },
  inputNameOverrides: {},
  devices: {}
};

function loadConfig() {
  if (!fs.existsSync(CONFIG_PATH)) {
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(DEFAULT_CONFIG, null, 2));
    console.log(`[config] wrote default config — setup wizard will run on first visit`);
  }
  return { ...DEFAULT_CONFIG, ...JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8')) };
}
function saveConfig() { fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2)); }

const config = loadConfig();
const DEBUG = process.env.DEBUG === '1';
const dbg = (...a) => { if (DEBUG) console.log('[debug]', ...a); };

// ---------------------------------------------------------------------------
// Auth (scrypt hash, in-memory session tokens)
// ---------------------------------------------------------------------------
const sessions = new Map(); // token -> expiry ms
const SESSION_TTL = 12 * 3600 * 1000;

function hashPassword(pw) {
  const salt = crypto.randomBytes(16).toString('hex');
  const hash = crypto.scryptSync(pw, salt, 64).toString('hex');
  return { salt, hash };
}
function verifyPassword(pw) {
  if (!config.auth) return false;
  const test = crypto.scryptSync(pw, config.auth.salt, 64);
  const real = Buffer.from(config.auth.hash, 'hex');
  return test.length === real.length && crypto.timingSafeEqual(test, real);
}
function tokenValid(req) {
  const h = req.headers.authorization || '';
  const token = h.startsWith('Bearer ') ? h.slice(7) : null;
  if (!token) return false;
  const exp = sessions.get(token);
  if (!exp || Date.now() > exp) { sessions.delete(token); return false; }
  return true;
}
// Mutations: open until a password is configured, then token-gated.
function requireAuthIfConfigured(req, res, next) {
  if (!config.auth || tokenValid(req)) return next();
  res.status(401).json({ error: 'auth required' });
}
// Strict: always requires a valid token (switching, re-running setup).
function requireAuthStrict(req, res, next) {
  if (config.auth && tokenValid(req)) return next();
  res.status(401).json({ error: 'auth required' });
}

// ---------------------------------------------------------------------------
// Tally state
// ---------------------------------------------------------------------------
let tally = new Uint8Array(config.cameraCount);
let seq = 0;
let atemConnected = false;
const liveDevices = new Map();

// ---------------------------------------------------------------------------
// ATEM connection (read path; command path is gated far below)
// ---------------------------------------------------------------------------
const atem = new Atem();

function recomputeTally(state) {
  const next = new Uint8Array(config.cameraCount);
  const mes = (state.video && state.video.mixEffects) || [];
  for (const me of mes) {
    if (!me) continue;
    const pgm = me.programInput;
    const pvw = me.previewInput;
    const inTransition = !!(me.transitionPosition && me.transitionPosition.inTransition);
    if (pgm >= 1 && pgm <= config.cameraCount) next[pgm - 1] |= 0x01;
    if (pvw >= 1 && pvw <= config.cameraCount) {
      next[pvw - 1] |= 0x02;
      if (inTransition) next[pvw - 1] |= 0x01;   // preview is hot mid-transition
    }
  }
  const changed = !next.every((v, i) => v === tally[i]);
  tally = next;
  if (changed) { broadcastTally(); pushToWebClients(); }
}

atem.on('connected', () => {
  atemConnected = true;
  console.log(`[atem] connected to ${config.atemIp}`);
  recomputeTally(atem.state);
  pushToWebClients();
});
atem.on('disconnected', () => {
  atemConnected = false;
  console.warn('[atem] disconnected — retrying automatically');
  pushToWebClients();
});
atem.on('stateChanged', (state) => recomputeTally(state));
atem.on('error', (e) => console.error('[atem] error:', e));

if (config.setupComplete) atem.connect(config.atemIp);
else console.log('[setup] waiting for setup wizard before connecting to an ATEM');

// ---------------------------------------------------------------------------
// UDP: tally broadcast + device status
// ---------------------------------------------------------------------------
const txSock = dgram.createSocket('udp4');
txSock.bind(() => txSock.setBroadcast(true));

function broadcastTally() {
  seq = (seq + 1) & 0xff;
  const buf = Buffer.alloc(4 + config.cameraCount);
  buf.write('T1', 0, 'ascii');
  buf[2] = seq;
  buf[3] = config.cameraCount;
  Buffer.from(tally).copy(buf, 4);
  txSock.send(buf, config.broadcastPort, config.broadcastAddress, (err) => {
    if (err) console.error('[udp] broadcast error:', err.message);
  });
}
setInterval(broadcastTally, config.heartbeatMs);

function inputNames() {
  const names = {};
  const overrides = config.inputNameOverrides || {};
  const inputs = (atemConnected && atem.state && atem.state.inputs) || {};
  for (let i = 1; i <= config.cameraCount; i++) {
    const inp = inputs[i];
    names[i] = overrides[i] || (inp && (inp.longName || inp.shortName)) || `Input ${i}`;
  }
  return names;
}

const rxSock = dgram.createSocket('udp4');
rxSock.on('message', (msg, rinfo) => {
  let report;
  try { report = JSON.parse(msg.toString('utf8')); } catch { return; }
  if (!report.mac) return;
  const mac = String(report.mac).toUpperCase();

  if (!config.devices[mac]) {
    const d = config.deviceDefaults || {};
    config.devices[mac] = { camera: 0, name: `New device ${mac.slice(-5)}`,
      pgmBright: d.pgmBright ?? 255, pvwBright: d.pvwBright ?? 60 };
    saveConfig();
    console.log(`[device] new device registered: ${mac} from ${rinfo.address}`);
  }
  const devCfg = config.devices[mac];

  if (typeof report.setCam === 'number') {   // on-device button assignment
    const cam = Math.max(0, Math.min(config.cameraCount, Math.floor(report.setCam)));
    if (cam !== devCfg.camera) {
      devCfg.camera = cam;
      saveConfig();
      console.log(`[device] ${devCfg.name} (${mac}) self-assigned to input ${cam || 'none'} via button`);
    }
  }

  liveDevices.set(mac, {
    ...devCfg,
    batt: report.batt ?? null, etaMin: report.eta ?? null, rssi: report.rssi ?? null,
    uptime: report.up ?? null, fw: report.fw ?? null, ip: rinfo.address, lastSeen: Date.now()
  });

  if (report.batt != null && report.batt >= 0 && report.batt <= config.lowBatteryPct) {
    console.warn(`[battery] LOW: ${devCfg.name} (${mac}) at ${report.batt}%${report.eta ? `, ~${report.eta} min left` : ''}`);
  }

  const reply = Buffer.from(JSON.stringify({
    cam: devCfg.camera, name: devCfg.name,
    pgmBright: devCfg.pgmBright ?? 255, pvwBright: devCfg.pvwBright ?? 60,
    hbMs: config.heartbeatMs, maxCam: config.cameraCount, inputs: inputNames()
  }));
  rxSock.send(reply, rinfo.port, rinfo.address);
  dbg(`status from ${mac} batt=${report.batt}% -> cam ${devCfg.camera}`);
  pushToWebClients();
});
rxSock.bind(config.statusPort, () => {
  console.log(`[udp] tally broadcast on :${config.broadcastPort}, device status on :${config.statusPort}`);
});

// ---------------------------------------------------------------------------
// Web
// ---------------------------------------------------------------------------
const app = express();
app.use(express.json());

// Until setup completes, route page requests to the wizard.
app.use((req, res, next) => {
  if (!config.setupComplete
      && (req.path === '/' || req.path === '/admin.html' || req.path === '/tally.html')) {
    return res.redirect('/setup.html');
  }
  if (config.setupComplete && req.path === '/') return res.redirect('/tally.html');
  next();
});
app.use(express.static(path.join(__dirname, 'public')));

function stateSnapshot() {
  return {
    type: 'state',
    setupComplete: config.setupComplete,
    mode: config.mode,
    authConfigured: !!config.auth,
    atemConnected,
    atemIp: config.atemIp,
    atemModel: config.atemModel || '',
    cameraCount: config.cameraCount,
    lowBatteryPct: config.lowBatteryPct,
    inputNames: inputNames(),
    tally: [...tally],
    devices: Object.fromEntries(
      Object.entries(config.devices).map(([mac, devCfg]) => {
        const live = liveDevices.get(mac);
        return [mac, {
          ...devCfg,
          batt: live?.batt ?? null, etaMin: live?.etaMin ?? null, rssi: live?.rssi ?? null,
          ip: live?.ip ?? null, lastSeen: live?.lastSeen ?? null,
          online: !!(live && Date.now() - live.lastSeen < 90000)
        }];
      })
    )
  };
}

// ---- Setup ----------------------------------------------------------------
function setupAllowed(req) {
  return !config.setupComplete || (config.auth && tokenValid(req));
}

app.post('/api/setup/test', async (req, res) => {
  if (!setupAllowed(req)) return res.status(403).json({ error: 'setup locked' });
  const ip = String(req.body.atemIp || '').trim();
  if (!ip) return res.status(400).json({ error: 'no IP' });
  const probe = new Atem();
  let done = false;
  const finish = (ok, detail) => {
    if (done) return; done = true;
    probe.disconnect().catch(() => {});
    res.json({ ok, detail });
  };
  probe.on('connected', () => {
    const model = probe.state?.info?.productIdentifier || 'ATEM';
    finish(true, model);
  });
  probe.on('error', () => {});
  setTimeout(() => finish(false, 'no response in 5 s'), 5000);
  probe.connect(ip).catch(() => finish(false, 'connection failed'));
});

app.post('/api/setup', (req, res) => {
  if (!setupAllowed(req)) return res.status(403).json({ error: 'setup locked — log in to re-run' });
  const b = req.body || {};
  const mode = b.mode === 'switcher' ? 'switcher' : 'readonly';
  const pw = typeof b.password === 'string' ? b.password : '';

  if (mode === 'switcher' && pw.length < 6) {
    return res.status(400).json({ error: 'switcher mode requires a password (min 6 chars)' });
  }
  config.atemModel = String(b.atemModel || '');
  config.atemIp = String(b.atemIp || config.atemIp).trim();
  config.cameraCount = Math.max(1, Math.min(40, Number(b.cameraCount) || 4));
  config.mode = mode;
  config.inputNameOverrides = {};
  if (b.inputNames && typeof b.inputNames === 'object') {
    for (const [k, v] of Object.entries(b.inputNames)) {
      const n = Number(k);
      const name = String(v || '').trim();
      if (n >= 1 && n <= config.cameraCount && name) config.inputNameOverrides[n] = name;
    }
  }
  if (b.behavior && typeof b.behavior === 'object') {
    config.deviceDefaults = {
      pgmBright: Math.min(255, Math.max(1, Number(b.behavior.pgmBright) || 255)),
      pvwBright: Math.min(255, Math.max(1, Number(b.behavior.pvwBright) || 60))
    };
    if (b.behavior.lowBatteryPct) config.lowBatteryPct = Math.min(90, Math.max(5, Number(b.behavior.lowBatteryPct)));
  }
  if (pw) config.auth = hashPassword(pw);
  else if (mode === 'readonly' && b.clearPassword) config.auth = null;

  config.setupComplete = true;
  tally = new Uint8Array(config.cameraCount);
  saveConfig();
  atem.disconnect().finally(() => atem.connect(config.atemIp));
  sessions.clear();
  pushToWebClients();
  console.log(`[setup] complete — mode=${config.mode}, atem=${config.atemIp}, cams=${config.cameraCount}, auth=${config.auth ? 'on' : 'off'}`);
  res.json({ ok: true });
});

// ---- Auth -----------------------------------------------------------------
app.post('/api/auth/login', (req, res) => {
  if (!config.auth) return res.status(400).json({ error: 'no password configured' });
  if (!verifyPassword(String(req.body.password || ''))) {
    return res.status(401).json({ error: 'wrong password' });
  }
  const token = crypto.randomBytes(24).toString('hex');
  sessions.set(token, Date.now() + SESSION_TTL);
  res.json({ ok: true, token });
});
app.get('/api/auth/check', (req, res) => res.json({ ok: tokenValid(req) }));

// ---- Read APIs (always open) ----------------------------------------------
app.get('/api/state', (req, res) => res.json(stateSnapshot()));

// ---- Mutations (token-gated once a password exists) -----------------------
app.post('/api/devices/:mac', requireAuthIfConfigured, (req, res) => {
  const mac = req.params.mac.toUpperCase();
  if (!config.devices[mac]) return res.status(404).json({ error: 'unknown device' });
  const { camera, name, pgmBright, pvwBright } = req.body;
  if (camera !== undefined) config.devices[mac].camera = Number(camera) || 0;
  if (name !== undefined) config.devices[mac].name = String(name);
  if (pgmBright !== undefined) config.devices[mac].pgmBright = Math.min(255, Math.max(1, Number(pgmBright)));
  if (pvwBright !== undefined) config.devices[mac].pvwBright = Math.min(255, Math.max(1, Number(pvwBright)));
  saveConfig(); pushToWebClients();
  res.json({ ok: true, device: config.devices[mac] });
});

app.delete('/api/devices/:mac', requireAuthIfConfigured, (req, res) => {
  const mac = req.params.mac.toUpperCase();
  delete config.devices[mac];
  liveDevices.delete(mac);
  saveConfig(); pushToWebClients();
  res.json({ ok: true });
});

app.post('/api/inputs/:n', requireAuthIfConfigured, (req, res) => {
  const n = Number(req.params.n);
  if (!(n >= 1 && n <= config.cameraCount)) return res.status(400).json({ error: 'bad input number' });
  config.inputNameOverrides = config.inputNameOverrides || {};
  const name = String(req.body.name || '').trim();
  if (name) config.inputNameOverrides[n] = name;
  else delete config.inputNameOverrides[n];
  saveConfig(); pushToWebClients();
  res.json({ ok: true, inputNames: inputNames() });
});

app.post('/api/atem', requireAuthIfConfigured, (req, res) => {
  if (req.body.atemIp) {
    config.atemIp = String(req.body.atemIp);
    saveConfig();
    atem.disconnect().finally(() => atem.connect(config.atemIp));
  }
  res.json({ ok: true, atemIp: config.atemIp });
});

// ---- Switcher mode (SOLE command path to the ATEM) ------------------------
// Guarded twice: config.mode must be 'switcher' AND the request must carry a
// valid session token. In readonly mode this endpoint refuses outright, so a
// readonly install sends zero commands to the switcher, ever.
app.post('/api/switch', requireAuthStrict, async (req, res) => {
  if (config.mode !== 'switcher') return res.status(403).json({ error: 'server is in read-only mode' });
  if (!atemConnected) return res.status(409).json({ error: 'ATEM not connected' });
  try {
    const { preview, action } = req.body;
    if (preview !== undefined) {
      const n = Number(preview);
      if (!(n >= 1 && n <= config.cameraCount)) return res.status(400).json({ error: 'bad input' });
      await atem.changePreviewInput(n);
    } else if (action === 'cut') {
      await atem.cut();
    } else if (action === 'auto') {
      await atem.autoTransition();
    } else {
      return res.status(400).json({ error: 'nothing to do' });
    }
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ---------------------------------------------------------------------------
const httpServer = http.createServer(app);
const wss = new WebSocketServer({ server: httpServer, path: '/ws' });

function pushToWebClients() {
  const payload = JSON.stringify(stateSnapshot());
  for (const client of wss.clients) if (client.readyState === 1) client.send(payload);
}
wss.on('connection', (ws) => ws.send(JSON.stringify(stateSnapshot())));
setInterval(pushToWebClients, 2000);

httpServer.listen(config.httpPort, () => {
  console.log('ATEM Web Tally - MIT - jithinmathew.com');
  if (!config.setupComplete) console.log(`[setup] open http://<server-ip>:${config.httpPort}/setup.html to begin`);
  console.log(`[web] admin:     http://<server-ip>:${config.httpPort}/admin.html`);
  console.log(`[web] web tally: http://<server-ip>:${config.httpPort}/tally.html`);
});
