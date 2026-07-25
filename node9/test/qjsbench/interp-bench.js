// interpreter-bound work of the kind module top-level code does:
// property access, closures, string building, array/object churn.
function work() {
  var i, j, sum = 0, objs = [], s = "";
  for (i = 0; i < 300; i++) objs.push({ id: i, name: "item" + i, tags: [i, i + 1, i + 2] });
  for (j = 0; j < 4000; j++) {
    for (i = 0; i < objs.length; i++) {
      var o = objs[i];
      sum += o.id + o.tags[o.id % 3];
      if ((i & 63) === 0) s = o.name.slice(0, 4) + (s.length & 7);
    }
  }
  var fns = objs.map(function (o) { return function () { return o.id * 2; }; });
  for (j = 0; j < 2000; j++) for (i = 0; i < fns.length; i++) sum += fns[i]();
  return sum + s.length;
}
work();
