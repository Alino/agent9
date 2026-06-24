const path = require('path');
const util = require('util');
const EventEmitter = require('events');
const assert = require('assert');
const fs = require('fs');
const os = require('node:os');

console.log("=== path ===");
console.log(path.join('/foo', 'bar', 'baz/..'));     // /foo/bar
console.log(path.resolve('/a/b', '../c'));            // /a/c
console.log(path.dirname('/a/b/c.txt'), path.basename('/a/b/c.txt'), path.extname('index.html'));
console.log(JSON.stringify(path.parse('/a/b/c.txt')));

console.log("=== events ===");
const ee = new EventEmitter();
ee.on('ping', (x) => console.log('ping:', x));
ee.once('once', () => console.log('once fired'));
ee.emit('ping', 42); ee.emit('once'); ee.emit('once');
console.log('listenerCount ping:', ee.listenerCount('ping'));

console.log("=== util ===");
console.log(util.format('%s has %d items, %j', 'list', 3, { a: 1 }));
console.log(util.inspect({ a: 1, b: [1, 2, 3], c: 'x' }));

console.log("=== Buffer ===");
const b = Buffer.from('hello', 'utf8');
console.log('hex:', b.toString('hex'), '| base64:', b.toString('base64'));
console.log('b64 roundtrip:', Buffer.from(b.toString('base64'), 'base64').toString());
console.log('concat:', Buffer.concat([Buffer.from('ab'), Buffer.from('cd')]).toString());
console.log('byteLength café:', Buffer.byteLength('café'));

console.log("=== process ===");
console.log('platform:', process.platform, '| arch:', process.arch, '| version:', process.version);
console.log('cwd:', process.cwd(), '| env.home:', process.env.home);
process.stdout.write('via process.stdout.write\n');

console.log("=== os ===");
console.log('hostname:', os.hostname(), '| tmpdir:', os.tmpdir(), '| homedir:', os.homedir());

console.log("=== fs ===");
fs.writeFileSync('/tmp/node9_fs_test.txt', 'line1\nline2\n');
console.log('readback:', JSON.stringify(fs.readFileSync('/tmp/node9_fs_test.txt', 'utf8')));
console.log('exists:', fs.existsSync('/tmp/node9_fs_test.txt'));
const st = fs.statSync('/tmp/node9_fs_test.txt');
console.log('size:', st.size, '| isFile:', st.isFile(), '| isDir:', st.isDirectory());
console.log('readdir has it:', fs.readdirSync('/tmp').includes('node9_fs_test.txt'));

console.log("=== assert ===");
assert.strictEqual(1 + 1, 2);
assert.deepStrictEqual({ a: 1, b: [1] }, { a: 1, b: [1] });
console.log('assert passed');

console.log("=== async fs + nextTick ===");
process.nextTick(() => console.log('nextTick ran'));
fs.readFile('/tmp/node9_fs_test.txt', 'utf8', (err, data) => {
  console.log('async readFile:', err ? 'ERR ' + err.message : JSON.stringify(data));
  console.log('ALL DONE');
});
