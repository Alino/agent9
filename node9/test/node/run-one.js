var common = require('/usr/glenda/node9/nt/common/index.js');
var tf = scriptArgs[1];
try { require(tf); }
catch (e) { print('FAIL ' + tf); print('  ' + (e && (e.stack || e.message) || e)); std.exit(1); }
setTimeout(function () {
  var bad = common.__verify();
  if (bad) { print('FAIL ' + tf + ' :: ' + bad); std.exit(1); }
  print('PASS ' + tf); std.exit(0);
}, 600);
