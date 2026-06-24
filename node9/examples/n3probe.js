var net = require('net');
var nat = globalThis.__n9native;
var host = 'registry.npmjs.org';
var c = net._dial('tcp', host, 443);
print('dialed:', c ? ('fd=' + c.fd + ' ctl=' + c.ctl) : 'FAIL');
var tfd = nat.tlsClient(c.fd, host);
print('tlsClient fd:', tfd);
var req = 'GET /left-pad HTTP/1.1\r\nHost: ' + host + '\r\nUser-Agent: node9\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n';
var rb = new Uint8Array(req.length); for (var i=0;i<req.length;i++) rb[i]=req.charCodeAt(i)&255;
var w = os.write(tfd, rb.buffer, 0, rb.length);
print('wrote bytes:', w);
// SYNC read loop (control: proves TLS data flows)
var buf = new ArrayBuffer(8192), got = '';
for (var k=0;k<3;k++){ var n = os.read(tfd, buf, 0, 8192); print('sync read #'+k+' n='+n); if(n>0){ var u=new Uint8Array(buf,0,Math.min(n,200)); var s=''; for(var j=0;j<u.length;j++) s+=String.fromCharCode(u[j]); if(k===0) print('first bytes: '+JSON.stringify(s.split('\r\n')[0])); } if(n<=0) break; }
print('=== now test async setReadHandler on a FRESH tls fd ===');
var c2 = net._dial('tcp', host, 443);
var tfd2 = nat.tlsClient(c2.fd, host);
os.write(tfd2, rb.buffer, 0, rb.length);
var fired = false, b2 = new ArrayBuffer(8192);
os.setReadHandler(tfd2, function(){
  fired = true;
  var n = os.read(tfd2, b2, 0, 8192);
  print('ASYNC handler fired! n='+n);
  var u=new Uint8Array(b2,0,Math.min(n,80)); var s=''; for(var j=0;j<u.length;j++) s+=String.fromCharCode(u[j]);
  print('async first: '+JSON.stringify(s.split('\r\n')[0]));
  os.setReadHandler(tfd2, null); os.close(tfd2);
});
os.setTimeout(function(){ if(!fired) print('ASYNC handler NEVER fired (select on tls fd not pollable)'); }, 4000);
