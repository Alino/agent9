function rd(fd, max){ var b=new Uint8Array(max); var n=os.read(fd,b.buffer,0,max); if(n<=0)return ''; var s=''; for(var i=0;i<n;i++)s+=String.fromCharCode(b[i]); return s; }
function wr(fd, s){ var b=new Uint8Array(s.length); for(var i=0;i<s.length;i++)b[i]=s.charCodeAt(i)&255; return os.write(fd,b.buffer,0,s.length); }
// proper Plan 9 dial via the connection server /net/cs
var target = scriptArgs[1] || 'tcp!127.0.0.1!9099';
var cs = os.open('/net/cs', os.O_RDWR);
var w = wr(cs, target);
console.log('cs query "' + target + '" wrote', w);
os.seek(cs, 0, std.SEEK_SET); var reply = rd(cs, 256).trim();
console.log('cs reply:', JSON.stringify(reply));
os.close(cs);
if (!reply) { console.log('NO CS REPLY'); std.exit(1); }
var parts = reply.split(' ');                 // "<clonefile> <addr>"
var clonefile = parts[0], connaddr = parts[1];
var ctl = os.open(clonefile, os.O_RDWR);
var conn = rd(ctl, 64).trim();
var r = wr(ctl, 'connect ' + connaddr);
console.log('connect "' + connaddr + '" ->', r);
if (r < 0) { console.log('CONNECT FAILED'); std.exit(1); }
var base = clonefile.replace(/\/clone$/, '');
var dfd = os.open(base + '/' + conn + '/data', os.O_RDWR);
wr(dfd, 'GET / HTTP/1.0\r\nHost: localhost\r\n\r\n');
console.log('--- RESPONSE ---\n' + rd(dfd, 2048));
os.close(dfd); os.close(ctl);
console.log('CLIENT DONE');
