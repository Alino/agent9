var n = globalThis.__n9native;
// read the gzip blob
var f = std.open('/usr/glenda/node9/work/test.gz','rb');
f.seek(0, std.SEEK_END); var sz=f.tell(); f.seek(0, std.SEEK_SET);
var ab = new ArrayBuffer(sz); f.read(ab,0,sz); f.close();
var gz = new Uint8Array(ab);
console.log('gzip bytes:', gz.length);
// gunzip via native inflate
var h = n.inflateCreate();
var out = new Uint8Array(4096), st = new Int32Array(2);
var produced = n.inflate(h, gz, out, st);
n.inflateDestroy(h);
console.log('inflate produced='+produced+' consumed='+st[0]+' done='+st[1]);
var s=''; for(var i=0;i<produced;i++) s+=String.fromCharCode(out[i]);
console.log('decompressed: '+JSON.stringify(s));
console.log('N1 ZLIB OK='+(s==='hello node9 zlib test — the quick brown fox jumps over the lazy dog 0123456789'));
