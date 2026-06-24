// N3: async Socket + TLSSocket over /net. Two milestones.
var net = require('net');
var tls = require('tls');
var done = 0, total = 1;
function fin(tag){ console.log(tag); if(++done>=total){ console.log('N3 DONE'); } }

// TLS milestone: connect to registry over real TLS, send HTTP/1.1 GET, read decrypted status + some body, async.
var host = 'registry.npmjs.org';
var got = '', sawData = false, ended = false;
var s = tls.connect({ host: host, port: 443, servername: host }, function () {
  console.log('secureConnect fired (async, on loop)');
  var req = 'GET /left-pad HTTP/1.1\r\nHost: ' + host + '\r\nUser-Agent: node9\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n';
  s.write(req);
});
s.on('data', function (chunk) {
  sawData = true;
  if (got.length < 4096) got += chunk.toString('latin1');
});
s.on('end', function () { ended = true; });
s.on('error', function (e) { console.log('TLS ERROR: ' + e.message); fin('N3 TLS FAIL'); });
s.on('close', function () {
  var statusLine = got.split('\r\n')[0];
  console.log('status: ' + JSON.stringify(statusLine));
  console.log('sawData=' + sawData + ' ended=' + ended + ' bytes~' + got.length);
  var okStatus = /HTTP\/1\.[01] 200/.test(statusLine);
  var hasJson = got.indexOf('"name"') >= 0 || got.indexOf('left-pad') >= 0;
  console.log('okStatus=' + okStatus + ' hasJsonish=' + hasJson);
  fin(okStatus ? 'N3 TLS OK' : 'N3 TLS PARTIAL');
});
