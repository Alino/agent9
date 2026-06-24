// node9: minimal HTTP server over Plan 9 /net (node9-original, not from Node).
function rd(fd, max) { var b = new Uint8Array(max); var n = os.read(fd, b.buffer, 0, max); var s = ''; for (var i = 0; i < n; i++) s += String.fromCharCode(b[i]); return s; }
function wr(fd, s) { var b = new Uint8Array(s.length); for (var i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 255; return os.write(fd, b.buffer, 0, s.length); }

var PORT = '9099';
var ctl = os.open('/net/tcp/clone', os.O_RDWR);
if (ctl < 0) { console.log('FAIL: cannot open /net/tcp/clone'); std.exit(1); }
var conn = rd(ctl, 64).trim().replace(/\s+$/, '');
console.log('clone -> conn dir', conn);
var aw = wr(ctl, 'announce *!' + PORT);
console.log('announce ' + PORT + ' wrote', aw);
console.log('listening on tcp!*!' + PORT + ' ...');

var lfd = os.open('/net/tcp/' + conn + '/listen', os.O_RDWR);
if (lfd < 0) { console.log('FAIL: listen open'); std.exit(1); }
var acc = rd(lfd, 64).trim();
console.log('accepted conn dir', acc);
var dfd = os.open('/net/tcp/' + acc + '/data', os.O_RDWR);
var req = rd(dfd, 2048);
var line1 = req.split('\r\n')[0];
console.log('REQUEST: ' + JSON.stringify(line1));
var body = 'Hello from node9 over /net on 9front!\n';
wr(dfd, 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ' + body.length + '\r\nConnection: close\r\n\r\n' + body);
os.close(dfd); os.close(lfd); os.close(ctl);
console.log('SERVED OK');
