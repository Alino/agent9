const greet = require('./greet');
console.log(greet('node9'));
console.log('meta:', JSON.stringify(greet.meta));
console.log('require.resolve("fs"):', require.resolve('fs'));
console.log('builtins still work:', require('path').join('a', 'b'), require('os').platform());
console.log('FILE REQUIRE OK');
