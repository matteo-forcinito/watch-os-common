// Tests the ORF2 decoder of the dashboard (components/services/web/index.html).
// Run with: node tools/test_remote_frame.cjs
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const source = fs.readFileSync(path.join(__dirname, '../components/services/web/index.html'), 'utf8');
const start = source.indexOf('function decodeOrf2'), end = source.indexOf('function remoteWanted', start);
if (start < 0 || end < start) throw new Error('Web decoder missing');
const decodeOrf2 = new Function(source.slice(start, end) + ';return decodeOrf2;')();

// 28-byte header: magic, screen w/h, area x/y/w/h, seq, flags, payload length.
function packet({sw = 4, sh = 4, x = 0, y = 0, w, h, seq = 9, flags, words}) {
  const b = new ArrayBuffer(28 + words.length * 2), v = new DataView(b);
  new Uint8Array(b).set([79, 82, 70, 50]);
  [sw, sh, x, y, w, h].forEach((n, i) => v.setUint16(4 + i * 2, n, true));
  v.setUint32(16, seq, true); v.setUint32(20, flags, true); v.setUint32(24, words.length * 2, true);
  words.forEach((n, i) => v.setUint16(28 + i * 2, n, true));
  return b;
}

const raw = decodeOrf2(packet({x: 1, y: 2, w: 2, h: 1, flags: 0, words: [0xf800, 0x001f]}));
const runs = decodeOrf2(packet({x: 1, y: 2, w: 2, h: 1, flags: 1, words: [1, 0xf800, 1, 0x001f]}));
assert.deepEqual([...raw.pixels], [255, 0, 0, 255, 0, 0, 255, 255]);
assert.deepEqual([...runs.pixels], [...raw.pixels]);
assert.equal(raw.x, 1); assert.equal(raw.y, 2); assert.equal(raw.seq, 9);

const idle = decodeOrf2(packet({w: 0, h: 0, flags: 0, words: []}));
assert.equal(idle.pixels, undefined);   // nothing changed

for (const bad of [
  packet({x: 3, w: 2, h: 1, flags: 0, words: [1, 2]}),        // area outside the screen
  packet({w: 2, h: 1, flags: 1, words: [0, 5]}),              // zero-length run
  packet({w: 2, h: 1, flags: 1, words: [3, 5]}),              // too many pixels
  packet({w: 2, h: 1, flags: 0, words: [1, 2, 3]}),           // wrong raw length
]) assert.throws(() => decodeOrf2(bad));

console.log('Web ORF2 decoder: areas, raw/RLE, idle frames, malformed input: PASS');
