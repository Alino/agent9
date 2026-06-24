globalThis.scriptArgs = ['/amd64/lib/node9/npm/bin/npm-cli.js'];
var pacote = require('/amd64/lib/node9/npm/node_modules/pacote/lib/index.js');
// correct integrity is sha512-XI5MPz...; give a deliberately WRONG one
var bad = 'sha512-' + 'A'.repeat(86) + '==';
print('requesting left-pad with WRONG integrity...');
pacote.tarball('left-pad@1.3.0', { registry:'https://registry.npmjs.org/', cache:'/tmp/n9cache2', integrity: bad, retry:{retries:0} })
 .then(function(buf){ print('SRI FAIL: accepted bad integrity (' + buf.length + ' bytes) -- SRI NOT gating!'); process.exit(1); },
  function(e){ print('SRI OK: rejected. code=' + (e&&e.code) + ' msg=' + JSON.stringify((e&&e.message||'').slice(0,90))); process.exit(0); });
