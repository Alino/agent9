var debug = require('/usr/glenda/n9d/node_modules/debug');
var log = debug('test'); log('hello %s', 'world');
print('debug loaded + ms dep resolved: ' + (typeof debug === 'function'));
print('ms dep: ' + require('/usr/glenda/n9d/node_modules/ms')(1000));
