const http = require('http');
const server = http.createServer((req, res) => {
  if (req.url === '/json') { res.writeHead(200, {'Content-Type':'application/json'}); res.end(JSON.stringify({ok:true, method:req.method, url:req.url})); }
  else { res.writeHead(200, {'Content-Type':'text/plain'}); res.end('node9 http: '+req.method+' '+req.url+'\n'); }
});
server.on('listening', () => std.err.puts('SERVER listening on 9098\n'));
server.listen(9098);
