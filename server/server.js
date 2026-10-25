// Web server and virtual LAN relay for the Land of the Dead web port (research and educational use only).
//
//   node server.js [--port 8080] [--game ../build_web/out]        LOTD_PASSWORD=secret  -> HTTP basic auth
//
// - Serves the web build (index.html, index.js, index.wasm, vnet.js and, when present, the private data package
//   game.data/game.js) with gzip and ETags.
// - /net is a WebSocket per page: the page joins a room (?room=&name=) and gets an address 10.77.0.x in it. Binary
//   messages carry UDP datagrams (web/vnet.js has the format); the relay forwards each to the page with the
//   destination address, or to every other page in the room for a broadcast. So the game's own LAN games work
//   between browsers: one player hosts from "Host Multiplayer Game", the others find it under "Join".
// - /rooms lists the rooms and who is in them.
// No dependencies: the WebSocket protocol (RFC 6455) is implemented below.
'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 && args[i + 1] ? args[i + 1] : def; };
const PORT = Number(opt('port', process.env.PORT || 8080));
const GAME_DIR = path.resolve(__dirname, opt('game', '../build_web/out'));
const CACHE_DIR = path.resolve(__dirname, 'cache');
const PASSWORD = process.env.LOTD_PASSWORD || '';
const MAX_ROOM = 16;              // players per room (the game's own limit is lower)
const MAX_MSGS_PER_SEC = 3000;    // per page; a game sends ~30-60 datagrams/s

// ---------------------------------------------------------------- static files
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm',
               '.data': 'application/octet-stream', '.json': 'application/json', '.css': 'text/css', '.png': 'image/png' };
fs.mkdirSync(CACHE_DIR, { recursive: true });
const gzipping = new Set();
// gzip copies of the big files are made once, in the background, and served when they are newer than the file
function gzPath(file) { return path.join(CACHE_DIR, path.basename(file) + '.gz'); }
function gzReady(file, st) {
  const gz = gzPath(file);
  try { const gs = fs.statSync(gz); if (gs.mtimeMs >= st.mtimeMs) return gz; } catch (e) {}
  if (!gzipping.has(file)) {
    gzipping.add(file);
    const tmp = gz + '.tmp';
    fs.createReadStream(file).pipe(zlib.createGzip({ level: 6 })).pipe(fs.createWriteStream(tmp))
      .on('finish', () => { fs.renameSync(tmp, gz); gzipping.delete(file); console.log('gzip ready:', path.basename(file)); })
      .on('error', (e) => { gzipping.delete(file); console.warn('gzip failed', file, e.message); });
  }
  return null;
}
function serveFile(req, res, file) {
  let st;
  try { st = fs.statSync(file); } catch (e) { res.writeHead(404); res.end('not found'); return; }
  if (!st.isFile()) { res.writeHead(404); res.end('not found'); return; }
  const etag = '"' + st.size.toString(16) + '-' + Math.floor(st.mtimeMs).toString(16) + '"';
  const ext = path.extname(file);
  const headers = { 'Content-Type': MIME[ext] || 'application/octet-stream', 'ETag': etag,
                    'Cache-Control': ext === '.html' ? 'no-cache' : 'no-cache', 'Vary': 'Accept-Encoding' };
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, headers); res.end(); return; }
  const wantGz = /\bgzip\b/.test(req.headers['accept-encoding'] || '') && ext !== '.png';
  let src = file;
  if (wantGz) {
    const gz = st.size > 256 * 1024 ? gzReady(file, st) : null;
    if (gz) { src = gz; headers['Content-Encoding'] = 'gzip'; headers['Content-Length'] = fs.statSync(gz).size; }
    else if (st.size <= 256 * 1024) {
      const body = zlib.gzipSync(fs.readFileSync(file));
      headers['Content-Encoding'] = 'gzip'; headers['Content-Length'] = body.length;
      res.writeHead(200, headers); res.end(req.method === 'HEAD' ? undefined : body); return;
    }
  }
  if (!headers['Content-Length']) headers['Content-Length'] = st.size;
  res.writeHead(200, headers);
  if (req.method === 'HEAD') { res.end(); return; }
  fs.createReadStream(src).pipe(res);
}

function authorized(req) {
  if (!PASSWORD) return true;
  const h = req.headers.authorization || '';
  if (!h.startsWith('Basic ')) return false;
  const pass = Buffer.from(h.slice(6), 'base64').toString().split(':').slice(1).join(':');
  const a = Buffer.from(pass), b = Buffer.from(PASSWORD);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

// ---------------------------------------------------------------- rooms
const rooms = new Map();          // name -> { clients: Map(ip -> client) }
function roomOf(name) {
  let r = rooms.get(name);
  if (!r) { r = { clients: new Map() }; rooms.set(name, r); }
  return r;
}
function playersOf(r) { return [...r.clients.values()].map(c => ({ name: c.name, ip: c.ip })); }
function announce(r) {
  const msg = JSON.stringify({ type: 'players', players: playersOf(r) });
  for (const c of r.clients.values()) c.sendText(msg);
}
const ipToBytes = (s) => s.split('.').map(Number);

const server = http.createServer((req, res) => {
  if (!authorized(req)) { res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="lotd"' }); res.end('password required'); return; }
  const url = new URL(req.url, 'http://x');
  if (url.pathname === '/rooms') {
    const list = [...rooms.entries()].filter(([, r]) => r.clients.size).map(([name, r]) => ({ room: name, players: playersOf(r) }));
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(list));
    return;
  }
  let p = decodeURIComponent(url.pathname);
  if (p === '/') p = '/index.html';
  const file = path.normalize(path.join(GAME_DIR, p));
  if (!file.startsWith(GAME_DIR)) { res.writeHead(403); res.end(); return; }
  serveFile(req, res, file);
});

// ---------------------------------------------------------------- WebSocket (RFC 6455, server side)
function wsAccept(req, socket) {
  const key = req.headers['sec-websocket-key'];
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  socket.setNoDelay(true);
  const c = { socket, buf: Buffer.alloc(0), frag: null, fragOp: 0, onmessage: null, onclose: null, closed: false };
  function frame(op, payload) {
    const n = payload.length;
    let head;
    if (n < 126) head = Buffer.from([0x80 | op, n]);
    else if (n < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | op; head[1] = 126; head.writeUInt16BE(n, 2); }
    else { head = Buffer.alloc(10); head[0] = 0x80 | op; head[1] = 127; head.writeBigUInt64BE(BigInt(n), 2); }
    if (!c.closed) socket.write(Buffer.concat([head, payload]));
  }
  c.sendText = (s) => frame(1, Buffer.from(s));
  c.sendBinary = (b) => frame(2, b);
  c.close = () => { if (!c.closed) { frame(8, Buffer.alloc(0)); c.closed = true; socket.end(); } };
  socket.on('data', (d) => {
    c.buf = c.buf.length ? Buffer.concat([c.buf, d]) : d;
    for (;;) {
      const b = c.buf;
      if (b.length < 2) return;
      const fin = b[0] & 0x80, op = b[0] & 15, masked = b[1] & 0x80;
      let len = b[1] & 127, off = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); off = 10; }
      if (len > 1 << 20) { c.close(); return; }
      if (!masked) { c.close(); return; }
      if (b.length < off + 4 + len) return;
      const mask = b.subarray(off, off + 4);
      const payload = Buffer.from(b.subarray(off + 4, off + 4 + len));
      for (let i = 0; i < len; i++) payload[i] ^= mask[i & 3];
      c.buf = b.subarray(off + 4 + len);
      if (op === 8) { c.close(); return; }
      if (op === 9) { frame(10, payload); continue; }
      if (op === 10) continue;
      if (op === 0) { if (!c.frag) continue; c.frag.push(payload); if (fin) { const all = Buffer.concat(c.frag); c.frag = null; c.onmessage && c.onmessage(c.fragOp, all); } continue; }
      if (!fin) { c.frag = [payload]; c.fragOp = op; continue; }
      c.onmessage && c.onmessage(op, payload);
    }
  });
  const gone = () => { if (!c.gone) { c.gone = true; c.closed = true; c.onclose && c.onclose(); } };
  socket.on('close', gone);
  socket.on('error', gone);
  return c;
}

server.on('upgrade', (req, socket) => {
  const url = new URL(req.url, 'http://x');
  if (url.pathname !== '/net' || !authorized(req)) { socket.end('HTTP/1.1 403 Forbidden\r\n\r\n'); return; }
  const roomName = (url.searchParams.get('room') || 'public').slice(0, 32);
  const name = (url.searchParams.get('name') || 'Player').replace(/[^\w .\-]/g, '').slice(0, 20) || 'Player';
  const c = wsAccept(req, socket);
  const r = roomOf(roomName);
  let ip = null;
  for (let n = 2; n < 255; n++) { const cand = '10.77.0.' + n; if (!r.clients.has(cand)) { ip = cand; break; } }
  if (!ip || r.clients.size >= MAX_ROOM) { c.sendText(JSON.stringify({ type: 'error', message: 'room full' })); c.close(); return; }
  c.ip = ip; c.ipBytes = ipToBytes(ip); c.name = name;
  r.clients.set(ip, c);
  console.log(`net: ${name} joined room "${roomName}" as ${ip} (${r.clients.size} in room)`);
  c.sendText(JSON.stringify({ type: 'hello', ip, room: roomName, players: playersOf(r) }));
  announce(r);
  let window0 = Date.now(), count = 0;
  c.onmessage = (op, data) => {
    if (op !== 2 || data.length < 9 || data[0] !== 1) return;
    const now = Date.now();
    if (now - window0 >= 1000) { window0 = now; count = 0; }
    if (++count > MAX_MSGS_PER_SEC) return;
    const dst = data[1] + '.' + data[2] + '.' + data[3] + '.' + data[4];
    // rewrite the header for the receiver: source address and port, destination port
    const out = Buffer.from(data);
    out[1] = c.ipBytes[0]; out[2] = c.ipBytes[1]; out[3] = c.ipBytes[2]; out[4] = c.ipBytes[3];
    const dport = data.readUInt16BE(5), sport = data.readUInt16BE(7);
    out.writeUInt16BE(sport, 5); out.writeUInt16BE(dport, 7);
    if (data[4] === 255 || dst === '255.255.255.255') {
      for (const o of r.clients.values()) if (o !== c) o.sendBinary(out);
    } else {
      const o = r.clients.get(dst);
      if (o && o !== c) o.sendBinary(out);
    }
  };
  c.onclose = () => {
    r.clients.delete(ip);
    console.log(`net: ${name} (${ip}) left room "${roomName}"`);
    if (r.clients.size) announce(r); else rooms.delete(roomName);
  };
});

server.listen(PORT, () => {
  console.log(`Land of the Dead web port on http://localhost:${PORT}/ (game files from ${GAME_DIR})` +
              (PASSWORD ? ', password protected' : ''));
});
