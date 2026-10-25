// The page's end of the virtual LAN (src/wsock.c, server/server.js). One WebSocket to the relay per page; the relay
// gives the page an address 10.77.x.y in its room and forwards UDP datagrams between the pages of that room.
//
// Wire format (binary messages), all numbers big-endian:
//   page -> relay: u8 1, u32 dst ip, u16 dst port, u16 src port, payload      (dst 255.255.255.255 = everyone)
//   relay -> page: u8 1, u32 src ip, u16 src port, u16 dst port, payload
// Text messages are JSON: {type: "hello", ip: "10.77.0.2", room, players: [...]}, {type: "players", players: [...]}
(function () {
  function ipToU32le(s) {             // "a.b.c.d" -> the 32-bit value a sockaddr_in holds on x86 (a in the low byte)
    var p = s.split('.').map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
  }
  function u32leToBe(v) { return (((v & 255) << 24) | (((v >> 8) & 255) << 16) | (((v >> 16) & 255) << 8) | ((v >>> 24) & 255)) >>> 0; }
  function beToU32le(v) { return u32leToBe(v); }

  var V = {
    ws: null, ip: 0, ipText: '', room: '', players: [], inbox: [], onchange: null, sent: 0, received: 0,
    connect: function (url, room, name) {
      V.room = room;
      return new Promise(function (resolve, reject) {
        var ws = new WebSocket(url + '?room=' + encodeURIComponent(room) + '&name=' + encodeURIComponent(name || 'Player'));
        ws.binaryType = 'arraybuffer';
        var done = false;
        ws.onmessage = function (ev) {
          if (typeof ev.data === 'string') {
            var m = JSON.parse(ev.data);
            if (m.type === 'hello') { V.ip = ipToU32le(m.ip); V.ipText = m.ip; if (!done) { done = true; resolve(m); } }
            if (m.players) V.players = m.players;
            if (m.type === 'error' && !done) { done = true; reject(new Error(m.message)); }
            if (V.onchange) V.onchange(m);
            return;
          }
          var d = new DataView(ev.data);
          if (d.getUint8(0) !== 1 || ev.data.byteLength < 9) return;
          if (V.inbox.length > 4096) V.inbox.shift();      // a stalled game drops the oldest, like a full UDP buffer
          V.inbox.push({ src: beToU32le(d.getUint32(1)), sport: d.getUint16(5), dport: d.getUint16(7),
                         data: new Uint8Array(ev.data, 9) });
          V.received++;
        };
        ws.onclose = function () { V.ip = 0; if (V.onchange) V.onchange({ type: 'closed' }); if (!done) { done = true; reject(new Error('connection closed')); } };
        ws.onerror = function () {};
        V.ws = ws;
      });
    },
    send: function (dst, dport, sport, bytes) {
      if (!V.ws || V.ws.readyState !== 1) return;
      var b = new Uint8Array(9 + bytes.length), d = new DataView(b.buffer);
      d.setUint8(0, 1); d.setUint32(1, u32leToBe(dst)); d.setUint16(5, dport); d.setUint16(7, sport);
      b.set(bytes, 9);
      V.ws.send(b);
      V.sent++;
    }
  };
  window.VNet = V;
})();
