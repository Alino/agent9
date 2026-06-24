var n = globalThis.__n9native;
function rd(fd,max){ var b=new Uint8Array(max); var k=os.read(fd,b.buffer,0,max); if(k<=0)return ''; var s=''; for(var i=0;i<k;i++)s+=String.fromCharCode(b[i]); return s; }
function wr(fd,s){ var b=new Uint8Array(s.length); for(var i=0;i<s.length;i++)b[i]=s.charCodeAt(i)&255; return os.write(fd,b.buffer,0,s.length); }
var HOST='registry.npmjs.org', PORT='443';
var cs=os.open('/net/cs',os.O_RDWR);
wr(cs,'tcp!'+HOST+'!'+PORT); os.seek(cs,0,std.SEEK_SET);
var reply=rd(cs,256).trim(); os.close(cs);
console.log('cs reply: '+JSON.stringify(reply));
if(!reply){ console.log('DNS/CS FAILED (no resolver?)'); std.exit(1); }
var parts=reply.split(' '), clonefile=parts[0], addr=parts[1];
var ctl=os.open(clonefile,os.O_RDWR); var conn=rd(ctl,64).trim();
var cr=wr(ctl,'connect '+addr);
console.log('connect "'+addr+'" -> '+cr);
if(cr<0){ console.log('CONNECT FAILED (no internet?)'); std.exit(1); }
var base=clonefile.replace(/\/clone$/,'');
var tcpfd=os.open(base+'/'+conn+'/data',os.O_RDWR);
console.log('tcp data fd: '+tcpfd);
var tlsfd=n.tlsClient(tcpfd, HOST);
console.log('tlsClient -> fd '+tlsfd+(tlsfd<0?' (FAILED: -1 handshake, -2 cert untrusted)':''));
if(tlsfd<0) std.exit(1);
wr(tlsfd, 'GET /left-pad HTTP/1.0\r\nHost: '+HOST+'\r\nAccept: application/json\r\nUser-Agent: node9\r\n\r\n');
var resp=rd(tlsfd, 1024);
console.log('--- decrypted response, first line ---');
console.log(resp.split('\r\n')[0]);
console.log('N1 TLS OK (real verified HTTPS to registry.npmjs.org from 9front)');
os.close(tlsfd);
