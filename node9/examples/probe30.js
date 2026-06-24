var DIR = '/usr/glenda/n9pkgs';
function NM(n){ return '/usr/glenda/pk/' + n + '/node_modules/' + n; }
var pkgs = [
  { n:'tslib', t:function(m){ var t=m.default||m; return typeof t.__assign==='function' && t.__assign({},{a:1}).a===1; } },
  { n:'ms',          t:function(m){ return m('2h')===7200000 && m(60000)==='1m'; } },
  { n:'semver',      t:function(m){ return m.valid('1.2.3')==='1.2.3' && m.gt('2.0.0','1.0.0')===true; } },
  { n:'qs',          t:function(m){ return m.parse('a=1&b=2').b==='2' && m.stringify({x:'y'})==='x=y'; } },
  { n:'minimist',    t:function(m){ return m(['--x','1','pos']).x==1 && m(['--x','1','pos'])._[0]==='pos'; } },
  { n:'commander',   t:function(m){ return typeof m.Command==='function' && typeof m.program==='object'; } },
  { n:'validator',   t:function(m){ var v=m.default||m; return v.isEmail('a@b.com')===true && v.isEmail('x')===false; } },
  { n:'classnames',  t:function(m){ var f=m.default||m; return f('a',{b:true,c:false})==='a b'; } },
  { n:'clsx',        t:function(m){ var f=m.clsx||m.default||m; return f('a',{b:true})==='a b'; } },
  { n:'is-number',   t:function(m){ var f=m.default||m; return f(5)===true && f('5')===true && f('x')===false; } },
  { n:'uuid',        t:function(m){ return /^[0-9a-f]{8}-[0-9a-f]{4}-4/.test((m.v4||m.default.v4)()); } },
  { n:'dayjs',       t:function(m){ var d=m.default||m; return typeof d().year()==='number' && d('2020-01-01').year()===2020; } },
  { n:'picocolors',  t:function(m){ var p=m.default||m; return typeof p.red==='function' && typeof p.red('x')==='string'; } },
  { n:'kleur',       t:function(m){ var k=m.default||m; return typeof k.red('x')==='string'; } },
  { n:'colorette',   t:function(m){ return typeof m.red==='function' && typeof m.red('x')==='string'; } },
  { n:'safe-buffer', t:function(m){ return m.Buffer.from('hi').toString()==='hi'; } },
  { n:'eventemitter3',t:function(m){ var E=m.default||m; var e=new E(),ok=0; e.on('a',function(){ok=1;}); e.emit('a'); return ok===1; } },
  { n:'inherits',    t:function(m){ var f=m.default||m; function A(){} function B(){} f(B,A); return (new B()) instanceof A; } },
  { n:'once',        t:function(m){ var o=m.default||m; var n=0,g=o(function(){n++;}); g();g();g(); return n===1; } },
  { n:'deepmerge',   t:function(m){ var d=m.default||m; var r=d({a:1},{b:2}); return r.a===1 && r.b===2; } },
  { n:'dotenv',      t:function(m){ var d=m.default||m; return typeof d.config==='function' && d.parse('A=1\nB=2').B==='2'; } },
  { n:'async',       t:function(m){ var a=m.default||m; return typeof a.map==='function' && typeof a.series==='function'; } },
  { n:'lru-cache',   t:function(m){ var L=m.LRUCache||m.default||m; var c=new L({max:5}); c.set('a',1); return c.get('a')===1; } },
  { n:'yallist',     t:function(m){ var Y=m.Yallist||m.default||m; var l=Y.create?Y.create([1,2,3]):new Y([1,2,3]); return l.length===3; } },
  { n:'chalk',       t:function(m){ var c=m.default||m; return typeof c.red==='function' && typeof c.red('x')==='string'; }, esm:true },
  { n:'nanoid',      t:function(m){ var f=m.nanoid||m.default; return typeof f()==='string' && f().length===21; }, esm:true },
  { n:'camelcase',   t:function(m){ var f=m.default||m; return f('foo-bar-baz')==='fooBarBaz'; }, esm:true },
  { n:'ansi-styles', t:function(m){ var s=m.default||m; return typeof s.red.open==='string' && s.red.open.charCodeAt(0)===27 && s.red.close.charCodeAt(0)===27; }, esm:true },
  { n:'strip-ansi',  t:function(m){ var f=m.default||m; var E=String.fromCharCode(27); return f(E+'[31mred'+E+'[0m')==='red'; }, esm:true },
  { n:'escape-string-regexp', t:function(m){ var f=m.default||m; return f('a.b*c')==='a\\.b\\*c'; }, esm:true },
];
(async function(){
  var pass=0, fail=0, results=[];
  for (var i=0;i<pkgs.length;i++){
    var p=pkgs[i], mod=null, err=null;
    if (!p.esm) { try { mod = require(NM(p.n)); } catch(e){ try { mod = await import(NM(p.n)); } catch(e2){ err=e2; } } }
    else { try { mod = await import(NM(p.n)); } catch(e){ try { mod = require(NM(p.n)); } catch(e2){ err=e; } } }
    if (err) { fail++; print('FAIL  ' + p.n + '  (load: ' + (err.message||err) + ')'); continue; }
    try { var r = p.t(mod); if (r) { pass++; print('OK    ' + p.n); } else { fail++; print('FAIL  ' + p.n + '  (assertion returned false)'); } }
    catch(e){ fail++; print('FAIL  ' + p.n + '  (run: ' + (e.message||e) + ')'); }
  }
  print('');
  print('=== 30 PACKAGES: ' + pass + ' OK / ' + fail + ' FAIL ===');
})();
