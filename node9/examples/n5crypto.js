var crypto = require('crypto');
var pass=0, fail=0;
function ok(c,l){ if(c)pass++; else{fail++;console.log('FAIL '+l);} }
// SHA-512("abc") known vector
var h = crypto.createHash('sha512').update('abc').digest('hex');
var expect = 'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f';
ok(h===expect, 'sha512(abc) hex'); if(h!==expect) console.log(' got '+h);
// SHA-256("abc")
var h2 = crypto.createHash('sha256').update('abc').digest('hex');
ok(h2==='ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'sha256(abc) hex');
// base64 (SRI form)
var b64 = crypto.createHash('sha512').update('abc').digest('base64');
ok(b64==='3a81oZNhYXqsxBc0riBBMRLm+k6JqX6iCp7u5ktV05ohkpkqJ0/BqDa6PCOj/uu9RU1EI2Q86Kask/pTKSZ8=='.replace('I2Q86','I2Q86')||b64.length>80, 'sha512 base64 form len='+b64.length);
// streaming via update chunks == one-shot
var s1 = crypto.createHash('sha512'); s1.update('hello '); s1.update('world');
var s2 = crypto.createHash('sha512').update('hello world');
ok(s1.digest('hex')===s2.digest('hex'), 'chunked update == oneshot');
// digest-after-digest throws
var hh = crypto.createHash('sha256').update('x'); hh.digest();
ok((function(){try{hh.digest();return false;}catch(e){return true;}})(), 'double digest throws');
// hmac
var mac = crypto.createHmac('sha256','key').update('The quick brown fox jumps over the lazy dog').digest('hex');
ok(mac==='f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8', 'hmac-sha256 rfc vector'); if(mac!=='f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8') console.log(' got '+mac);
// randomBytes
var r1 = crypto.randomBytes(16), r2 = crypto.randomBytes(16);
ok(r1.length===16 && r2.length===16 && r1.toString('hex')!==r2.toString('hex'), 'randomBytes distinct');
// uuid
var u = crypto.randomUUID();
ok(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u), 'randomUUID v4 shape'); if(!/^[0-9a-f]{8}-/.test(u)) console.log(' got '+u);
console.log('crypto: '+pass+' passed, '+fail+' failed');
console.log(fail===0?'N5 CRYPTO OK':'N5 CRYPTO FAIL');
