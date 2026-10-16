// Run with: node tools/test_remote_frame.cjs [captured.orf]
const fs=require('node:fs'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../components/services/web/index.html'),'utf8');
const start=source.indexOf('function decodeOrbitFrame'),end=source.indexOf('let remoteInputBusy',start);
if(start<0||end<start)throw new Error('Web decoder missing');
const decodeOrbitFrame=new Function(source.slice(start,end)+';return decodeOrbitFrame;')();
const assert=require('node:assert/strict');
function packet(flags,words,w=2,h=2){const b=new ArrayBuffer(16+words.length*2),v=new DataView(b);new Uint8Array(b).set([79,82,70,49]);v.setUint16(4,w,true);v.setUint16(6,h,true);v.setUint32(8,flags,true);v.setUint32(12,words.length*2,true);words.forEach((n,i)=>v.setUint16(16+i*2,n,true));return b;}
const a=decodeOrbitFrame(packet(0,[0xf800,0x7e0,0x1f,0xffff]));const b=decodeOrbitFrame(packet(1,[1,0xf800,1,0x7e0,1,0x1f,1,0xffff]));assert.deepEqual(a,b);assert.deepEqual([...a.pixels],[255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255]);assert.equal(decodeOrbitFrame(new ArrayBuffer(54)),null);
for(const data of [packet(0,[1,2,3]),packet(1,[0,0]),packet(1,[5,0]),packet(1,[3,0]),packet(2,[1,2,3,4]),packet(0,[1,2,3,4],2048,2048)])assert.throws(()=>decodeOrbitFrame(data));
console.log('Web decoder raw/RLE, colors, malformed bounds: PASS');

if(process.argv[2]){const file=fs.readFileSync(process.argv[2]),decoded=decodeOrbitFrame(file.buffer.slice(file.byteOffset,file.byteOffset+file.byteLength));assert(decoded);const crypto=require('node:crypto');console.log(JSON.stringify({width:decoded.width,height:decoded.height,rgba_sha256:crypto.createHash('sha256').update(decoded.pixels).digest('hex')}));}
