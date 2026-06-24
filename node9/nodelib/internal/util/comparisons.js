/* node9: isDeepStrictEqual (functional). */
function deq(a,b){ if(a===b)return true; if(typeof a!=='object'||typeof b!=='object'||!a||!b)return a!==a&&b!==b;
  if(Array.isArray(a)!==Array.isArray(b))return false;
  var ka=Object.keys(a),kb=Object.keys(b); if(ka.length!==kb.length)return false;
  for(var i=0;i<ka.length;i++){ if(!Object.prototype.hasOwnProperty.call(b,ka[i]))return false; if(!deq(a[ka[i]],b[ka[i]]))return false; } return true; }
module.exports = { isDeepStrictEqual: deq, isDeepEqual: deq };
