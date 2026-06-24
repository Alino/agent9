console.log('attempting require("express")...');
var express = require('express');
console.log('express loaded:', typeof express);
var app = express();
console.log('app created:', typeof app, '| app.get:', typeof app.get, '| app.listen:', typeof app.listen);
console.log('EXPRESS LOADS OK');
