var n = globalThis.__n9native;
console.log('__n9native:', typeof n, n ? Object.keys(n).join(',') : '-');
// SHA-512 of "abc" (known test vector)
var h = n.hashCreate(1);                 // 1 = sha512
n.hashUpdate(h, new Uint8Array([97,98,99]));
var out = new Uint8Array(64);
var dlen = n.hashDigest(h, out);
var hex=''; for(var i=0;i<dlen;i++) hex += (out[i]<16?'0':'')+out[i].toString(16);
var exp = 'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f';
console.log('sha512(abc) len='+dlen+' MATCH='+(hex===exp));
if(hex!==exp){ console.log(' got '+hex); console.log(' exp '+exp); }
// SHA-256 of "abc"
var h2=n.hashCreate(0); n.hashUpdate(h2,new Uint8Array([97,98,99])); var o2=new Uint8Array(32); var d2=n.hashDigest(h2,o2);
var hx2=''; for(i=0;i<d2;i++) hx2+=(o2[i]<16?'0':'')+o2[i].toString(16);
console.log('sha256(abc) MATCH='+(hx2==='ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'));
// randomBytes: 16 bytes, check not all-zero
var r=new Uint8Array(16); n.randomBytes(r); var nz=0; for(i=0;i<16;i++) if(r[i])nz++;
console.log('randomBytes nonzero bytes:', nz, '(>0 expected)');
console.log('N1 CRYPTO OK');
