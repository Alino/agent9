var https = require('https');
var done = false;
function fin(t){ if(done)return; done=true; console.log(t); }
var req = https.get('https://registry.npmjs.org/left-pad', function (res) {
  console.log('statusCode: ' + res.statusCode);
  console.log('content-type: ' + res.headers['content-type']);
  console.log('content-encoding: ' + (res.headers['content-encoding']||'(none)'));
  console.log('transfer-encoding: ' + (res.headers['transfer-encoding']||'(none)'));
  var chunks = [], total = 0;
  res.on('data', function (c) { chunks.push(c); total += c.length; });
  res.on('end', function () {
    var Buffer = require('buffer').Buffer;
    var body = Buffer.concat(chunks).toString('utf8');
    console.log('body bytes: ' + total);
    var okJson = false, name = '', nver = 0;
    try { var j = JSON.parse(body); okJson = true; name = j.name; nver = Object.keys(j.versions||{}).length; } catch(e){ console.log('JSON parse error: ' + e.message + ' | head=' + JSON.stringify(body.slice(0,80))); }
    console.log('parsed: name=' + name + ' versions=' + nver);
    var pass = res.statusCode === 200 && okJson && name === 'left-pad' && nver > 0;
    fin(pass ? 'N4 HTTPS OK' : 'N4 HTTPS FAIL');
  });
});
req.on('error', function (e) { console.log('REQ ERROR: ' + e.message); fin('N4 HTTPS FAIL'); });
