const path = require('path');
const data = require('./data.json');
module.exports = function greet(name) {
  return data.greeting + ', ' + name + '! (from ' + path.basename(__filename) + ')';
};
module.exports.meta = { dir: __dirname, version: data.version };
