/* node9 minimal Buffer for adapted Node modules (node9-original, not from Node). */
function u8enc(str){ var b=[],i,c; for(i=0;i<str.length;i++){ c=str.charCodeAt(i);
  if(c<0x80)b.push(c); else if(c<0x800)b.push(0xC0|(c>>6),0x80|(c&0x3F));
  else b.push(0xE0|(c>>12),0x80|((c>>6)&0x3F),0x80|(c&0x3F)); } return new Uint8Array(b); }
function u8dec(b){ var s='',i=0,c; while(i<b.length){ c=b[i++];
  if(c<0x80)s+=String.fromCharCode(c);
  else if(c<0xE0)s+=String.fromCharCode(((c&0x1F)<<6)|(b[i++]&0x3F));
  else s+=String.fromCharCode(((c&0x0F)<<12)|((b[i++]&0x3F)<<6)|(b[i++]&0x3F)); } return s; }
var P = Object.create(Uint8Array.prototype);
P.toString = function(enc,start,end){ enc=(enc||'utf8').toLowerCase();
  var s=this.subarray(start||0,end==null?this.length:end);
  if(enc==='hex'){ var h=''; for(var i=0;i<s.length;i++)h+=(s[i]<16?'0':'')+s[i].toString(16); return h; }
  if(enc==='latin1'||enc==='binary'||enc==='ascii'){ var r=''; for(var j=0;j<s.length;j++)r+=String.fromCharCode(s[j]); return r; }
  return u8dec(s); };
P.slice = function(a,b){ var x=this.subarray(a,b); Object.setPrototypeOf(x,P); return x; };
P.write = function(str,off){ off=off||0; var e=u8enc(str); this.set(e.subarray(0,this.length-off),off); return e.length; };
function wrap(x){ Object.setPrototypeOf(x,P); return x; }
function Buffer(){}
Buffer.allocUnsafe = function(n){ return wrap(new Uint8Array(n)); };
Buffer.alloc = function(n,f){ var b=wrap(new Uint8Array(n)); if(f!=null)b.fill(typeof f==='string'?f.charCodeAt(0):f); return b; };
Buffer.from = function(v,enc){ if(typeof v==='string'){ enc=(enc||'utf8').toLowerCase();
    if(enc==='latin1'||enc==='binary'||enc==='ascii'){ var u=new Uint8Array(v.length); for(var k=0;k<v.length;k++)u[k]=v.charCodeAt(k)&255; return wrap(u); }
    if(enc==='hex'){ var b=new Uint8Array(v.length/2); for(var m=0;m<b.length;m++)b[m]=parseInt(v.substr(m*2,2),16); return wrap(b); }
    return wrap(u8enc(v)); }
  if(v instanceof ArrayBuffer)return wrap(new Uint8Array(v));
  return wrap(Uint8Array.from(v)); };
Buffer.isBuffer = function(v){ return v && Object.getPrototypeOf(v)===P; };
Buffer.byteLength = function(s){ return u8enc(String(s)).length; };
module.exports = { Buffer: Buffer };
