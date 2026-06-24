var f=0; function c(l,g,w){ if(g!==w){f++;print('FAIL '+l+' got='+g);} }
c('x!==x for NaN', (function(x){return x!==x;})(NaN), true);
c('[NaN].indexOf(NaN)', [NaN].indexOf(NaN), -1);
c('[NaN].includes(NaN)', [NaN].includes(NaN), true);
c('Object.is(NaN,NaN)', Object.is(NaN,NaN), true);
c('Object.is(0,-0)', Object.is(0,-0), false);
c('new Set([NaN,NaN]).size', new Set([NaN,NaN]).size, 1);
c('new Map().set(NaN,1).get(NaN)', new Map().set(NaN,1).get(NaN), 1);
var s=[3,1,NaN,2,0]; s.sort(function(a,b){return a-b;}); c('sort-no-crash len', s.length, 5);
c('+0===-0', +0===-0, true); c('1.5===1.5', 1.5===1.5, true); c('NaN===NaN', NaN===NaN, false);
print(f===0?'NAN2 ALL PASS':('NAN2 '+f+' FAIL'));
