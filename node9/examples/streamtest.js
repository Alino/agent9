// load the stream impl, build the module, run N2 milestones
(0, eval)((function () { var f = std.open('/usr/glenda/node9/work/stream-impl.js', 'rb'); var s = f.readAsString(); f.close(); return s; })());
var S = globalThis.__makeStream(require('events'), process.nextTick);

var pass = 0, fail = 0, pending = 0, doneCalled = false;
function ok(c, l) { if (c) pass++; else { fail++; console.log('FAIL ' + l); } }
function eq(a, b, l) { ok(JSON.stringify(a) === JSON.stringify(b), l + ' (got ' + JSON.stringify(a) + ')'); }
function fin() { if (doneCalled) return; doneCalled = true; console.log('stream: ' + pass + ' passed, ' + fail + ' failed'); console.log(fail === 0 && pending === 0 ? 'N2 STREAM OK' : 'N2 STREAM FAILURES (pending=' + pending + ')'); }

// 1. Readable.from + pipe to a collecting Writable
pending++;
(function () {
  var got = [];
  var w = new S.Writable({ objectMode: true, write: function (c, e, cb) { got.push(c); cb(); } });
  w.on('finish', function () { eq(got, [1, 2, 3, 4, 5], 'from+pipe collects in order'); pending--; check(); });
  S.Readable.from([1, 2, 3, 4, 5]).pipe(w);
})();

// 2. backpressure: slow writer, fast producer — all chunks arrive in order, writes serialize
pending++;
(function () {
  var got = [], sawBackpressure = false;
  var w = new S.Writable({ objectMode: true, highWaterMark: 2, write: function (c, e, cb) { got.push(c); process.nextTick(cb); } });
  var r = new S.Readable({ objectMode: true, highWaterMark: 2 });
  for (var i = 1; i <= 10; i++) { var ret = r.push(i); if (ret === false) sawBackpressure = true; }
  r.push(null);
  w.on('finish', function () { eq(got, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 'backpressure: all in order'); ok(sawBackpressure, 'backpressure: push() signalled false'); pending--; check(); });
  r.pipe(w);
})();

// 3. Transform (doubling)
pending++;
(function () {
  var got = [];
  var t = new S.Transform({ objectMode: true, transform: function (c, e, cb) { cb(null, c * 2); } });
  t.on('data', function (c) { got.push(c); });
  t.on('end', function () { eq(got, [2, 4, 6], 'transform doubles'); pending--; check(); });
  t.write(1); t.write(2); t.write(3); t.end();
})();

// 4. pipeline(readable, transform, writable)
pending++;
(function () {
  var got = [];
  var r = S.Readable.from(['a', 'b', 'c']);
  var t = new S.Transform({ objectMode: true, transform: function (c, e, cb) { cb(null, c.toUpperCase()); } });
  var w = new S.Writable({ objectMode: true, write: function (c, e, cb) { got.push(c); cb(); } });
  S.pipeline(r, t, w, function (err) { ok(!err, 'pipeline no error'); eq(got, ['A', 'B', 'C'], 'pipeline transforms'); pending--; check(); });
})();

// 5. async iteration
pending++;
(async function () {
  var got = [];
  var r = S.Readable.from([10, 20, 30]);
  for await (var c of r) got.push(c);
  eq(got, [10, 20, 30], 'async iteration'); pending--; check();
})();

function check() { if (pending === 0) fin(); }
// safety: report after the loop drains even if a stream hung
process.nextTick(function () { process.nextTick(function () { if (pending !== 0) { /* let loop run */ } }); });
