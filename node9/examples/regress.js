var f=0; function c(l,g,w){ if(g!==w){f++;print('FAIL '+l+' got='+JSON.stringify(g));} }
// path
var p=require('path'); c('join', p.join('/a','b','../c'), '/a/c'); c('basename', p.basename('/x/y.js','.js'), 'y'); c('relative', p.relative('/a/b','/a/c/d'), '../c/d'); c('dirname/', p.dirname('/'), '/');
// url
var U=require('url'); var u=new U.URL('https://x.com:8443/p?a=1&b=2#h'); c('url.host', u.host, 'x.com:8443'); c('url.pathname', u.pathname, '/p'); c('url.searchParams', u.searchParams.get('b'), '2');
// querystring
var qs=require('querystring'); c('qs.parse', qs.parse('a=1&b=2').b, '2'); c('qs.stringify', qs.stringify({x:'y'}), 'x=y');
// events
var EE=require('events'); var e=new EE(); var got=0; e.on('x',function(v){got=v;}); e.emit('x',42); c('emit', got, 42);
// util
var util=require('util'); c('format', util.format('%s=%d','a',5), 'a=5'); c('isArray', Array.isArray([]), true);
// crypto
var cr=require('crypto'); c('sha256', cr.createHash('sha256').update('abc').digest('hex'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
// zlib roundtrip via gunzip of a known gzip (skip; covered by npm). stream:
var S=require('stream'); c('stream.Readable', typeof S.Readable, 'function'); c('stream is ctor', typeof S, 'function');
// os
var os2=require('os'); c('os.platform', os2.platform(), 'plan9'); c('os.constants.errno.ENOENT', os2.constants.errno.ENOENT, 2);
// fs roundtrip
var fs=require('fs'); fs.writeFileSync('/tmp/rg.txt','hi'); c('fs roundtrip', fs.readFileSync('/tmp/rg.txt','utf8'), 'hi'); fs.unlinkSync('/tmp/rg.txt');
// process
c('process.platform', process.platform, 'plan9'); c('umask', typeof process.umask, 'function');
print(f===0?'REGRESSION SMOKE: ALL PASS':('REGRESSION: '+f+' FAIL'));
