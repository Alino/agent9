const http = require('http');
http.get({host:'10.0.2.15', port:9098, path:'/json'}, (res) => {
  console.log('status: '+res.statusCode);
  console.log('content-type: '+res.headers['content-type']);
  res.on('data', (d)=>console.log('body: '+d));
  res.on('end', ()=>console.log('HTTP CLIENT DONE (Node API over /net)'));
});
