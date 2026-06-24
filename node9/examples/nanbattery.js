var fail = 0;
function chk(label, got, want) { var ok = (got === want) || (typeof want === 'number' && isNaN(want) && isNaN(got)); if (!ok) { fail++; print('FAIL ' + label + ' got=' + got + ' want=' + want); } }
// relational vs NaN (all must be false)
chk('NaN<5', NaN < 5, false); chk('NaN>5', NaN > 5, false); chk('NaN<=5', NaN <= 5, false); chk('NaN>=5', NaN >= 5, false);
chk('5<NaN', 5 < NaN, false); chk('5>NaN', 5 > NaN, false);
chk('NaN<NaN', NaN < NaN, false); chk('NaN>NaN', NaN > NaN, false);
// float relational both-float (fast path uses ints only, floats hit slow path)
chk('1.5<2.5', 1.5 < 2.5, true); chk('2.5>1.5', 2.5 > 1.5, true); chk('2.5<=2.5', 2.5 <= 2.5, true); chk('2.5>=2.6', 2.5 >= 2.6, false);
chk('-0.1<0', -0.1 < 0, true);
// equality vs NaN
chk('NaN===NaN', NaN === NaN, false); chk('NaN==NaN', NaN == NaN, false); chk('NaN!==NaN', NaN !== NaN, true); chk('NaN!=NaN', NaN != NaN, true);
chk('0.5===0.5', 0.5 === 0.5, true);
// isNaN / Number.isNaN
chk('isNaN(NaN)', isNaN(NaN), true); chk('isNaN(5)', isNaN(5), false); chk('Number.isNaN(NaN)', Number.isNaN(NaN), true); chk('Number.isNaN("x")', Number.isNaN('x'), false);
// Math.min/max with NaN -> NaN
chk('Math.min(NaN,1)', Math.min(NaN, 1), NaN); chk('Math.max(NaN,1)', Math.max(NaN, 1), NaN);
chk('Math.min(3,1,2)', Math.min(3, 1, 2), 1); chk('Math.max(3,1,2)', Math.max(3, 1, 2), 3);
chk('Math.min(-0,0)', Math.min(-0, 0), -0);
// Infinity
chk('Inf>1e308', Infinity > 1e308, true); chk('-Inf<0', -Infinity < 0, true); chk('Inf===Inf', Infinity === Infinity, true);
chk('1/0', 1/0, Infinity); chk('-1/0', -1/0, -Infinity);
// floor/ceil/round/trunc
chk('floor(2.7)', Math.floor(2.7), 2); chk('ceil(2.1)', Math.ceil(2.1), 3); chk('round(2.5)', Math.round(2.5), 3); chk('trunc(-2.7)', Math.trunc(-2.7), -2);
chk('floor(NaN)', Math.floor(NaN), NaN);
// sort numeric with custom comparator returning floats (uses < internally? no, comparator result)
var a = [3.3, 1.1, 2.2, 0.5]; a.sort(function(x,y){ return x - y; });
chk('sort[0]', a[0], 0.5); chk('sort[3]', a[3], 3.3);
var b = [5, 3, 8, 1, 9, 2]; b.sort(function(x,y){ return x - y; });
chk('intsort', b.join(','), '1,2,3,5,8,9');
// sort with NaN-producing comparator shouldn't crash
var c = [1,2,3]; c.sort(function(x,y){ return NaN; }); chk('nan-sort-len', c.length, 3);
// parseFloat / Number
chk('parseFloat(3.14)', parseFloat('3.14'), 3.14); chk('Number("2.5")', Number('2.5'), 2.5); chk('parseInt(ff,16)', parseInt('ff',16), 255);
// arithmetic precision
chk('0.1+0.2', (0.1+0.2).toFixed(2), '0.30'); chk('toString', (255).toString(16), 'ff');
// comparison mixing int/float
chk('3<3.5', 3 < 3.5, true); chk('4>3.9', 4 > 3.9, true);
// clamp pattern (used a lot: Math.min(Math.max(x,lo),hi))
chk('clamp', Math.min(Math.max(5, 0), 10), 5); chk('clamp2', Math.min(Math.max(-3, 0), 10), 0); chk('clamp3', Math.min(Math.max(15, 0), 10), 10);
print(fail === 0 ? 'NAN/FLOAT BATTERY: ALL PASS' : ('NAN/FLOAT BATTERY: ' + fail + ' FAILURES'));
