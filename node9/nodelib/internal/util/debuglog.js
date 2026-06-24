function debuglog(){ var f=function(){}; f.enabled=false; return f; }
module.exports = { debuglog, debug: debuglog };
