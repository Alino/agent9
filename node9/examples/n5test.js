var fs = require('fs');
var pass=0, fail=0, pend=0, done=false;
function ok(c,l){ if(c)pass++; else{fail++;console.log('FAIL '+l);} }
function fin(){ if(done)return; done=true; console.log('fs: '+pass+' passed, '+fail+' failed'); console.log(fail===0&&pend===0?'N5 FS OK':'N5 FS FAIL pend='+pend); }
function chk(){ if(pend===0) fin(); }

var base = fs.mkdtempSync('/tmp/n9fs-');
ok(typeof base==='string' && fs.existsSync(base), 'mkdtemp creates uniq dir');
fs.mkdirSync(base+'/a/b/c', {recursive:true});
ok(fs.existsSync(base+'/a/b/c') && fs.statSync(base+'/a/b/c').isDirectory(), 'recursive mkdir');
fs.writeFileSync(base+'/a/hello.txt', 'hi node9');
ok(fs.readFileSync(base+'/a/hello.txt','utf8')==='hi node9', 'write/read file');
// fd ops
var fd = fs.openSync(base+'/a/raw.bin','w');
fs.writeSync(fd, Buffer.from([1,2,3,4]));
fs.closeSync(fd);
var rb = fs.readFileSync(base+'/a/raw.bin');
ok(rb.length===4 && rb[0]===1 && rb[3]===4, 'fd write + readback');
// readdir withFileTypes
var ents = fs.readdirSync(base+'/a', {withFileTypes:true});
var names = ents.map(function(e){return e.name;}).sort();
ok(names.indexOf('hello.txt')>=0 && ents.filter(function(e){return e.name==='b';})[0].isDirectory(), 'readdir withFileTypes + Dirent');
// rename
fs.renameSync(base+'/a/hello.txt', base+'/a/renamed.txt');
ok(!fs.existsSync(base+'/a/hello.txt') && fs.existsSync(base+'/a/renamed.txt'), 'rename');

// write stream
pend++;
var ws = fs.createWriteStream(base+'/a/stream.txt');
ws.on('open', function(){ ws.write('line1\n'); ws.write('line2\n'); ws.end(); });
ws.on('close', function(){ ok(fs.readFileSync(base+'/a/stream.txt','utf8')==='line1\nline2\n', 'createWriteStream'); pend--; chk(); });

// read stream
pend++;
fs.writeFileSync(base+'/a/big.txt', 'X'.repeat(200000));
var rs = fs.createReadStream(base+'/a/big.txt'); var got=0;
rs.on('data', function(c){ got += c.length; });
rs.on('end', function(){ ok(got===200000, 'createReadStream full file ('+got+')'); pend--; chk(); });

// promises + rm -rf
pend++;
fs.promises.mkdir(base+'/p/q', {recursive:true})
  .then(function(){ return fs.promises.writeFile(base+'/p/q/f.txt','data'); })
  .then(function(){ return fs.promises.readFile(base+'/p/q/f.txt','utf8'); })
  .then(function(d){ ok(d==='data', 'fs.promises write/read'); 
    fs.rmSync(base, {recursive:true, force:true});
    ok(!fs.existsSync(base), 'rm -rf removes tree');
    pend--; chk();
  })
  .catch(function(e){ ok(false,'promises chain: '+e.message); pend--; chk(); });

chk();
