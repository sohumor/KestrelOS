/* Host test harness for libjs.
 *
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -DJS_HOST \
 *       -Ilibjs -o /tmp/test_js tools/test_js.c libjs/[a-z]*.c && /tmp/test_js
 *
 * Modes:
 *   (none)     run the table and report pass/fail counts
 *   -v         also print every passing case
 *   --dump     print "index<TAB>result" for every case, for diffing against
 *              a reference engine (see --emit)
 *   --emit     write a Node script that produces the same TSV, so the two
 *              can be diffed directly:
 *                  ./test_js --emit > /tmp/ref.js && node /tmp/ref.js > /tmp/ref.tsv
 *                  ./test_js --dump > /tmp/ours.tsv && diff /tmp/ref.tsv /tmp/ours.tsv
 *   --limits   exercise the four safety caps and report what actually fired
 *   --embed    run the embedding demo: native functions, native property
 *              accessors and opaque host pointers, i.e. the surface the DOM
 *              binding layer is expected to build on
 *   --fuzz N   run N mutated programs through the parser and interpreter
 *
 * Stack budget. The KestrelOS user stack is 16 pages (64 KiB) with
 * unmapped pages below it, so overflow faults rather than corrupting.
 * Measured with a probe in the console.log callback: about 1.3 KiB of C
 * stack per nested JS call, about 430 bytes per level of expression
 * nesting, and the shipped defaults hold the worst adversarial program
 * measured (deepest legal call chain plus a catastrophically backtracking
 * regular expression) to roughly 32 KiB.
 *
 * Result encoding: a completion value is rendered with String(); a thrown
 * value is rendered as "!" followed by the error's name; a fatal limit is
 * "!Fatal". Programs are evaluated in a fresh context each time so that
 * one case cannot contaminate the next.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JS_INTERNAL
#include "js.h"

typedef struct { const char *src; const char *want; } tcase;

static const tcase cases[] = {

/* ---- arithmetic, precedence, associativity ---- */
{ "1+1", "2" },
{ "2+3*4", "14" },
{ "(2+3)*4", "20" },
{ "2-3-4", "-5" },
{ "2*3%4", "2" },
{ "-2*-3", "6" },
{ "7/2", "3.5" },
{ "7%3", "1" },
{ "-7%3", "-1" },
{ "7%-3", "1" },
{ "1/0", "Infinity" },
{ "-1/0", "-Infinity" },
{ "0/0", "NaN" },
{ "2*3+4*5", "26" },
{ "1<2", "true" },
{ "2<=2", "true" },
{ "'b'>'a'", "true" },
{ "'abc'<'abd'", "true" },
{ "'10'<'9'", "true" },
{ "10<9", "false" },
{ "1&&2", "2" },
{ "0&&2", "0" },
{ "1||2", "1" },
{ "0||2", "2" },
{ "!0", "true" },
{ "!!''", "false" },
{ "~5", "-6" },
{ "5&3", "1" },
{ "5|3", "7" },
{ "5^3", "6" },
{ "1<<10", "1024" },
{ "-8>>1", "-4" },
{ "-8>>>28", "15" },
{ "1<<32", "1" },
{ "2147483647+1|0", "-2147483648" },
{ "4294967296|0", "0" },
{ "(-1)>>>0", "4294967295" },
{ "1?2:3", "2" },
{ "0?2:3", "3" },
{ "true?false?1:2:3", "2" },
{ "(1,2,3)", "3" },
{ "0.1+0.2", "0.30000000000000004" },
{ "0.1+0.2===0.3", "false" },
{ "0.3-0.1", "0.19999999999999998" },
{ "1e308*10", "Infinity" },
{ "Math.pow(2,53)+1", "9007199254740992" },
{ "2**0", "1" },

/* ---- coercion corner cases ---- */
{ "'5'-2", "3" },
{ "'5'+2", "52" },
{ "5+'2'", "52" },
{ "'5'*'2'", "10" },
{ "[]+{}", "[object Object]" },
{ "[]+[]", "" },
{ "[1,2]+[3]", "1,23" },
{ "1+null", "1" },
{ "1+undefined", "NaN" },
{ "'a'+null", "anull" },
{ "'a'+undefined", "aundefined" },
{ "+true", "1" },
{ "+''", "0" },
{ "+' '", "0" },
{ "+'\\n'", "0" },
{ "+'12abc'", "NaN" },
{ "+'0x10'", "16" },
{ "+'-0x10'", "NaN" },
{ "+'1e3'", "1000" },
{ "+[]", "0" },
{ "+[5]", "5" },
{ "+[1,2]", "NaN" },
{ "+{}", "NaN" },
{ "null==undefined", "true" },
{ "null===undefined", "false" },
{ "null==0", "false" },
{ "null>=0", "true" },
{ "null>0", "false" },
{ "undefined==0", "false" },
{ "NaN==NaN", "false" },
{ "NaN!==NaN", "true" },
{ "[]==false", "true" },
{ "[]==0", "true" },
{ "'0'==false", "true" },
{ "''==0", "true" },
{ "'1'==1", "true" },
{ "[1]==1", "true" },
{ "({}=={})", "false" },
{ "0===-0", "true" },
{ "1/-0", "-Infinity" },
{ "String(-0)", "0" },
{ "typeof 1", "number" },
{ "typeof 'x'", "string" },
{ "typeof true", "boolean" },
{ "typeof undefined", "undefined" },
{ "typeof null", "object" },
{ "typeof {}", "object" },
{ "typeof []", "object" },
{ "typeof function(){}", "function" },
{ "typeof Math", "object" },
{ "typeof nothingAtAll", "undefined" },
{ "void 0", "undefined" },
{ "String(null)", "null" },
{ "String(undefined)", "undefined" },
{ "String([1,[2,3]])", "1,2,3" },
{ "String({})", "[object Object]" },
{ "Number('')", "0" },
{ "Number(null)", "0" },
{ "Number(undefined)", "NaN" },
{ "Number(true)", "1" },
{ "Number([])", "0" },
{ "Boolean('')", "false" },
{ "Boolean('0')", "true" },
{ "Boolean(NaN)", "false" },
{ "Boolean([])", "true" },

/* ---- number formatting ---- */
{ "String(1e21)", "1e+21" },
{ "String(1e20)", "100000000000000000000" },
{ "String(1e-6)", "0.000001" },
{ "String(1e-7)", "1e-7" },
{ "String(123456789012345678901234)", "1.2345678901234569e+23" },
{ "String(0.000001)", "0.000001" },
{ "String(1/3)", "0.3333333333333333" },
{ "String(2/3)", "0.6666666666666666" },
{ "String(5e-324)", "5e-324" },
{ "String(1.7976931348623157e308)", "1.7976931348623157e+308" },
{ "String(-1.5)", "-1.5" },
{ "String(100)", "100" },
{ "(255).toString(16)", "ff" },
{ "(255).toString(2)", "11111111" },
{ "(-255).toString(16)", "-ff" },
{ "(3.5).toString(2)", "11.1" },
{ "(1234.5678).toFixed(0)", "1235" },
{ "(1234.5678).toFixed(3)", "1234.568" },
{ "(0.5).toFixed(0)", "1" },
{ "(1.45).toFixed(1)", "1.4" },
{ "(1.005).toFixed(2)", "1.00" },
{ "(0).toFixed(2)", "0.00" },
{ "(-1.5).toFixed(0)", "-2" },
{ "(123.456).toPrecision(4)", "123.5" },
{ "(0.000123).toPrecision(2)", "0.00012" },
{ "(123456).toPrecision(2)", "1.2e+5" },
{ "(77.1234).toExponential(2)", "7.71e+1" },
{ "(5).toExponential()", "5e+0" },
{ "parseInt('42')", "42" },
{ "parseInt('42abc')", "42" },
{ "parseInt('abc')", "NaN" },
{ "parseInt('0x1f')", "31" },
{ "parseInt('11',2)", "3" },
{ "parseInt('  -17 ')", "-17" },
{ "parseInt('08')", "8" },
{ "parseFloat('3.14abc')", "3.14" },
{ "parseFloat('.5')", "0.5" },
{ "parseFloat('abc')", "NaN" },
{ "isNaN('abc')", "true" },
{ "isFinite('12')", "true" },
{ "isFinite(Infinity)", "false" },
{ "Number.MAX_VALUE", "1.7976931348623157e+308" },
{ "Number.MIN_VALUE", "5e-324" },

/* ---- strings ---- */
{ "'abc'.length", "3" },
{ "'abc'[1]", "b" },
{ "'abc'.charAt(1)", "b" },
{ "'abc'.charAt(9)", "" },
{ "'abc'.charCodeAt(0)", "97" },
{ "'abc'.charCodeAt(9)", "NaN" },
{ "String.fromCharCode(72,105)", "Hi" },
{ "'hello'.indexOf('l')", "2" },
{ "'hello'.indexOf('l',3)", "3" },
{ "'hello'.indexOf('z')", "-1" },
{ "'hello'.lastIndexOf('l')", "3" },
{ "'hello'.slice(1,3)", "el" },
{ "'hello'.slice(-3)", "llo" },
{ "'hello'.slice(3,1)", "" },
{ "'hello'.substring(3,1)", "el" },
{ "'hello'.substring(-2,2)", "he" },
{ "'hello'.substr(1,3)", "ell" },
{ "'hello'.substr(-3,2)", "ll" },
{ "'a,b,,c'.split(',').length", "4" },
{ "'a,b,c'.split(',')[1]", "b" },
{ "'abc'.split('')", "a,b,c" },
{ "'abc'.split('',2)", "a,b" },
{ "'a1b2c'.split(/[0-9]/)", "a,b,c" },
{ "'one two'.replace('two','2')", "one 2" },
{ "'aaa'.replace('a','b')", "baa" },
{ "'aaa'.replace(/a/g,'b')", "bbb" },
{ "'John Smith'.replace(/(\\w+)\\s(\\w+)/,'$2 $1')", "Smith John" },
{ "'abc'.replace(/b/,function(m){return m.toUpperCase()})", "aBc" },
{ "'abc'.toUpperCase()", "ABC" },
{ "'ABC'.toLowerCase()", "abc" },
{ "'  x  '.trim()", "x" },
{ "'a'.concat('b','c')", "abc" },
{ "'abc'.localeCompare('abd')", "-1" },
{ "'abc'+'def'", "abcdef" },
{ "'abc'.split('').reverse().join('')", "cba" },
{ "'%'+encodeURIComponent('a b&c')", "%a%20b%26c" },
{ "decodeURIComponent('a%20b')", "a b" },
{ "new TextEncoder().encoding", "utf-8" },
{ "Array.prototype.join.call(new TextEncoder().encode('Aé'))", "65,195,169" },
{ "var d=new Uint8Array(3);var r=new TextEncoder().encodeInto('AéB',d);r.read+','+r.written+','+Array.prototype.join.call(d)", "3,3,65,195,169" },
{ "new TextDecoder().decode(new Uint8Array([65,195,169]))", "Aé" },
{ "new TextDecoder().decode(new Uint8Array([239,187,191,65]))", "A" },
{ "new TextDecoder('utf8',{ignoreBOM:true}).decode(new Uint8Array([239,187,191,65])).length", "4" },
{ "new TextDecoder().decode(new Uint8Array([255])).charCodeAt(0)", "239" },
{ "try{new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array([255]));'no'}catch(e){e.name}", "TypeError" },
{ "try{new TextDecoder('windows-1252');'no'}catch(e){e.name}", "RangeError" },
{ "try{TextEncoder();'no'}catch(e){e.name}", "TypeError" },
{ "var p=new URLSearchParams('?a=1&a=2&b=hello+world&x=%26');p.size+','+p.get('a')+','+p.getAll('a').join(',')+','+p.get('b')+','+p.get('x')", "4,1,1,2,hello world,&" },
{ "var p=new URLSearchParams('?a=1&a=2&b=hello+world&x=%26');p.set('a','3');p.append('c','a b');p.delete('x');p.sort();p.toString()", "a=3&b=hello+world&c=a+b" },
{ "var p=new URLSearchParams('a=1&a=2');p.delete('a','1');p.has('a','1')+','+p.has('a','2')+','+p.toString()", "false,true,a=2" },
{ "new URLSearchParams([['z',2],['a','x y']]).toString()", "z=2&a=x+y" },
{ "new URLSearchParams({a:1,b:'x y'}).toString()", "a=1&b=x+y" },
{ "var p=new URLSearchParams('a=1&b=2'),s='';p.forEach((v,k)=>s+=k+v);s", "a1b2" },
{ "var a=new URLSearchParams('q=é');var b=new URLSearchParams(a);b.toString()", "q=%C3%A9" },
{ "new URLSearchParams().get('missing')===null", "true" },
{ "try{URLSearchParams('a=1');'no'}catch(e){e.name}", "TypeError" },
{ "try{new URLSearchParams().append('a');'no'}catch(e){e.name}", "TypeError" },
{ "typeof 'x'.repeat", "function" },
{ "typeof ''.trim", "function" },
{ "new String('ab').length", "2" },
{ "typeof new String('ab')", "object" },
{ "new String('ab')=='ab'", "true" },
{ "new String('ab')==='ab'", "false" },

/* ---- arrays ---- */
{ "[1,2,3].length", "3" },
{ "[1,2,3][1]", "2" },
{ "[1,2,3][9]", "undefined" },
{ "var a=[1,2,3]; a.push(4); a.join()", "1,2,3,4" },
{ "var a=[1,2,3]; a.push(4)", "4" },
{ "var a=[1,2,3]; a.pop()", "3" },
{ "var a=[1,2,3]; a.pop(); a.length", "2" },
{ "var a=[1,2,3]; a.shift()", "1" },
{ "var a=[1,2,3]; a.shift(); a.join()", "2,3" },
{ "var a=[1,2]; a.unshift(0); a.join()", "0,1,2" },
{ "[1,2,3,4].slice(1,3).join()", "2,3" },
{ "[1,2,3,4].slice(-2).join()", "3,4" },
{ "var a=[1,2,3,4]; a.splice(1,2).join()", "2,3" },
{ "var a=[1,2,3,4]; a.splice(1,2); a.join()", "1,4" },
{ "var a=[1,4]; a.splice(1,0,2,3); a.join()", "1,2,3,4" },
{ "var a=[1,2,3]; a.splice(1,1,'x','y'); a.join()", "1,x,y,3" },
{ "[1,2].concat([3,4],5).join()", "1,2,3,4,5" },
{ "[1,2,3].indexOf(2)", "1" },
{ "[1,2,3].indexOf(9)", "-1" },
{ "[1,2,1].lastIndexOf(1)", "2" },
{ "[1,2,3].join('-')", "1-2-3" },
{ "[1,null,3].join('-')", "1--3" },
{ "[1,2,3].reverse().join()", "3,2,1" },
{ "[3,1,2].sort().join()", "1,2,3" },
{ "[10,9,1].sort().join()", "1,10,9" },
{ "[10,9,1].sort(function(a,b){return a-b}).join()", "1,9,10" },
{ "['b','a','c'].sort().join()", "a,b,c" },
{ "var n=0; [1,2,3].forEach(function(x){n+=x}); n", "6" },
{ "[1,2,3].map(function(x){return x*2}).join()", "2,4,6" },
{ "[1,2,3,4].filter(function(x){return x%2==0}).join()", "2,4" },
{ "[1,2,3].reduce(function(a,b){return a+b})", "6" },
{ "[1,2,3].reduce(function(a,b){return a+b},10)", "16" },
{ "[1,2,3].reduceRight(function(a,b){return a+''+b})", "321" },
{ "[1,2,3].every(function(x){return x>0})", "true" },
{ "[1,2,3].some(function(x){return x>2})", "true" },
{ "[].reduce(function(a,b){return a+b})", "!TypeError" },
{ "Array.isArray([])", "true" },
{ "Array.isArray({})", "false" },
{ "new Array(3).length", "3" },
{ "new Array(1,2).length", "2" },
{ "Array(2,3).join()", "2,3" },
{ "var a=[]; a[3]=1; a.length", "4" },
{ "var a=[1,2,3]; a.length=1; a.join()", "1" },
{ "[1,2,].length", "2" },
{ "[,1].length", "2" },
{ "[1,2,3].map(function(x,i){return i}).join()", "0,1,2" },
{ "var a=[1,2,3]; delete a[1]; a[1]", "undefined" },
{ "[1,[2,[3]]].join()", "1,2,3" },
{ "var s=''; for(var k in [7,8]) s+=k; s", "01" },

/* ---- objects and prototypes ---- */
{ "var o={a:1,b:2}; o.a+o.b", "3" },
{ "var o={a:1}; o['a']", "1" },
{ "var o={}; o.x=5; o.x", "5" },
{ "var o={a:1}; 'a' in o", "true" },
{ "var o={a:1}; 'b' in o", "false" },
{ "var o={a:1}; delete o.a; 'a' in o", "false" },
{ "var o={a:1,b:2}; Object.keys(o).join()", "a,b" },
{ "var o={a:1}; o.hasOwnProperty('a')", "true" },
{ "var o={a:1}; o.hasOwnProperty('toString')", "false" },
{ "var o={}; 'toString' in o", "true" },
{ "var s=''; for(var k in {a:1,b:2}) s+=k; s", "ab" },
{ "var o={if:1,for:2}; o.if+o.for", "3" },
{ "var o={1:'a'}; o[1]", "a" },
{ "function F(){}; F.prototype.x=1; new F().x", "1" },
{ "function F(){this.y=2}; new F().y", "2" },
{ "function F(){}; new F() instanceof F", "true" },
{ "function F(){}; ({}) instanceof F", "false" },
{ "[] instanceof Array", "true" },
{ "[] instanceof Object", "true" },
{ "(function(){}) instanceof Function", "true" },
{ "function F(){}; Object.getPrototypeOf(new F())===F.prototype", "true" },
{ "var o=Object.create(null); typeof o.toString", "undefined" },
{ "var o=Object.create({z:9}); o.z", "9" },
{ "function A(){}; function B(){}; B.prototype=new A(); new B() instanceof A", "true" },
{ "var o={get x(){return 42}}; o.x", "42" },
{ "var o={_v:0,set x(v){this._v=v*2}}; o.x=5; o._v", "10" },
{ "var o={}; Object.defineProperty(o,'a',{value:7}); o.a", "7" },
{ "var o={}; Object.defineProperty(o,'a',{value:7}); Object.keys(o).length", "0" },
{ "var o={}; Object.defineProperty(o,'a',{value:7}); o.a=9; o.a", "7" },
{ "var o={a:1}; Object.freeze(o); o.a=2; o.a", "1" },
{ "var o={a:1}; Object.freeze(o); Object.isFrozen(o)", "true" },
{ "var o={a:1}; Object.preventExtensions(o); o.b=2; o.b", "undefined" },
{ "var a=[1,2]; Object.freeze(a); a[0]=9; Object.isFrozen(a) && a[0]===1", "true" },
{ "var a=[1,2]; Object.freeze(a); try{a.push(3); 'no'}catch(e){e.name}", "TypeError" },
{ "var a=[1,2]; Object.seal(a); a[0]=5; Object.isSealed(a) && !Object.isFrozen(a) && a[0]===5", "true" },
{ "Object.getOwnPropertyDescriptor({a:1},'a').writable", "true" },
{ "({}).toString()", "[object Object]" },
{ "[].toString()", "" },
{ "Object.prototype.toString.call([])", "[object Array]" },
{ "Object.prototype.toString.call(null)", "[object Null]" },
{ "Object.prototype.toString.call(3)", "[object Number]" },
{ "var o={valueOf:function(){return 7}}; o*2", "14" },
{ "var o={toString:function(){return 'z'}}; 'a'+o", "az" },
{ "var o={valueOf:function(){return 7},toString:function(){return 'z'}}; o+''", "7" },
{ "var o={valueOf:function(){return 7},toString:function(){return 'z'}}; String(o)", "z" },
{ "var o={a:{b:{c:5}}}; o.a.b.c", "5" },
{ "var o={a:1}; var p={}; p.__proto__===undefined", "true" },

/* ---- functions, closures, this ---- */
{ "(function(){return 1})()", "1" },
{ "(function(a,b){return a+b})(1,2)", "3" },
{ "(function(a,b){return b})(1)", "undefined" },
{ "(function(){return arguments.length})(1,2,3)", "3" },
{ "(function(){return arguments[1]})(1,2)", "2" },
{ "((a,b)=>a+b)(20,22)", "42" },
{ "(()=>42)()", "42" },
{ "(x=>{var y=x*2;return y+1})(5)", "11" },
{ "(function(){var f=()=>this.x;return f.call({x:99})}).call({x:7})", "7" },
{ "(function(x){return (()=>arguments[0])()})(11)", "11" },
{ "(function(){var f=()=>42;try{new f();return false}catch(e){return e.name}})()", "TypeError" },
{ "function f(){return f.length}; f(1,2,3)", "0" },
{ "function f(a,b){return f.length}; f()", "2" },
{ "function f(){}; f.name", "f" },
{ "var f=function g(){return typeof g}; f()", "function" },
{ "function outer(){var x=1;return function(){return ++x}}; var c=outer(); c(); c()", "3" },
{ "var fs=[]; for(var i=0;i<3;i++){fs.push(function(){return i})}; fs[0]()+','+fs[2]()", "3,3" },
{ "var fs=[]; for(var i=0;i<3;i++){(function(j){fs.push(function(){return j})})(i)}; fs[0]()+','+fs[2]()", "0,2" },
{ "function counter(){var n=0;return{inc:function(){return ++n}}}; var c=counter(); c.inc(); c.inc()", "2" },
{ "var o={v:5,get:function(){return this.v}}; o.get()", "5" },
{ "var o={v:5,get:function(){return this.v}}; var g=o.get; typeof g()", "undefined" },
{ "function f(){return this===undefined}; f()", "false" },
{ "var o={v:1}; function f(){return this.v}; f.call(o)", "1" },
{ "var o={v:1}; function f(a){return this.v+a}; f.apply(o,[2])", "3" },
{ "var o={v:1}; function f(a,b){return this.v+a+b}; f.bind(o,10)(100)", "111" },
{ "function f(){return 1}; f.call()", "1" },
{ "(function(){return typeof arguments.callee})()", "function" },
{ "function fact(n){return n<2?1:n*fact(n-1)}; fact(10)", "3628800" },
{ "function fib(n){return n<2?n:fib(n-1)+fib(n-2)}; fib(15)", "610" },
{ "var x=1; function f(){var x=2; return x}; f()+','+x", "2,1" },
{ "var x=1; function f(){x=2}; f(); x", "2" },
{ "function f(){return g()}; function g(){return 7}; f()", "7" },
{ "typeof hoisted; function hoisted(){}", "function" },
{ "var f=function(){return 1}; var g=f; g()", "1" },
{ "(function(){ return (function(){ return (function(){ return 3 })() })() })()", "3" },
{ "function F(){this.a=1}; new F().constructor===F", "true" },

/* ---- control flow, labels, exceptions ---- */
{ "var s=0; for(var i=0;i<5;i++) s+=i; s", "10" },
{ "var s=0,i=0; while(i<5){s+=i;i++} s", "10" },
{ "var s=0,i=0; do{s+=i;i++}while(i<5); s", "10" },
{ "var s=0; for(var i=0;i<10;i++){if(i%2)continue; s+=i} s", "20" },
{ "var s=0; for(var i=0;i<10;i++){if(i>4)break; s+=i} s", "10" },
{ "var s=''; outer: for(var i=0;i<3;i++){for(var j=0;j<3;j++){if(j==1)continue outer; s+=''+i+j}} s", "001020" },
{ "var s=''; outer: for(var i=0;i<3;i++){for(var j=0;j<3;j++){if(i==1)break outer; s+=''+i+j}} s", "000102" },
{ "var s=0; blk: { s=1; break blk; s=2 } s", "1" },
{ "var x=2,s=''; switch(x){case 1:s='a';case 2:s+='b';case 3:s+='c';break;default:s+='d'} s", "bc" },
{ "var x=9,s=''; switch(x){case 1:s='a';break;default:s='d'} s", "d" },
{ "var x='2',s=''; switch(x){case 2:s='num';break;case '2':s='str';break} s", "str" },
{ "try{throw 1}catch(e){e}", "1" },
{ "try{throw new Error('x')}catch(e){e.message}", "x" },
{ "try{throw new TypeError('x')}catch(e){e.name}", "TypeError" },
{ "try{throw 1}catch(e){e}finally{}", "1" },
{ "var s=''; try{s+='t'}finally{s+='f'} s", "tf" },
{ "var s=''; try{throw 1}catch(e){s+='c'}finally{s+='f'} s", "cf" },
{ "function f(){try{return 1}finally{}}; f()", "1" },
{ "function f(){try{return 1}finally{return 2}}; f()", "2" },
{ "function f(){var s='';try{try{throw 1}finally{s+='a'}}catch(e){s+='b'}finally{s+='c'} return s}; f()", "abc" },
{ "function f(){for(var i=0;i<3;i++){try{if(i==1)continue}finally{}} return i}; f()", "3" },
{ "try{null.x}catch(e){e.name}", "TypeError" },
{ "try{undefinedFn()}catch(e){e.name}", "ReferenceError" },
{ "try{(1)()}catch(e){e.name}", "TypeError" },
{ "try{JSON.parse('{')}catch(e){e.name}", "SyntaxError" },
{ "try{[].length=-1}catch(e){e.name}", "RangeError" },
{ "var e=new Error('m'); e.toString()", "Error: m" },
{ "new RangeError('r').toString()", "RangeError: r" },
{ "(function(){try{return 'a'}finally{}})()", "a" },
{ "throw 5", "!5" },
{ "(function(){var i=0; while(true){i++; if(i>100)break} return i})()", "101" },

/* ---- ASI and parsing ---- */
{ "var a=1\nvar b=2\na+b", "3" },
{ "function f(){return\n1}; f()", "undefined" },
{ "function f(){return 1\n}; f()", "1" },
{ "var a=1;var b=2\nb", "2" },
{ "var x=1\n++x\nx", "2" },
{ "var a=1,b=1\nvar c=a\n+b\nc", "2" },
{ "/* c */ 1 // c\n+2", "3" },
{ "var a = /a+/.source; a", "a+" },
{ "1/2/2", "0.25" },
{ "var x=4; x /2/ 2", "1" },
{ "if(1)2;else 3", "2" },
{ ";;;1", "1" },
{ "{1;2}", "2" },
{ "var o={a:1,}; o.a", "1" },
{ "'a\\tb'.length", "3" },
{ "'\\u0041'", "A" },
{ "'\\x41'", "A" },
{ "'a\\\nb'", "ab" },
{ "0x10", "16" },
{ "010", "8" },
{ "09", "9" },
{ "1.5e3", "1500" },
{ ".5", "0.5" },
{ "1..toString()", "1" },
{ "var x=1; x\n=2; x", "2" },

/* ---- JSON ---- */
{ "JSON.stringify(1)", "1" },
{ "JSON.stringify('a')", "\"a\"" },
{ "JSON.stringify(null)", "null" },
{ "JSON.stringify(true)", "true" },
{ "String(JSON.stringify(undefined))", "undefined" },
{ "JSON.stringify([1,2])", "[1,2]" },
{ "JSON.stringify({a:1})", "{\"a\":1}" },
{ "JSON.stringify({a:undefined})", "{}" },
{ "JSON.stringify({a:function(){}})", "{}" },
{ "JSON.stringify([undefined])", "[null]" },
{ "JSON.stringify(NaN)", "null" },
{ "JSON.stringify({a:'x\\ny'})", "{\"a\":\"x\\ny\"}" },
{ "JSON.stringify({a:1,b:[2,{c:3}]})", "{\"a\":1,\"b\":[2,{\"c\":3}]}" },
{ "JSON.stringify({a:1},null,2)", "{\n  \"a\": 1\n}" },
{ "JSON.parse('1')", "1" },
{ "JSON.parse('\"a\"')", "a" },
{ "JSON.parse('[1,2]').length", "2" },
{ "JSON.parse('{\"a\":1}').a", "1" },
{ "JSON.parse('{\"a\":[1,{\"b\":2}]}').a[1].b", "2" },
{ "JSON.parse(JSON.stringify({a:[1,2],b:'x'})).a[1]", "2" },
{ "JSON.parse('true')", "true" },
{ "JSON.parse('null')", "null" },
{ "JSON.parse('  {\"a\" : 1 } ').a", "1" },
{ "JSON.parse('\"\\\\u0041\"')", "A" },
{ "try{JSON.parse('{a:1}')}catch(e){e.name}", "SyntaxError" },
{ "try{JSON.parse('[1,]')}catch(e){e.name}", "SyntaxError" },
{ "JSON.parse('[1,2,3]',function(k,v){return typeof v==='number'?v*2:v}).join()", "2,4,6" },

/* ---- Math ---- */
{ "Math.abs(-3)", "3" },
{ "Math.ceil(1.1)", "2" },
{ "Math.ceil(-1.1)", "-1" },
{ "Math.floor(1.9)", "1" },
{ "Math.floor(-1.1)", "-2" },
{ "Math.round(1.5)", "2" },
{ "Math.round(-1.5)", "-1" },
{ "Math.round(2.5)", "3" },
{ "Math.round(0.49999999999999994)", "0" },
{ "Math.max(1,2,3)", "3" },
{ "Math.min(1,2,3)", "1" },
{ "Math.max()", "-Infinity" },
{ "Math.min()", "Infinity" },
{ "Math.max(1,NaN)", "NaN" },
{ "Math.pow(2,10)", "1024" },
{ "Math.pow(2,-1)", "0.5" },
{ "Math.pow(2,0.5)", "1.4142135623730951" },
{ "Math.pow(-8,1/3)", "NaN" },
{ "Math.sqrt(16)", "4" },
{ "Math.sqrt(-1)", "NaN" },
{ "Math.sqrt(2)", "1.4142135623730951" },
{ "Math.exp(0)", "1" },
{ "Math.exp(1)", "2.718281828459045" },
{ "Math.log(1)", "0" },
{ "Math.log(Math.E)", "1" },
{ "Math.log(0)", "-Infinity" },
{ "Math.sin(0)", "0" },
{ "Math.cos(0)", "1" },
{ "Math.atan2(1,1)", "0.7853981633974483" },
{ "Math.PI", "3.141592653589793" },
{ "Math.E", "2.718281828459045" },
{ "Math.LN2", "0.6931471805599453" },
{ "Math.SQRT2", "1.4142135623730951" },
{ "typeof Math.random()", "number" },
{ "Math.random()>=0 && Math.random()<1", "true" },
{ "Math.cbrt(0)", "0" },
{ "1/Math.cbrt(-0)", "-Infinity" },
{ "Math.abs(Math.cbrt(27) - 3) < 1e-14", "true" },
{ "Math.abs(Math.cbrt(-8) + 2) < 1e-14", "true" },
{ "Math.cbrt(NaN)", "NaN" },
{ "Math.cbrt(Infinity)", "Infinity" },
{ "Math.cbrt(-Infinity)", "-Infinity" },
{ "Math.clz32(0)", "32" },
{ "Math.clz32(1)", "31" },
{ "Math.clz32(0x100)", "23" },
{ "Math.clz32(-1)", "0" },
{ "Math.clz32(0x80000000)", "0" },
{ "Math.clz32('16')", "27" },
{ "Math.log2(2)", "1" },
{ "Math.abs(Math.log2(8) - 3) < 1e-14", "true" },
{ "Math.log2(0)", "-Infinity" },
{ "Math.log2(-1)", "NaN" },
{ "Math.abs(Math.log10(1000) - 3) < 1e-14", "true" },
{ "Math.log10(0)", "-Infinity" },
{ "Math.fround(1.5)", "1.5" },
{ "Math.fround(1e40)", "Infinity" },
{ "Math.fround(1e-50)", "0" },
{ "Math.fround(NaN)", "NaN" },
{ "Math.fround(1.337)", "1.3370000123977661" },

/* ---- regular expressions ---- */
{ "/abc/.test('xabcy')", "true" },
{ "/^abc$/.test('abc')", "true" },
{ "/^abc$/.test('xabc')", "false" },
{ "/a.c/.test('abc')", "true" },
{ "/a.c/.test('a\\nc')", "false" },
{ "/[a-c]+/.exec('xxabcyy')[0]", "abc" },
{ "/[^a-c]+/.exec('abcxyz')[0]", "xyz" },
{ "/a*/.exec('aaa')[0]", "aaa" },
{ "/a+?/.exec('aaa')[0]", "a" },
{ "/a{2,3}/.exec('aaaa')[0]", "aaa" },
{ "/a{2}/.exec('aaaa')[0]", "aa" },
{ "/a{2,}/.exec('aaaa')[0]", "aaaa" },
{ "/(ab)+/.exec('ababab')[0]", "ababab" },
{ "/(a)(b)/.exec('ab')[2]", "b" },
{ "/(a)|(b)/.exec('b')[1]", "undefined" },
{ "/x(y)?z/.exec('xz')[1]", "undefined" },
{ "/(\\d+)-(\\d+)/.exec('10-20')[1]", "10" },
{ "/\\bfoo\\b/.test('a foo b')", "true" },
{ "/\\bfoo\\b/.test('afoob')", "false" },
{ "/\\Bfoo/.test('afoo')", "true" },
{ "/abc/i.test('ABC')", "true" },
{ "/[a-c]/i.test('B')", "true" },
{ "/^b/m.test('a\\nb')", "true" },
{ "/^b/.test('a\\nb')", "false" },
{ "/(a+)\\1/.test('aaaa')", "true" },
{ "/(a)\\1/.test('ab')", "false" },
{ "/a(?=b)/.test('ab')", "true" },
{ "/a(?=b)/.test('ac')", "false" },
{ "/a(?!b)/.test('ac')", "true" },
{ "/(?:ab)+/.exec('abab')[0]", "abab" },
{ "/a|ab/.exec('ab')[0]", "a" },
{ "'2020-01-02'.match(/(\\d+)-(\\d+)-(\\d+)/)[3]", "02" },
{ "'a1b2'.match(/\\d/g).join()", "1,2" },
{ "String('abc'.match(/z/g))", "null" },
{ "'aaa'.search(/a/)", "0" },
{ "'xyz'.search(/y/)", "1" },
{ "'xyz'.search(/q/)", "-1" },
{ "var r=/a/g; r.exec('aa'); r.lastIndex", "1" },
{ "var r=/a/g; r.exec('aa'); r.exec('aa')[0]", "a" },
{ "var r=/a/g; r.exec('aa'); r.exec('aa'); String(r.exec('aa'))", "null" },
{ "/a/.source", "a" },
{ "/a/gi.global", "true" },
{ "/a/gi.ignoreCase", "true" },
{ "/a/.multiline", "false" },
{ "String(/ab/g)", "/ab/g" },
{ "new RegExp('a+').test('caaa')", "true" },
{ "new RegExp('a','g').global", "true" },
{ "'a-b-c'.replace(/-/g,'+')", "a+b+c" },
{ "'abc'.replace(/(b)/,'[$1]')", "a[b]c" },
{ "'abc'.replace(/b/,'$&$&')", "abbc" },
{ "'abc'.replace(/b/,'$`')", "aac" },
{ "'abc'.replace(/b/,\"$'\")", "acc" },
{ "'abc'.replace(/b/,'$$')", "a$c" },
{ "'a1b'.split(/(\\d)/).join('|')", "a|1|b" },
{ "/\\s+/.exec(' \\t ')[0].length", "3" },
{ "/\\w+/.exec('__ab12!')[0]", "__ab12" },
{ "/[\\d]+/.exec('ab123')[0]", "123" },
{ "try{new RegExp('(')}catch(e){e.name}", "SyntaxError" },
{ "try{new RegExp('a','q')}catch(e){e.name}", "SyntaxError" },

/* ---- Date ---- */
{ "typeof new Date().getTime()", "number" },
{ "new Date(0).getTime()", "0" },
{ "new Date(0).toISOString()", "1970-01-01T00:00:00.000Z" },
{ "new Date(86400000).getUTCDate()", "2" },
{ "new Date(Date.UTC(2020,0,2)).toISOString()", "2020-01-02T00:00:00.000Z" },
{ "new Date('2020-01-02T03:04:05Z').getUTCHours()", "3" },
{ "new Date('2020-01-02').getUTCFullYear()", "2020" },
{ "new Date('2020-01-02').getUTCMonth()", "0" },
{ "Date.parse('1970-01-01T00:00:00Z')", "0" },
{ "String(Date.parse('not a date'))", "NaN" },
{ "new Date(1970,0,1).getTime()", "0" },
{ "new Date(0).getUTCDay()", "4" },
{ "typeof Date.now()", "number" },
{ "var d=new Date(0); d.setTime(1000); d.getTime()", "1000" },
{ "new Date(NaN).toString()", "Invalid Date" },
{ "JSON.stringify(new Date(0))", "\"1970-01-01T00:00:00.000Z\"" },

/* ---- common ES2015+ static built-ins ---- */
{ "var target={a:0};var out=Object.assign(target,{a:1,b:2},null,{c:3});(out===target)+','+target.a+target.b+target.c", "true,123" },
{ "var source={a:1};Object.defineProperty(source,'hidden',{value:2,enumerable:false});var out=Object.assign({},source);out.a+','+String(out.hidden)", "1,undefined" },
{ "Object.is(NaN,NaN)+','+Object.is(0,-0)+','+Object.is(-0,-0)+','+Object.is({}, {})", "true,false,true,false" },
{ "Object.values({a:1,b:2}).join(',')", "1,2" },
{ "Object.entries({a:1,b:2}).map(function(x){return x.join(':')}).join(',')", "a:1,b:2" },
{ "Array.from({0:'a',1:'b',length:2}).join('')", "ab" },
{ "Array.from('kestrel').join('-')", "k-e-s-t-r-e-l" },
{ "Array.from([1,2,3],function(x,i){return x*this.n+i},{n:2}).join(',')", "2,5,8" },
{ "Array.of(3).length+','+Array.of(3)[0]+','+Array.of('a','b').join('')", "1,3,ab" },
{ "Number.isFinite(2)+','+Number.isFinite('2')+','+Number.isNaN(NaN)+','+Number.isNaN('x')", "true,false,true,false" },
{ "Number.isInteger(2)+','+Number.isInteger(2.5)+','+Number.isSafeInteger(9007199254740991)+','+Number.isSafeInteger(9007199254740992)", "true,false,true,false" },
{ "Number.parseInt('10',2)+','+Number.parseFloat('1.5x')+','+(Number.EPSILON>0)+','+Number.MAX_SAFE_INTEGER", "2,1.5,true,9007199254740991" },

/* ---- common modern Array and String methods ---- */
{ "[1,NaN,3].includes(NaN)+','+[1,2,3].includes(1,-3)+','+[1,2,3].includes(1,1)", "true,true,false" },
{ "[1,4,7].find(function(x){return x>3})+','+[1,4,7].findIndex(function(x){return x>4})", "4,2" },
{ "var scope={n:3};[1,2].find(function(x,i,a){return x*this.n===6&&i===1&&a.length===2},scope)", "2" },
{ "String([1,2].find(function(){return false}))+','+[1,2].findIndex(function(){return false})", "undefined,-1" },
{ "'kestrel-browser'.includes('browser')+','+'kestrel'.startsWith('est',1)+','+'kestrel'.endsWith('str',5)", "true,true,true" },
{ "'ab'.repeat(3)+','+'x'.repeat(0)", "ababab," },
{ "'7'.padStart(3,'0')+','+'x'.padEnd(4,'ab')+','+'x'.padStart(3)", "007,xaba,  x" },
{ "try{'x'.includes(/x/);'no'}catch(e){e.name}", "TypeError" },
{ "try{'x'.repeat(-1);'no'}catch(e){e.name}", "RangeError" },
{ "try{'x'.padEnd(1048577,'x');'no'}catch(e){e.name}", "RangeError" },
{ "[1,[2,[3]],4].flat().join(',')+','+[1,[2,[3]],4].flat(2).join(',')", "1,2,3,4,1,2,3,4" },
{ "[1,2].flatMap(function(x,i,a){return [x,x+i+a.length]}).join(',')", "1,3,2,5" },
{ "try{[1].flat(33);'no'}catch(e){e.name}", "RangeError" },
{ "' a a '.replaceAll('a','x')", " x x " },
{ "'aaa'.replaceAll('a',function(m,i,s){return m+i+s.length})", "a03a13a23" },
{ "'aba'.replaceAll(/a/g,'x')", "xbx" },
{ "try{'aba'.replaceAll(/a/,'x');'no'}catch(e){e.name}", "TypeError" },
{ "'  kestrel  '.trimStart()+','+'  kestrel  '.trimEnd()+','+'  kestrel  '.trimLeft().trimRight()", "kestrel  ,  kestrel,kestrel" },
{ "var i=['a','b'].keys();i.next().value+','+i.next().value+','+i.next().done", "0,1,true" },
{ "var i=['a','b'].values();i.next().value+i.next().value+','+i.next().done", "ab,true" },
{ "var i=['a','b'].entries(),a=i.next().value,b=i.next().value;a.join(':')+','+b.join(':')", "0:a,1:b" },
{ "var o=Object.fromEntries([['a',1],['b',2],['a',3]]);o.a+','+o.b+','+Object.keys(o).join('')", "3,2,ab" },
{ "var o=Object.fromEntries(new Map([['a',1],['b',2]]));o.a+o.b", "3" },
{ "var o=Object.fromEntries(new Map([['a',1],['b',2]]).entries());o.a+','+o.b", "1,2" },
{ "try{Object.fromEntries([1]);'no'}catch(e){e.name}", "TypeError" },

/* ---- bounded Map and Set ---- */
{ "var m=new Map([['a',1],['b',2],['a',3]]);m.size+','+m.get('a')+','+m.has('b')+','+(m instanceof Map)+','+Object.prototype.toString.call(m)", "2,3,true,true,[object Map]" },
{ "var m=new Map();m.set(NaN,'nan').set(-0,'zero');m.get(NaN)+','+m.get(0)+','+m.size", "nan,zero,2" },
{ "var a={},b={},m=new Map();m.set(a,1);m.has(a)+','+m.has(b)+','+String(m.get(b))", "true,false,undefined" },
{ "var m=new Map([['a',1],['b',2],['c',3]]);m.delete('b');var i=m.entries(),a=i.next(),b=i.next(),c=i.next();a.value.join('')+b.value.join('')+','+c.done+','+m.size", "a1c3,true,2" },
{ "var m=new Map([['a',1],['b',2]]),s='';m.forEach(function(v,k,x){s+=k+v+(x===m)});s", "a1trueb2true" },
{ "var m=new Map([['a',1]]);var x=m.delete('x'),a=m.delete('a');m.clear();x+','+a+','+m.size", "false,true,0" },
{ "try{Map();'no'}catch(e){e.name}", "TypeError" },
{ "var s=new Set([1,2,2,NaN,NaN]);s.add(3).add(3);s.size+','+s.has(NaN)+','+(s instanceof Set)+','+Object.prototype.toString.call(s)", "4,true,true,[object Set]" },
{ "var s=new Set(['a','b']),o='';s.forEach(function(v,k,x){o+=v+k+(x===s)});o", "aatruebbtrue" },
{ "var s=new Set(['a','b']),i=s.values(),a=i.next(),b=i.next(),d=i.next();a.value+b.value+','+d.done+','+s.delete('a')+','+s.size", "ab,true,true,1" },

/* ---- bounded WeakMap and WeakSet ---- */
{ "var a={},b={},w=new WeakMap([[a,1]]);w.set(b,2);w.get(a)+','+w.get(b)+','+w.has(a)+','+(w instanceof WeakMap)+','+Object.prototype.toString.call(w)", "1,2,true,true,[object WeakMap]" },
{ "var a={},w=new WeakMap([[a,'x']]);var x=w.delete({}),y=w.delete(a);x+','+y+','+w.has(a)+','+String(w.get(a))", "false,true,false,undefined" },
{ "try{new WeakMap().set(1,2);'no'}catch(e){e.name}", "TypeError" },
{ "var a={},b={},s=new WeakSet([a]);s.add(b);s.has(a)+','+s.has(b)+','+s.delete(a)+','+(s instanceof WeakSet)+','+Object.prototype.toString.call(s)", "true,true,true,true,[object WeakSet]" },
{ "try{new WeakSet().add('x');'no'}catch(e){e.name}", "TypeError" },
{ "var w=new WeakMap(),s=new WeakSet();typeof w.keys+','+typeof w.size+','+typeof s.values+','+typeof s.clear", "undefined,undefined,undefined,undefined" },
{ "try{WeakMap();'no'}catch(e){e.name}", "TypeError" },

/* ---- ES6 let / const / block scoping / TDZ ---- */
{ "{ let x = 1; { let x = 2; } x }", "1" },
{ "let x = 10; if (true) { let x = 20; } x", "10" },
{ "const c = 42; c", "42" },
{ "const c;", "!SyntaxError" },
{ "const c = 1; c = 2", "!TypeError" },
{ "const c = 1; c++", "!TypeError" },
{ "const c = 1; c += 2", "!TypeError" },
{ "let r = ''; try { r += y; } catch (e) { r += e.name; } let y = 1; r", "ReferenceError" },
{ "(function(){ try { return t; } catch(e) { return e.name; } let t = 5; })()", "ReferenceError" },
{ "(function(){ try { return typeof t; } catch(e) { return e.name; } const t = 5; })()", "ReferenceError" },
{ "let fns = []; for (let i = 0; i < 4; i++) { fns.push(() => i); } fns.map(f => f()).join(',')", "0,1,2,3" },
{ "let fns = []; for (let i = 0, s = 10; i < 3; i++, s += 10) { fns.push(() => i + ':' + s); } fns.map(f => f()).join(',')", "0:10,1:20,2:30" },
{ "let fns = []; for (const v of [10, 20, 30]) fns.push(() => v); fns.map(f => f()).join(',')", "10,20,30" },
{ "let fns = []; const o = {a:1, b:2}; for (const k in o) fns.push(() => k); fns.map(f => f()).join(',')", "a,b" },
{ "try { throw 'ok'; } catch { 'caught' }", "caught" },
{ "var x = 1; { let x = 2; var z = x; } x + ',' + z", "1,2" },
{ "let r = 0; switch (1) { case 1: { let a = 5; r = a; break; } } r + ',' + typeof a", "5,undefined" },

/* ---- template literals & tagged templates ---- */
{ "`hello ${1 + 2} world`", "hello 3 world" },
{ "`a ${`b ${ {x:7}.x } c`} d`", "a b 7 c d" },
{ "var s = `line1\nline2\\t\\\\\\`\\$${1+1}`; s.split('\\n').join('|')", "line1|line2\t\\`$2" },
{ "function tag(s, ...v) { return s.join('|') + ':' + s.raw.join('|') + ':' + v.join(','); } tag`a\\nb${1+2}c\\`d`", "a\nb|c`d:a\\nb|c\\`d:3" },
{ "function tag(s) { return String(s[0]) + ':' + s.raw[0]; } tag`\\uZZZZ`", "undefined:\\uZZZZ" },
{ "`\\uZZZZ`", "!SyntaxError" },
{ "try { `${Symbol('a')}`; 'no' } catch(e) { e.name }", "TypeError" },
{ "String.raw`a\\nb${1+2}`", "a\\nb3" },

/* ---- destructuring, default params, rest & spread ---- */
{ "const [a, , b = 2, [c, ...d], ...rest] = [10, 99, undefined, [3, 4, 5], 6, 7]; [a, b, c, d.join('+'), rest.join('-')].join('|')", "10|2|3|4+5|6-7" },
{ "let x = 1, y = 2; [x, y] = [y, x]; x + ',' + y", "2,1" },
{ "const obj = {}; [obj.a, {b: obj.b = 9}] = [4, {}]; obj.a + ',' + obj.b", "4,9" },
{ "const k = 'dyn'; const { a, b: renamed = 5, [k]: val, ...rest } = { a: 1, dyn: 7, c: 3, d: 4 }; [a, renamed, val, Object.keys(rest).sort().join(',')].join('|')", "1|5|7|c,d" },
{ "function f(a = 10, b = a * 2, ...r) { return [a, String(b), r.join(',')].join(':'); } f() + '|' + f(undefined, null, 3, 4) + '|' + f.length", "10:20:|10:null:3,4|0" },
{ "function g(a, b = 1, c) { return a; } g.length + ',' + ((x, y = 2) => x + y).length", "1,1" },
{ "const fn = ({x, y = 2}, [head, ...tail], ...extra) => x + y + head + tail.length + extra.length; fn({x:3}, [4,5,6], 7, 8)", "13" },
{ "const a = [1, 2], b = [4, 5]; [...a, 3, ...b, ...'xy'].join(',')", "1,2,3,4,5,x,y" },
{ "Math.max(0, ...[3, 9, 2], 5)", "9" },
{ "function Pair(a, b, c) { this.s = a + '-' + b + '-' + c; } new Pair(...[1, 2], 3).s", "1-2-3" },
{ "const o1 = {a: 1, b: 2}, o2 = {b: 20, c: 30}; const m = {...o1, ...o2, c: 300}; m.a + ',' + m.b + ',' + m.c", "1,20,300" },
{ "((a, b, c) => a + b + c)(...new Set([10, 20, 30]))", "60" },
{ "for (const [k, v] of [['a', 1], ['b', 2]]) { if (k === 'b') { var found = v; } } found", "2" },

/* ---- for..of & iterable protocols ---- */
{ "let out = ''; outer: for (const a of [1, 2]) { for (const b of [10, 20, 30]) { if (b === 20) continue; if (a === 2 && b === 30) break outer; out += a + ':' + b + ','; } } out", "1:10,1:30,2:10," },
{ "[...'a\\u03B1b'].join(',')", "a,\u03B1,b" },
{ "let s = 0; for (const b of new Uint8Array([7, 8, 9])) s += b; s", "24" },
{ "let out = []; for (const [k, v] of new Map([['x', 1], ['y', 2]])) out.push(k + '=' + v); out.join(',')", "x=1,y=2" },
{ "let out = []; for (const v of new Set(['p', 'q', 'p'])) out.push(v); out.join(',')", "p,q" },
{ "let out = []; for (const [k, v] of new URLSearchParams('a=1&b=two&a=3')) out.push(k + ':' + v); out.join(',')", "a:1,b:two,a:3" },
{ "(function(){ let s = ''; for (const x of arguments) s += x; return s; })('x', 'y', 'z')", "xyz" },
{ "let s = ''; for (const x of {0: 'u', 1: 'v', length: 2}) s += x; s", "uv" },
{ "const range = { [Symbol.iterator]() { let i = 0; return { next() { return i < 3 ? { value: i++, done: false } : { done: true }; } }; } }; [...range].join(',')", "0,1,2" },
{ "[...[10, 20].entries()].map(([i, v]) => i + ':' + v).join(',')", "0:10,1:20" },
{ "try { for (const x of 123) {} 'no' } catch(e) { e.name }", "TypeError" },

/* ---- modern operators: ?., ??, ??=, &&=, ||=, **, **= ---- */
{ "let c = 0; const x = null; [x?.a, x?.(), x?.[c++], x?.a.b[c++], c].map(String).join(',')", "undefined,undefined,undefined,undefined,0" },
{ "try { (null?.a).b; 'no' } catch(e) { e.name }", "TypeError" },
{ "const o = { v: 42, f() { return this.v; } }; [o?.f(), o.f?.(), o.missing?.()].map(String).join(',')", "42,42,undefined" },
{ "const o = { a: 1 }; (delete o?.a) + ',' + ('a' in o) + ',' + (delete (null)?.a)", "true,false,true" },
{ "(true?.5:1) + ',' + (false?.5:1)", "0.5,1" },
{ "[null ?? 'a', undefined ?? 'b', 0 ?? 'c', '' ?? 'd', false ?? 'e'].join('|')", "a|b|0||false" },
{ "1 ?? 2 || 3", "!SyntaxError" },
{ "1 && 2 ?? 3", "!SyntaxError" },
{ "(1 ?? 2) || 3", "1" },
{ "let sets = 0; const o = { get a(){ return 1; }, set a(v){ sets++; }, get b(){ return null; }, set b(v){ sets++; } }; o.a ||= 9; o.a ?\?= 9; o.b &&= 9; let x; x ?\?= 5; let y = 2; y &&= 7; let z = 0; z ||= 4; [sets, x, y, z].join(',')", "0,5,7,4" },
{ "(2 ** 3 ** 2) + ',' + ((-2) ** 2) + ',' + (-(2 ** 2)) + ',' + (2 ** -1)", "512,4,-4,0.5" },
{ "-2 ** 2", "!SyntaxError" },
{ "let x = 3; x **= 4; x", "81" },

/* ---- classes / extends / super / static / get / set ---- */
{ "class Animal { constructor(n) { this.n = n; } speak() { return this.n + '!'; } static kind() { return 'animal'; } } class Dog extends Animal { constructor(n, b) { super(n); this.b = b; } speak() { return super.speak() + ':' + this.b; } static kind() { return super.kind() + '>dog'; } get desc() { return this.n + '/' + this.b; } set desc(v) { this.b = v; } } const d = new Dog('Rex', 'Lab'); d.desc = 'Pug'; [d.speak(), Dog.kind(), d.desc, d instanceof Animal, d instanceof Dog, Object.getPrototypeOf(Dog) === Animal, Object.getPrototypeOf(Dog.prototype) === Animal.prototype, Object.keys(d).sort().join(',')].join('|')", "Rex!:Pug|animal>dog|Rex/Pug|true|true|true|true|b,n" },
{ "class Bad extends Object { constructor() {} } try { new Bad(); 'no' } catch(e) { e.name }", "ReferenceError" },
{ "class Bad2 extends Object { constructor() { this.x = 1; } } try { new Bad2(); 'no' } catch(e) { e.name }", "ReferenceError" },
{ "class C {} try { C(); 'no' } catch(e) { e.name }", "TypeError" },
{ "const C = class Named { who() { return typeof Named; } }; new C().who() + ',' + typeof Named", "function,undefined" },
{ "try { new Late(); 'no' } catch(e) { e.name }; class Late {}", "ReferenceError" },
{ "const s = Symbol('m'); class X { [s]() { return 7; } } new X()[s]()", "7" },

/* ---- ES6+ built-ins: Symbol, Map/Set, Object, Array, String, Math ---- */
{ "const s1 = Symbol('a'), s2 = Symbol('a'); [typeof s1, s1 === s2, s1.description, Symbol().description, s1.toString(), String(s1)].map(String).join('|')", "symbol|false|a|undefined|Symbol(a)|Symbol(a)" },
{ "Symbol('x')", "Symbol(x)" },
{ "try { '' + Symbol('a'); 'no' } catch(e) { e.name }", "TypeError" },
{ "try { new Symbol(); 'no' } catch(e) { e.name }", "TypeError" },
{ "const g1 = Symbol.for('k'), g2 = Symbol.for('k'); [g1 === g2, Symbol.keyFor(g1), Symbol.keyFor(Symbol('k'))].map(String).join(',')", "true,k,undefined" },
{ "const sym = Symbol('hidden'); const o = { [sym]: 42, a: 1 }; const c = Object.assign({}, o); [c[sym], c.a, Object.keys(o).join(','), Object.getOwnPropertySymbols(o).length, JSON.stringify(o), JSON.stringify([sym])].join('|')", "42|1|a|1|{\"a\":1}|[null]" },
{ "const custom = { [Symbol.toStringTag]: 'Custom' }; Object.prototype.toString.call(custom)", "[object Custom]" },
{ "const s = Symbol('mapk'); const m = new Map([[s, 9]]); const st = new Set('abca'); [m.get(s), m.has(s), st.size, [...st].join('')].join(',')", "9,true,3,abc" },
{ "const u = new URLSearchParams('x=1&y=2'); const m = new Map(u); const o = Object.fromEntries(u); [m.get('y'), o.x, o.y].join(',')", "2,1,2" },
{ "Object.is(+0, -0) + ',' + Object.is(NaN, NaN) + ',' + Object.is(0, 0)", "false,true,true" },
{ "const a = { p: 1 }, b = {}; Object.setPrototypeOf(b, a); let cyc = ''; try { Object.setPrototypeOf(a, b); } catch(e) { cyc = e.name; } b.p + ',' + cyc", "1,TypeError" },
{ "Array.from(new Set([1, 2, 3]), x => x * 10).join(',') + '|' + Array.from('hi').join(',')", "10,20,30|h,i" },
{ "[10, 20, 30].at(-1) + ',' + String([1].at(-2)) + ',' + 'abc'.at(-1) + ',' + String('a'.at(5))", "30,undefined,c,undefined" },
{ "[1, 2, 3, 4].fill(9, 1, 3).join(',') + '|' + [1, 2, 3, 4, 5].copyWithin(0, 3).join(',') + '|' + [1, 2, 3, 4, 5].copyWithin(2, 0, 3).join(',')", "1,9,9,4|4,5,3,4,5|1,2,1,2,3" },
{ "[Math.trunc(-3.9), Object.is(Math.sign(-0), -0), Math.sign(-42), Math.hypot(3, 4), Math.hypot(Infinity, NaN), Math.imul(0xffffffff, 5)].join(',')", "-3,true,-1,5,Infinity,-5" },
{ "Array.prototype[Symbol.iterator] === Array.prototype.values && Map.prototype[Symbol.iterator] === Map.prototype.entries && Set.prototype[Symbol.iterator] === Set.prototype.values && Set.prototype.keys === Set.prototype.values", "true" },
{ "class Base { constructor(a, b) { this.sum = a + b; } greet() { return 'hi'; } } class Mid extends Base { greet() { return super.greet() + '!'; } } class Sub extends Mid {} const s = new Sub(3, 4); [s.sum, s.greet(), s instanceof Base, s instanceof Mid, s instanceof Sub].join(',')", "7,hi!,true,true,true" },
{ "function f(){ function g(){ return x; } let e = ''; try { g(); } catch(err) { e = err.name; } const x = 7; return e + ',' + g(); } f()", "ReferenceError,7" },
{ "const i = 99; const fns = []; for (let {a, b} of [{a:1,b:2},{a:3,b:4}]) fns.push(() => a + ':' + b); i + '|' + fns.map(f => f()).join(',')", "99|1:2,3:4" },
{ "const o = {}; let a; [a, ...o.rest] = [1, 2, 3, 4]; a + ':' + o.rest.join('-')", "1:2-3-4" },
{ "const k = 'b'; const { [k]: bv, a, ...rest } = { a: 1, b: 2, c: 3 }; [a, bv, Object.keys(rest).join(',')].join('|')", "1|2|c" },
{ "const u = {...null, ...undefined, a: 1, ...{b: 2}}; u.a + ',' + u.b", "1,2" },
{ "`a\r\nb`.length + ':' + `a\r\nb`.replace('\\n', '|')", "3:a|b" },

/* ---- Wave 7: Full TypedArray family & ArrayBuffer ---- */
{ "[Int8Array.BYTES_PER_ELEMENT, Uint8Array.BYTES_PER_ELEMENT, Uint8ClampedArray.BYTES_PER_ELEMENT, Int16Array.BYTES_PER_ELEMENT, Uint16Array.BYTES_PER_ELEMENT, Int32Array.BYTES_PER_ELEMENT, Uint32Array.BYTES_PER_ELEMENT, Float32Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT].join(',')", "1,1,1,2,2,4,4,4,8" },
{ "[Int8Array.prototype.BYTES_PER_ELEMENT, Int16Array.prototype.BYTES_PER_ELEMENT, Int32Array.prototype.BYTES_PER_ELEMENT, Float64Array.prototype.BYTES_PER_ELEMENT].join(',')", "1,2,4,8" },
{ "Array.from(new Int8Array([127, 128, -128, -129, 255])).join(',')", "127,-128,-128,127,-1" },
{ "Array.from(new Uint8Array([0, 255, 256, -1, 1.9, NaN])).join(',')", "0,255,0,255,1,0" },
{ "Array.from(new Uint8ClampedArray([-5, 0, 0.5, 1.5, 2.5, 3.5, 254.6, 255, 300, NaN, Infinity, -Infinity])).join(',')", "0,0,0,2,2,4,255,255,255,0,255,0" },
{ "Array.from(new Int16Array([32767, 32768, -32768, -32769])).join(',')", "32767,-32768,-32768,32767" },
{ "Array.from(new Uint16Array([0, 65535, 65536, -1])).join(',')", "0,65535,0,65535" },
{ "Array.from(new Int32Array([2147483647, 2147483648, -2147483648, -2147483649])).join(',')", "2147483647,-2147483648,-2147483648,2147483647" },
{ "Array.from(new Uint32Array([0, 4294967295, 4294967296, -1])).join(',')", "0,4294967295,0,4294967295" },
{ "const f = new Float32Array([1.5, 0.1, -0, NaN, Infinity]); [f[0], f[1] !== 0.1, Math.abs(f[1] - 0.1) < 1e-7, Object.is(f[2], -0), Number.isNaN(f[3]), f[4] === Infinity].join(',')", "1.5,true,true,true,true,true" },
{ "const f = new Float64Array([0.1, Math.PI, -1e-200, 9007199254740991]); [f[0] === 0.1, f[1] === Math.PI, f[2] === -1e-200, f[3] === 9007199254740991].join(',')", "true,true,true,true" },
{ "const b = new ArrayBuffer(16); const v = new Int32Array(b, 4, 2); v[0] = -123456; v[1] = 789; [v.buffer === b, v.byteOffset, v.byteLength, v.length, v[0], v[1]].join(',')", "true,4,8,2,-123456,789" },
{ "const b = new ArrayBuffer(4); const u16 = new Uint16Array(b); const u8 = new Uint8Array(b); u16[0] = 0x1234; u16[1] = 0xABCD; [u8[0].toString(16), u8[1].toString(16), u8[2].toString(16), u8[3].toString(16)].join(',')", "34,12,cd,ab" },
{ "new Int16Array(new Set([10, 20, 30])).toString() + '|' + new Uint16Array({0: 7, 1: 8, length: 2}).toString()", "10,20,30|7,8" },
{ "const src = new Float32Array([1.9, -2.7, 40000]); const dst = new Int16Array(src); [dst.buffer !== src.buffer, dst[0], dst[1], dst[2]].join(',')", "true,1,-2,-25536" },
{ "new Int16Array(new ArrayBuffer(4), 1)", "!RangeError" },
{ "new Int16Array(new ArrayBuffer(5))", "!RangeError" },
{ "new Int32Array(new ArrayBuffer(8), 4, 2)", "!RangeError" },
{ "new Int16Array(-1)", "!RangeError" },
{ "Int16Array(4)", "!TypeError" },
{ "const a = new Int16Array([10, 20]); a[5] = 99; a[-1] = 88; [String(a[5]), String(a[-1]), '5' in a, '-1' in a, a.length].join(',')", "undefined,undefined,false,false,2" },
{ "const a = new Int16Array(5); a.set([10, 20, 30], 1); a.set(a.subarray(1, 4), 2); a.toString()", "0,10,10,20,30" },
{ "const b = new ArrayBuffer(8); const u16 = new Uint16Array(b); const u8 = new Uint8Array(b); u8[0]=1; u8[1]=0; u8[2]=2; u8[3]=0; u16.set(u8.subarray(0, 4), 0); u16.toString()", "1,0,2,0" },
{ "new Int16Array(2).set([1, 2, 3])", "!RangeError" },
{ "const a = new Int32Array([10, 20, 30, 40]); const sub = a.subarray(1, -1); const sl = a.slice(-3, -1); sub[0] = 99; sl[0] = 77; [a.toString(), sub.buffer === a.buffer, sl.buffer !== a.buffer, sl.toString()].join('|')", "10,99,30,40|true|true|77,30" },
{ "const a = new Uint8ClampedArray([1, 2, 3, 4]); a.fill(500, 1, -1); a.toString() + '|' + new Int16Array(4).fill(-7, 1, 3).toString()", "1,255,255,4|0,-7,-7,0" },
{ "const a = new Int32Array([10, 20, 30]); [a.at(0), a.at(-1), a.at(-3), String(a.at(3)), String(a.at(-4))].join(',')", "10,30,10,undefined,undefined" },
{ "let s = 0; const a = new Uint16Array([2, 4, 6]); a.forEach((v, i, arr) => { s += v * (i + 1) + (arr === a ? 0 : 100); }); s", "28" },
{ "const a = new Int16Array([10, 20, 30]); const m = a.map((x, i) => x * 2 + i + 32760); [m instanceof Int16Array, m.buffer !== a.buffer, m.toString()].join('|')", "true|true|-32756,-32735,-32714" },
{ "const a = new Float64Array([1.5, -0, 2.5, NaN, 2.5]); [a.indexOf(2.5), a.indexOf(2.5, 3), a.indexOf(2.5, -2), a.indexOf(0), a.indexOf(NaN), a.indexOf('2.5')].join(',')", "2,4,4,1,-1,-1" },
{ "const sc = { m: 3 }; const a = Int16Array.from([1, 2, 3], function(x, i){ return (x + i) * this.m; }, sc); const b = Float32Array.of(1.5, -2.5, 4); [a instanceof Int16Array, a.toString(), b instanceof Float32Array, b.toString()].join('|')", "true|3,9,15|true|1.5,-2.5,4" },
{ "const a = new Uint32Array([9, 8]); let ks = ''; for (const k in a) ks += k; [Object.keys(a).join(','), ks, [...a].join(','), ({...a})[1], Object.prototype.toString.call(a), Object.getPrototypeOf(Int8Array) === Object.getPrototypeOf(Float64Array)].join('|')", "0,1|01|9,8|8|[object Uint32Array]|true" },
{ "const ab = new ArrayBuffer(8); [ArrayBuffer.isView(new Int8Array(ab)), ArrayBuffer.isView(new Uint8ClampedArray(ab)), ArrayBuffer.isView(new Int16Array(ab)), ArrayBuffer.isView(new Uint16Array(ab)), ArrayBuffer.isView(new Int32Array(ab)), ArrayBuffer.isView(new Uint32Array(ab)), ArrayBuffer.isView(new Float32Array(ab)), ArrayBuffer.isView(new Float64Array(ab)), ArrayBuffer.isView(new DataView(ab)), ArrayBuffer.isView(ab), ArrayBuffer.isView([1]), ArrayBuffer.isView({})].join(',')", "true,true,true,true,true,true,true,true,true,false,false,false" },
{ "const u = new Uint8Array([10, 20, 30, 40, 50]); const s1 = new Uint8Array(u.buffer.slice(1, 4)); const s2 = new Uint8Array(u.buffer.slice(2, undefined)); const s3 = new Uint8Array(u.buffer.slice(-2)); s1[0] = 99; [u.toString(), s1.toString(), s2.toString(), s3.toString()].join('|')", "10,20,30,40,50|99,30,40|30,40,50|40,50" },
{ "new Uint16Array([10, 2, 1, 100]).sort().toString() + '|' + new Float64Array([NaN, 0, -0, -2, 1]).toSorted().map(x => Object.is(x, -0) ? -999 : x).toString()", "1,2,10,100|-2,-999,0,1,NaN" },

/* ---- Wave 7: DataView ---- */
{ "const ab = new ArrayBuffer(16); const dv = new DataView(ab, 4, 8); [dv.buffer === ab, dv.byteOffset, dv.byteLength, Object.prototype.toString.call(dv)].join(',')", "true,4,8,[object DataView]" },
{ "DataView(new ArrayBuffer(4))", "!TypeError" },
{ "new DataView({})", "!TypeError" },
{ "new DataView(new ArrayBuffer(4), -1)", "!RangeError" },
{ "new DataView(new ArrayBuffer(4), 5)", "!RangeError" },
{ "new DataView(new ArrayBuffer(4), 2, 3)", "!RangeError" },
{ "const dv = new DataView(new ArrayBuffer(8)); dv.setUint32(0, 0x12345678); [dv.getUint8(0).toString(16), dv.getUint8(3).toString(16), dv.getUint16(0).toString(16), dv.getUint16(0, true).toString(16), dv.getUint32(0).toString(16), dv.getUint32(0, true).toString(16)].join(',')", "12,78,1234,3412,12345678,78563412" },
{ "const dv = new DataView(new ArrayBuffer(8)); dv.setInt8(0, -5); dv.setInt16(1, -300, true); dv.setInt32(3, -100000, false); [dv.getInt8(0), dv.getUint8(0), dv.getInt16(1, true), dv.getUint16(1, true), dv.getInt32(3, false), dv.getUint32(3, false)].join(',')", "-5,251,-300,65236,-100000,4294867296" },
{ "const dv = new DataView(new ArrayBuffer(12)); dv.setFloat32(1, 1.5, false); dv.setFloat64(4, 1.5, true); [dv.getUint32(1, false).toString(16), dv.getFloat32(1, false), dv.getUint32(8, true).toString(16), dv.getFloat64(4, true)].join(',')", "3fc00000,1.5,3ff80000,1.5" },
{ "const dv = new DataView(new ArrayBuffer(10)); dv.setFloat64(1, -Math.PI, false); dv.getFloat64(1, false) === -Math.PI", "true" },
{ "const dv = new DataView(new ArrayBuffer(12), 2, 4); const errs = []; ['dv.getUint8(-1)', 'dv.getUint8(4)', 'dv.getInt16(3)', 'dv.getInt32(1)', 'dv.getFloat64(0)', 'dv.setInt16(3, 1)', 'dv.setFloat32(1, 1)'].forEach(s => { try { if(s==='dv.getUint8(-1)')dv.getUint8(-1); else if(s==='dv.getUint8(4)')dv.getUint8(4); else if(s==='dv.getInt16(3)')dv.getInt16(3); else if(s==='dv.getInt32(1)')dv.getInt32(1); else if(s==='dv.getFloat64(0)')dv.getFloat64(0); else if(s==='dv.setInt16(3, 1)')dv.setInt16(3,1); else dv.setFloat32(1,1); errs.push('no'); } catch(e) { errs.push(e.name); } }); errs.every(x => x === 'RangeError') && errs.length === 7", "true" },

/* ---- Wave 7: WHATWG URL & URL.canParse ---- */
{ "const u = new URL('HTTP://User:Pass@EXAMPLE.COM:80/a/b/../c?x=1#frag'); [u.href, u.origin, u.protocol, u.username, u.password, u.host, u.hostname, u.port, u.pathname, u.search, u.hash].join('|')", "http://User:Pass@example.com/a/c?x=1#frag|http://example.com|http:|User|Pass|example.com|example.com||/a/c|?x=1|#frag" },
{ "const u = new URL('../api?q=hello%20world#part', 'https://user:pw@example.test:8443/a/b'); [u.origin, u.host, u.hostname, u.port, u.pathname, u.search, u.hash, u.searchParams.get('q'), u.toString() === u.toJSON(), JSON.stringify(u)].join('|')", "https://example.test:8443|example.test:8443|example.test|8443|/api|?q=hello%20world|#part|hello world|true|\"https://user:pw@example.test:8443/api?q=hello%20world#part\"" },
{ "const u = new URL('https://example.com/a/b/c'); [new URL('?q=1', u).href, new URL('#h', u).href, new URL('', u).href, new URL('/root', u).href, new URL('//cdn.test/lib', u).href, new URL('https:rel', u).href].join('|')", "https://example.com/a/b/c?q=1|https://example.com/a/b/c#h|https://example.com/a/b/c|https://example.com/root|https://cdn.test/lib|https://example.com/a/b/rel" },
{ "const u = new URL(' \\thttps:\\\\\\\\EXAMPLE.COM\\\\a\\t\\n\\\\b\\\\../c\\\\ '); u.href", "https://example.com/a/c/" },
{ "const u = new URL('http://[2001:DB8::1]:8080/x/%2e%2e/y'); [u.origin, u.host, u.hostname, u.port, u.pathname].join('|')", "http://[2001:db8::1]:8080|[2001:db8::1]:8080|[2001:db8::1]|8080|/y" },
{ "const u = new URL('https://example.com/p?a=1'); const sp = u.searchParams; sp.append('b', 'x y'); const h1 = u.href; u.search = '?k=9&k=8'; const same = (sp === u.searchParams); sp.delete('k', '9'); [h1, same, u.href].join('|')", "https://example.com/p?a=1&b=x+y|true|https://example.com/p?k=8" },
{ "const u = new URL('https://example.com:8443/old'); u.protocol = 'http:'; u.username = 'u 1'; u.password = 'p@2'; u.hostname = 'next.test'; u.port = '80'; u.pathname = 'v 1/./sub'; u.hash = 'top'; const h1 = u.href; u.href = 'https://final.test:0/done'; [h1, u.href, u.port].join('|')", "http://u%201:p%402@next.test/v%201/sub#top|https://final.test:0/done|0" },
{ "[new URL('mailto:user@example.com').origin, new URL('file:///tmp/a.txt').origin, new URL('blob:https://example.com:8443/uuid').origin, new URL('blob:https://example.com:8443/uuid').pathname].join('|')", "null|null|https://example.com:8443|https://example.com:8443/uuid" },
{ "[URL.canParse('https://example.com/'), URL.canParse('/rel', 'https://example.com/a'), URL.canParse('/rel'), URL.canParse('https://example.com', 'bad-base'), URL.canParse('http://a.com:99999')].join(',')", "true,true,false,false,false" },
{ "URL.canParse()", "!TypeError" },
{ "URL('https://example.com/')", "!TypeError" },
{ "new URL('/rel')", "!TypeError" },
{ "new URL('https://example.com', 'not-a-url')", "!TypeError" },
{ "new URL('https://:80/')", "!TypeError" },
{ "const u = new URL('https://a.com/'); u.href = 'invalid'", "!TypeError" },

/* ---- Wave 7: ES2022 / ES2023 builtins ---- */
{ "const o = Object.create({ inh: 1 }); o.own = 2; const s = Symbol('k'); o[s] = 3; const np = Object.create(null); np.x = 4; [Object.hasOwn(o, 'own'), Object.hasOwn(o, 'inh'), Object.hasOwn(o, s), Object.hasOwn(np, 'x'), Object.hasOwn([10], 0), Object.hasOwn([10], 'length'), Object.hasOwn(new Int16Array([5]), 0), Object.hasOwn('hi', 1)].join(',')", "true,false,true,true,true,true,true,true" },
{ "Object.hasOwn(null, 'a')", "!TypeError" },
{ "Object.hasOwn(undefined, 'a')", "!TypeError" },
{ "const a = [10, 20, 30]; [a.at(NaN), a.at(undefined), a.at(1.9), a.at(-1.9), a.at(-2.9), String(a.at(Infinity)), String(a.at(-Infinity)), 'xyz'.at(-2), String(''.at(0))].join(',')", "10,10,20,30,20,undefined,undefined,y,undefined" },
{ "const a = [5, 12, 8, 130, 44]; const sc = { min: 10 }; [a.findLast(function(x, i, arr){ return x > this.min && i < arr.length - 1; }, sc), a.findLastIndex(function(x){ return x > this.min; }, sc), a.findLast(x => x > 999), a.findLastIndex(x => x > 999)].map(String).join(',')", "130,4,undefined,-1" },
{ "const a = [1, 2, 3]; const r = a.toReversed(); [a.join(','), r.join(','), r !== a].join('|')", "1,2,3|3,2,1|true" },
{ "const a = [10, 2, undefined, 1]; const s1 = a.toSorted(); const s2 = a.toSorted((x, y) => x - y); [a.join(','), s1.map(String).join(','), s2.map(String).join(',')].join('|')", "10,2,,1|1,10,2,undefined|1,2,10,undefined" },
{ "[1, 2].toSorted(123)", "!TypeError" },
{ "const a = [1, 2, 3, 4]; const s1 = a.toSpliced(1, 2, 'a', 'b', 'c'); const s2 = a.toSpliced(-1, 1); const s3 = a.toSpliced(2); const s4 = a.toSpliced(); [a.join(','), s1.join(','), s2.join(','), s3.join(','), s4.join(','), s4 !== a].join('|')", "1,2,3,4|1,a,b,c,4|1,2,3|1,2|1,2,3,4|true" },
{ "const a = [10, 20, 30]; const w1 = a.with(1, 99); const w2 = a.with(-1, 77); const w3 = a.with(NaN, 5); [a.join(','), w1.join(','), w2.join(','), w3.join(',')].join('|')", "10,20,30|10,99,30|10,20,77|5,20,30" },
{ "[1, 2].with(2, 9)", "!RangeError" },
{ "[1, 2].with(-3, 9)", "!RangeError" },
{ "[1, 2].with(Infinity, 9)", "!RangeError" },
{ "const re = /a/g; re.lastIndex = 5; ['ab'.replaceAll('', '-'), ''.replaceAll('', 'x'), 'ab'.replaceAll(/(?:)/g, '|'), 'a$b$'.replaceAll('$', '$$'), 'aaa'.replaceAll(re, 'z'), re.lastIndex].join(',')", "-a-b-,x,|a|b|,a$b$,zzz,0" },
{ "'abc'.replaceAll(/a/, 'x')", "!TypeError" },

/* ---- Wave 7: structuredClone ---- */
{ "[structuredClone(42), structuredClone('hi'), structuredClone(true), structuredClone(null) === null, String(structuredClone(undefined)), Number.isNaN(structuredClone(NaN)), Object.is(structuredClone(-0), -0)].join(',')", "42,hi,true,true,undefined,true,true" },
{ "const orig = { a: 1, b: [2, { c: 3 }] }; orig.b.extra = 'x'; const cl = structuredClone(orig); cl.b[1].c = 99; cl.b.push(4); [orig.b[1].c, orig.b.length, cl.b[1].c, cl.b.extra].join(',')", "3,2,99,x" },
{ "const a = { name: 'A' }; const b = { name: 'B', peer: a }; a.peer = b; a.self = a; const c = structuredClone(a); [c !== a, c.self === c, c.peer !== b, c.peer.peer === c, c.peer.name].join(',')", "true,true,true,true,B" },
{ "const arr = [1]; arr.push(arr); const c = structuredClone(arr); [c !== arr, c[0], c[1] === c, Array.isArray(c)].join(',')", "true,1,true,true" },
{ "const shared = { x: 7 }; const c = structuredClone([shared, shared]); [c[0] === c[1], c[0] !== shared, c[0].x].join(',')", "true,true,7" },
{ "const k = { id: 1 }; const m = new Map([[k, { v: 2 }], [NaN, 'n']]); m.set('self', m); const s = new Set([k, 'z']); s.add(s); const cm = structuredClone(m); const cs = structuredClone(s); [cm !== m, cm.get('self') === cm, cm.get(NaN), cm.size, cs !== s, cs.has(s) === false, cs.has(cs), cs.size].join(',')", "true,true,n,3,true,true,true,3" },
{ "const d = new Date(123456789); const cd = structuredClone(d); cd.setTime(0); const inv = structuredClone(new Date(NaN)); [d.getTime(), cd.getTime(), cd instanceof Date, Number.isNaN(inv.getTime())].join(',')", "123456789,0,true,true" },
{ "const re = /ab+c/gi; re.lastIndex = 4; const cre = structuredClone(re); [cre !== re, cre instanceof RegExp, cre.source, cre.flags, cre.lastIndex, cre.test('XXABBC')].join(',')", "true,true,ab+c,gi,0,true" },
{ "const ab = new ArrayBuffer(8); const u16 = new Uint16Array(ab); const u8 = new Uint8Array(ab); u16[0] = 0x1234; u16[1] = 0x5678; const cl = structuredClone({ a: u16, b: u8, buf: ab }); cl.a[0] = 0x9999; [u16[0].toString(16), cl.a.buffer === cl.b.buffer, cl.a.buffer === cl.buf, cl.buf !== ab, cl.b[0].toString(16), cl.b[1].toString(16)].join(',')", "1234,true,true,true,99,99" },
{ "const dv = new DataView(new ArrayBuffer(8), 2, 4); dv.setInt32(0, -42); const cdv = structuredClone(dv); cdv.setInt32(0, 99); [dv.getInt32(0), cdv.getInt32(0), cdv instanceof DataView, cdv.byteOffset, cdv.byteLength].join(',')", "-42,99,true,2,4" },
{ "structuredClone()", "!TypeError" },
{ "structuredClone(() => 1)", "!DataCloneError" },
{ "structuredClone(Symbol('s'))", "!DataCloneError" },
{ "structuredClone({ fn: function(){} })", "!DataCloneError" },

/* ---- Wave 8B: Web Crypto, AbortController/Signal, Blob, FormData ---- */
{ "const u = crypto.randomUUID(); const u2 = crypto.randomUUID(); [/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u), u !== u2, Object.prototype.toString.call(crypto), Object.prototype.toString.call(crypto.subtle)].join(',')", "true,true,[object Crypto],[object SubtleCrypto]" },
{ "const a1 = new Uint8Array(4); const a2 = new Uint8ClampedArray(4); const a3 = new Int16Array(2); const a4 = new Uint32Array(2); const a5 = new Uint8Array(0); [crypto.getRandomValues(a1) === a1, crypto.getRandomValues(a2) === a2, crypto.getRandomValues(a3) === a3, crypto.getRandomValues(a4) === a4, crypto.getRandomValues(a5).length].join(',')", "true,true,true,true,0" },
{ "crypto.getRandomValues(new Float32Array(2))", "!TypeError" },
{ "crypto.getRandomValues(new Float64Array(2))", "!TypeError" },
{ "crypto.getRandomValues(new DataView(new ArrayBuffer(4)))", "!TypeError" },
{ "crypto.getRandomValues(new ArrayBuffer(4))", "!TypeError" },
{ "crypto.getRandomValues({})", "!TypeError" },
{ "crypto.getRandomValues()", "!TypeError" },
{ "crypto.getRandomValues(new Uint8Array(65537))", "!RangeError" },
{ "crypto.getRandomValues(new Uint32Array(16385))", "!RangeError" },
{ "const ac = new AbortController(); const log = []; ac.signal.onabort = e => log.push('on:' + e.type + ':' + (e.target === ac.signal)); const fn = e => log.push('ev:' + ac.signal.reason); ac.signal.addEventListener('abort', fn); ac.abort('halt'); ac.abort('ignored'); [ac.signal instanceof AbortSignal, ac.signal.aborted, ac.signal.reason, log.join(',')].join('|')", "true|true|halt|on:abort:true,ev:halt" },
{ "const ac = new AbortController(); ac.abort(); [ac.signal.aborted, ac.signal.reason instanceof Error, ac.signal.reason.name].join(',')", "true,true,AbortError" },
{ "const ac = new AbortController(); ac.abort(); ac.signal.throwIfAborted()", "!AbortError" },
{ "const ac = new AbortController(); ac.abort('my-why'); try { ac.signal.throwIfAborted(); 'ok'; } catch (e) { e; }", "my-why" },
{ "const outer = new AbortController(); const ac = new AbortController(); let hits = 0; const fn = () => hits++; ac.signal.addEventListener('abort', () => hits++, { signal: outer.signal }); ac.signal.addEventListener('abort', fn); ac.signal.removeEventListener('abort', fn); outer.abort(); ac.abort(); hits", "0" },
{ "const s = AbortSignal.abort('done'); [s instanceof AbortSignal, s.aborted, s.reason, AbortSignal.abort().reason.name].join(',')", "true,true,done,AbortError" },
{ "const c1 = new AbortController(); const c2 = new AbortController(); const comb = AbortSignal.any([c1.signal, c2.signal]); const pre = AbortSignal.any([c1.signal, AbortSignal.abort('early')]); let hits = 0; comb.addEventListener('abort', () => hits++); c2.abort('second'); c1.abort('first'); [comb.aborted, comb.reason, hits, pre.aborted, pre.reason].join(',')", "true,second,1,true,early" },
{ "AbortSignal()", "!TypeError" },
{ "new AbortSignal()", "!TypeError" },
{ "AbortController()", "!TypeError" },
{ "const dv = new DataView(new Uint8Array([97, 98, 99, 100]).buffer, 1, 2); const b1 = new Blob(['hi ', new Int16Array([0x2121]), dv], { type: 'TEXT/PLAIN;Charset=Utf-8' }); const b2 = b1.slice(1, -1, 'Application/JSON'); const b3 = new Blob(); [b1 instanceof Blob, b1.size, b1.type, b2.size, b2.type, b3.size, b3.type, b1.slice(0, 1, 'bad\\ntype').type].join('|')", "true|7|text/plain;charset=utf-8|5|application/json|0||" },
{ "Blob()", "!TypeError" },
{ "new Blob(123)", "!TypeError" },
{ "const fd = new FormData(); fd.append('a', '1'); fd.append('a', '2'); const bl = new Blob(['xy'], { type: 'text/plain' }); fd.append('f', bl, 'doc.txt'); fd.set('a', '3'); const ks = []; const vs = []; for (const [k, v] of fd) { ks.push(k); vs.push(v === bl ? 'BLOB' : v); } let cbLog = ''; fd.forEach(function(v, k, p){ cbLog += this.tag + k + '=' + (v === bl ? 'B' : v) + ';'; }, { tag: '@' }); [fd instanceof FormData, fd.has('a'), fd.get('a'), fd.getAll('a').join(','), fd.get('missing') === null, ks.join(','), vs.join(','), cbLog].join('|')", "true|true|3|3|true|f,a|BLOB,3|@f=B;@a=3;" },
{ "FormData()", "!TypeError" },

{ 0, 0 }
};

/* ------------------------------------------------------------------ */

static void render(js_ctx *ctx, int rc, js_value v, char *out, unsigned long n)
{
    js_value s;

    if (js_fatal(ctx)) {
        snprintf(out, n, "!Fatal");
        return;
    }
    if (rc != JS_OK) {
        js_value name;
        if (v.type == JS_OBJECT && js_get(ctx, v, "name", &name) == JS_OK &&
            name.type == JS_STRING) {
            snprintf(out, n, "!%s", js_string_bytes(name, 0));
            return;
        }
        if (js_to_string(ctx, v, &s) == JS_OK)
            snprintf(out, n, "!%s", js_string_bytes(s, 0));
        else
            snprintf(out, n, "!?");
        return;
    }
    if (js_is_symbol(v)) {
        js_value str_fn;
        if (js_get(ctx, js_global(ctx), "String", &str_fn) == JS_OK &&
            js_call(ctx, str_fn, js_undefined(), 1, &v, &s) == JS_OK &&
            s.type == JS_STRING) {
            snprintf(out, n, "%s", js_string_bytes(s, 0));
            return;
        }
    }
    if (js_to_string(ctx, v, &s) != JS_OK) {
        snprintf(out, n, "<tostring failed>");
        return;
    }
    snprintf(out, n, "%s", js_string_bytes(s, 0));
}

static void run_one(const char *src, char *out, unsigned long n)
{
    js_config cfg;
    js_ctx *ctx;
    js_value r;
    int rc;

    js_config_default(&cfg);
    cfg.max_steps = 2000000;
    ctx = js_new(&cfg);
    if (!ctx) { snprintf(out, n, "<no context>"); return; }
    rc = js_eval(ctx, src, "test", &r);
    render(ctx, rc, r, out, n);
    js_free(ctx);
}

static void escape_js(const char *s, char *out, unsigned long n)
{
    unsigned long i = 0;

    for (; *s && i + 8 < n; s++) {
        switch (*s) {
        case '\\': out[i++] = '\\'; out[i++] = '\\'; break;
        case '"':  out[i++] = '\\'; out[i++] = '"';  break;
        case '\n': out[i++] = '\\'; out[i++] = 'n';  break;
        case '\r': out[i++] = '\\'; out[i++] = 'r';  break;
        case '\t': out[i++] = '\\'; out[i++] = 't';  break;
        default:   out[i++] = *s;
        }
    }
    out[i] = 0;
}

static int emit_reference(void)
{
    int i;
    char buf[4096];

    printf("var vm = require('vm');\n");
    printf("var cases = [\n");
    for (i = 0; cases[i].src; i++) {
        escape_js(cases[i].src, buf, sizeof(buf));
        printf("\"%s\",\n", buf);
    }
    printf("];\n");
    printf("for (var i = 0; i < cases.length; i++) {\n"
           "  var out;\n"
           "  try {\n"
           "    var v = vm.runInNewContext(cases[i]);\n"
           "    out = String(v);\n"
           "  } catch (e) {\n"
           "    out = '!' + ((e && e.name) ? e.name : String(e));\n"
           "    if (e !== null && typeof e !== 'object') out = '!' + String(e);\n"
           "  }\n"
           "  out = out.replace(/\\n/g, '\\\\n').replace(/\\t/g, '\\\\t');\n"
           "  process.stdout.write(i + '\\t' + out + '\\n');\n"
           "}\n");
    return 0;
}

static void tsv_escape(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", stdout);
        else if (*s == '\t') fputs("\\t", stdout);
        else putchar(*s);
    }
}

/* ------------------------------------------------------------------ */
/* Safety limits                                                       */
/* ------------------------------------------------------------------ */

static int check_limits(void)
{
    struct { const char *name; const char *src; js_config cfg; } t[4];
    int i, fails = 0;

    for (i = 0; i < 4; i++)
        js_config_default(&t[i].cfg);

    t[0].name = "step budget (infinite loop inside try/catch)";
    t[0].src  = "try { while (true) { } } catch (e) { 'swallowed' }";
    t[0].cfg.max_steps = 200000;

    t[1].name = "heap cap (unbounded string growth)";
    t[1].src  = "var s='x'; for(;;){ s = s + s; }";
    t[1].cfg.max_heap = 1024 * 1024;
    t[1].cfg.max_steps = 100000000UL;

    t[2].name = "call depth (unbounded recursion)";
    t[2].src  = "function f(){ return f() } try { f() } catch (e) { e.name }";
    t[2].cfg.max_call_depth = 16;

    t[3].name = "parser depth (deeply nested parentheses)";
    t[3].src  = 0;                       /* built below */
    t[3].cfg.max_parse_depth = 32;

    for (i = 0; i < 4; i++) {
        js_ctx *ctx;
        js_value r;
        int rc;
        char out[256];
        char *deep = 0;

        if (i == 3) {
            int k, n = 400;
            deep = (char *)malloc((unsigned long)n * 2 + 4);
            for (k = 0; k < n; k++) deep[k] = '(';
            deep[n] = '1';
            for (k = 0; k < n; k++) deep[n + 1 + k] = ')';
            deep[2 * n + 1] = 0;
            t[3].src = deep;
        }
        ctx = js_new(&t[i].cfg);
        if (!ctx) { printf("  %-52s CONTEXT FAILED\n", t[i].name); fails++; continue; }
        rc = js_eval(ctx, t[i].src, "limit", &r);
        render(ctx, rc, r, out, sizeof(out));
        printf("  %-52s -> %s%s\n", t[i].name, out,
               js_fatal(ctx) ? " (fatal, uncatchable)" : "");
        if (rc == JS_OK && i != 2) {
            printf("      FAIL: the limit did not fire\n");
            fails++;
        }
        if (i == 2 && strcmp(out, "RangeError") != 0) {
            printf("      FAIL: expected a catchable RangeError\n");
            fails++;
        }
        js_free(ctx);
        free(deep);
    }
    return fails;
}

/* ------------------------------------------------------------------ */
/* Parser fuzzing                                                      */
/* ------------------------------------------------------------------ */

static unsigned long rng_state = 123456789UL;

static unsigned long rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int fuzz(int n)
{
    static const char alphabet[] =
        "(){}[];,.+-*/%<>=!&|^~?:'\"\\ \n\tabcfginorstuvwxyz0123456789$_";
    int i, crashed = 0, base = 0, ncases = 0;
    char buf[512];

    while (cases[ncases].src) ncases++;

    for (i = 0; i < n; i++) {
        js_config cfg;
        js_ctx *ctx;
        js_value r;
        unsigned long len, k;

        /* half mutations of real programs, half random soup */
        if (i & 1) {
            const char *src = cases[base % ncases].src;
            base++;
            len = strlen(src);
            if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
            memcpy(buf, src, len);
            for (k = 0; k < 1 + (rng() % 4); k++) {
                unsigned long pos = rng() % (len ? len : 1);
                buf[pos] = alphabet[rng() % (sizeof(alphabet) - 1)];
            }
        } else {
            len = 1 + rng() % 120;
            for (k = 0; k < len; k++)
                buf[k] = alphabet[rng() % (sizeof(alphabet) - 1)];
        }
        buf[len] = 0;

        js_config_default(&cfg);
        cfg.max_steps = 200000;
        cfg.max_heap = 2 * 1024 * 1024;
        ctx = js_new(&cfg);
        if (!ctx) { crashed++; continue; }
        js_eval(ctx, buf, "fuzz", &r);
        js_free(ctx);
    }
    return crashed;
}


/* ------------------------------------------------------------------ */
/* Embedding demo                                                      */
/* ------------------------------------------------------------------ */
/* This is not a language test. It exercises exactly the three things a
 * DOM binding layer needs from libjs -- native functions, native property
 * getters and setters, and opaque host pointers surfaced as JS objects --
 * so that the contract is executable rather than merely described. */

#define TAG_ELEM 0x454c454dU

struct demo_elem { const char *id; char text[128]; int clicks; };
static struct demo_elem demo_body = { "body", "Hello", 0 };
static struct demo_elem demo_para = { "p1", "old text", 0 };
static js_object *demo_proto;

static int demo_get_text(js_ctx *c, js_value t, int argc, js_value *v, js_value *r)
{
    struct demo_elem *e = (struct demo_elem *)js_host_ptr(t, TAG_ELEM);
    (void)argc; (void)v;
    if (!e) return js_throw_error(c, JS_ERR_TYPE, "not an element");
    *r = js_mkcstring(c, e->text);
    return JS_OK;
}

static int demo_set_text(js_ctx *c, js_value t, int argc, js_value *v, js_value *r)
{
    struct demo_elem *e = (struct demo_elem *)js_host_ptr(t, TAG_ELEM);
    js_value s;
    if (!e) return js_throw_error(c, JS_ERR_TYPE, "not an element");
    if (js_to_string(c, argc ? v[0] : js_undefined(), &s) != JS_OK) return JS_THROW;
    snprintf(e->text, sizeof(e->text), "%s", js_string_bytes(s, 0));
    *r = js_undefined();
    return JS_OK;
}

static int demo_get_id(js_ctx *c, js_value t, int argc, js_value *v, js_value *r)
{
    struct demo_elem *e = (struct demo_elem *)js_host_ptr(t, TAG_ELEM);
    (void)argc; (void)v;
    *r = e ? js_mkcstring(c, e->id) : js_undefined();
    return JS_OK;
}

static int demo_click(js_ctx *c, js_value t, int argc, js_value *v, js_value *r)
{
    struct demo_elem *e = (struct demo_elem *)js_host_ptr(t, TAG_ELEM);
    (void)argc; (void)v; (void)c;
    if (e) e->clicks++;
    *r = js_undefined();
    return JS_OK;
}

static int demo_get_by_id(js_ctx *c, js_value t, int argc, js_value *v, js_value *r)
{
    const char *n;
    (void)t;
    n = js_to_cstring(c, argc ? v[0] : js_undefined());
    if (!n) return JS_THROW;
    if (strcmp(n, "p1") == 0)
        { *r = js_object_value(js_new_host(c, &demo_para, TAG_ELEM, demo_proto)); return JS_OK; }
    if (strcmp(n, "body") == 0)
        { *r = js_object_value(js_new_host(c, &demo_body, TAG_ELEM, demo_proto)); return JS_OK; }
    *r = js_null();
    return JS_OK;
}

static void demo_print(void *user, const char *s) { (void)user; printf("  [script] %s\n", s); }

static int embed_demo(void)
{
    js_config cfg;
    js_ctx *c;
    js_object *doc;
    js_value r;
    int rc;
    static const char *src =
        "var el = document.getElementById('p1');\n"
        "console.log('id =', el.id, 'text =', el.textContent);\n"
        "el.textContent = 'new ' + [1,2,3].map(function(x){return x*2}).join('-');\n"
        "for (var i = 0; i < 3; i++) el.click();\n"
        "console.log('missing is', String(document.getElementById('nope')));\n"
        "el.id = 'ignored';\n"
        "'done: ' + el.textContent;\n";

    js_config_default(&cfg);
    cfg.print = demo_print;
    c = js_new(&cfg);
    if (!c) { printf("no context\n"); return 1; }
    demo_proto = js_new_object(c);
    js_define_accessor(c, demo_proto, "textContent", demo_get_text, demo_set_text, 1);
    js_define_accessor(c, demo_proto, "id", demo_get_id, 0, 1);  /* read-only */
    js_define_native(c, demo_proto, "click", demo_click, 0);
    doc = js_new_object(c);
    js_define_native(c, doc, "getElementById", demo_get_by_id, 1);
    js_set(c, js_global(c), "document", js_object_value(doc));

    rc = js_eval(c, src, "page.js", &r);
    printf("  result: %s\n", rc == JS_OK ? js_to_cstring(c, r) : js_error_text(c, r));
    printf("  host state: text=\"%s\" clicks=%d id=%s\n",
           demo_para.text, demo_para.clicks, demo_para.id);
    rc = (rc == JS_OK && demo_para.clicks == 3 &&
          strcmp(demo_para.text, "new 2-4-6") == 0 &&
          strcmp(demo_para.id, "p1") == 0) ? 0 : 1;
    printf("  %s\n", rc ? "EMBEDDING DEMO FAILED" : "embedding surface behaved as documented");
    js_free(c);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Async / await + microtask queue tests                              */
/* ------------------------------------------------------------------ */

#define MAX_TEST_DEFERS 256
static struct {
    js_value prom;
    js_value val;
    int      reject;
} g_defers[MAX_TEST_DEFERS];
static int g_defer_count;

static int native_later(js_ctx *ctx, js_value this_val, int argc,
                        js_value *argv, js_value *ret)
{
    js_value p;
    (void)this_val;
    if (g_defer_count >= MAX_TEST_DEFERS)
        return js_throw_error(ctx, JS_ERR_RANGE, "too many test defers");
    p = js_promise_new(ctx);
    if (p.type != JS_OBJECT) return JS_THROW;
    g_defers[g_defer_count].prom = p;
    g_defers[g_defer_count].val = argc > 0 ? argv[0] : js_undefined();
    g_defers[g_defer_count].reject = 0;
    g_defer_count++;
    *ret = p;
    return JS_OK;
}

static int native_later_reject(js_ctx *ctx, js_value this_val, int argc,
                               js_value *argv, js_value *ret)
{
    js_value p;
    (void)this_val;
    if (g_defer_count >= MAX_TEST_DEFERS)
        return js_throw_error(ctx, JS_ERR_RANGE, "too many test defers");
    p = js_promise_new(ctx);
    if (p.type != JS_OBJECT) return JS_THROW;
    g_defers[g_defer_count].prom = p;
    g_defers[g_defer_count].val = argc > 0 ? argv[0] : js_undefined();
    g_defers[g_defer_count].reject = 1;
    g_defer_count++;
    *ret = p;
    return JS_OK;
}

static const struct { const char *src; const char *want; } async_cases[] = {
{ "async function f(){ return 42; } f()", "42" },
{ "async function f(){ return await 21 + await Promise.resolve(21); } f()", "42" },
{ "((async (x) => await __later(x + 1))(41))", "42" },
{ "async function f(){ const a = await __later(10); const b = await __later(a + 20); return a + b; } f()", "40" },
{ "async function f(){ return await { then(res){ res(99); } }; } f()", "99" },
{ "async function f(){ throw new RangeError('boom'); } f()", "!RangeError" },
{ "async function f(){ await __later(1); throw new TypeError('after'); } f()", "!TypeError" },
{ "async function f(){ try { await Promise.reject(new TypeError('bad')); return 'no'; } catch (e) { return await __later(e.name + ':ok'); } } f()", "TypeError:ok" },
{ "async function f(){ try { await __laterReject(new Error('fail')); return 'no'; } catch (e) { return 'caught:' + e.message; } } f()", "caught:fail" },
{ "async function f(){ const log = []; async function inner(){ try { log.push(await __later('a')); return 'ret'; } finally { log.push(await __later('b')); } } const v = await inner(); return log.join(',') + ':' + v; } f()", "a,b:ret" },
{ "async function f(){ const fns = []; for (let i = 0; i < 4; i++) { const v = await __later(i * 10); if (i === 1) continue; fns.push(() => i + '=' + v); if (i === 2) break; } return fns.map(g => g()).join(','); } f()", "0=0,2=20" },
{ "async function f(){ let i = 0, s = 0; while (i < 3) { s += await __later(++i); } do { s += await __later(10); } while (s < 16); return s; } f()", "16" },
{ "async function f(){ let s = ''; for (const [k, v] of new Map([['a', 1], ['b', 2]])) { s += k + (await __later(v)); } return s; } f()", "a1b2" },
{ "async function fact(n){ if (n <= 1) return 1; return n * await fact(await __later(n - 1)); } fact(6)", "720" },
{ "async function sum(){ let s = 0; for (let i = 1; i <= 150; i++) s += await __later(i); return s; } sum()", "11325" },
{ "class A { async val(x = 5){ return await __later(x * 2); } } class B extends A { async val(x){ return (await super.val(x)) + await __later(32); } } const obj = { async m(){ return await new B().val(); } }; obj.m()", "42" },
{ "async function item(x){ const a = await __later(x); return await __later(a * 10); } async function run(){ const xs = await Promise.all(new Set([item(1), item(2), item(3)])); return xs.join(','); } run()", "10,20,30" },
{ "async function f(k){ let r = ''; switch (await __later(k)) { case 1: r = await __later('one'); break; default: r = 'def'; } return r; } f(1)", "one" },
{ "await 1", "!SyntaxError" },
{ "async function f(){ const a = (await __later(null)) ?? (await __later(7)); const b = (await __later({ x: 3 }))?.x; return a + ',' + b; } f()", "7,3" },
{ "async function f(){ const [a, b] = [await __later(3), await __later(4)]; return `${a}+${b}=${a+b}`; } f()", "3+4=7" },
{ "const fn = async x => await __later(x * 3); fn(14)", "42" },
{ "async function f(){ return Promise.reject(new RangeError('rejected')); } f()", "!RangeError" },
{ "async function f(){ try { try { await __laterReject(new TypeError('inner')); } catch (e) { await __later(1); throw e; } } catch (outer) { return 'outer:' + outer.name; } } f()", "outer:TypeError" },
{ "async function f({a = 1}, ...r) { return a + await __later(r.length); } f({}, 10, 20)", "3" },

/* ---- Wave 8B async: Web Crypto subtle.digest & Blob readers ---- */
{ "async function hex(ab){ return Array.from(new Uint8Array(ab), b => (b < 16 ? '0' : '') + b.toString(16)).join(''); } (async () => [await hex(await crypto.subtle.digest('SHA-1', new Uint8Array(0))), await hex(await crypto.subtle.digest('sha1', new TextEncoder().encode('abc')))].join(','))()", "da39a3ee5e6b4b0d3255bfef95601890afd80709,a9993e364706816aba3e25717850c26c9cd0d89d" },
{ "async function hex(ab){ return Array.from(new Uint8Array(ab), b => (b < 16 ? '0' : '') + b.toString(16)).join(''); } (async () => [await hex(await crypto.subtle.digest('SHA-256', new ArrayBuffer(0))), await hex(await crypto.subtle.digest({ name: 'sha256' }, new Uint8ClampedArray([97, 98, 99])))].join(','))()", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855,ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
{ "async function hex(ab){ return Array.from(new Uint8Array(ab), b => (b < 16 ? '0' : '') + b.toString(16)).join(''); } (async () => { const raw = new Uint8Array([120, 97, 98, 99, 121]); const dv = new DataView(raw.buffer, 1, 3); const ab = await crypto.subtle.digest('SHA-384', dv); return (ab instanceof ArrayBuffer) + ':' + ab.byteLength + ':' + await hex(ab); })()", "true:48:cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7" },
{ "async function hex(ab){ return Array.from(new Uint8Array(ab), b => (b < 16 ? '0' : '') + b.toString(16)).join(''); } (async () => { const ab = await crypto.subtle.digest({ name: 'Sha-512' }, new TextEncoder().encode('abc')); return ab.byteLength + ':' + await hex(ab); })()", "64:ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
{ "crypto.subtle.digest('MD5', new Uint8Array(1))", "!NotSupportedError" },
{ "crypto.subtle.digest('SHA-256')", "!TypeError" },
{ "crypto.subtle.digest('SHA-256', 'plain-string')", "!TypeError" },
{ "(async () => { const b = new Blob(['Hello ', new Uint8Array([87, 111]), new Blob(['rld'])]); const s = b.slice(3, -1); const ab = await s.arrayBuffer(); const u8 = await s.bytes(); return [await b.text(), await s.text(), ab instanceof ArrayBuffer, ab.byteLength, u8 instanceof Uint8Array, new TextDecoder().decode(u8)].join('|'); })()", "Hello World|lo Worl|true|7|true|lo Worl" },

{ 0, 0 }
};

static void run_async_one(const char *src, char *out, unsigned long n)
{
    js_config cfg;
    js_ctx *ctx;
    js_value r, tmp;
    int rc, di = 0;

    g_defer_count = 0;
    js_config_default(&cfg);
    cfg.max_steps = 2000000;
    ctx = js_new(&cfg);
    if (!ctx) { snprintf(out, n, "<no context>"); return; }
    js_define_native(ctx, js_global(ctx).u.obj, "__later", native_later, 1);
    js_define_native(ctx, js_global(ctx).u.obj, "__laterReject", native_later_reject, 1);
    if (js_eval(ctx, "var __as='PENDING', __av='';", "init", &tmp) != JS_OK) {
        snprintf(out, n, "<init failed>");
        js_free(ctx);
        return;
    }
    rc = js_eval(ctx, src, "test", &r);
    if (rc != JS_OK) {
        render(ctx, rc, r, out, n);
        js_free(ctx);
        return;
    }
    js_set(ctx, js_global(ctx), "__p", r);
    if (js_eval(ctx,
                "if (!__p || typeof __p.then !== 'function') {"
                "  __as='ERR'; __av='!NotPromise';"
                "} else {"
                "  __p.then(function(v){ __as='OK'; __av=String(v); },"
                "           function(e){ __as='ERR'; __av='!'+((e&&e.name)?e.name:String(e)); });"
                "}",
                "hook", &tmp) != JS_OK) {
        render(ctx, 1, js_exception(ctx), out, n);
        js_free(ctx);
        return;
    }
    for (;;) {
        int jobs = js_run_jobs(ctx, 10000);
        if (jobs < 0 || js_fatal(ctx)) {
            snprintf(out, n, "!Fatal");
            js_free(ctx);
            return;
        }
        if (di < g_defer_count) {
            if (g_defers[di].reject)
                js_promise_reject(ctx, g_defers[di].prom, g_defers[di].val);
            else
                js_promise_resolve(ctx, g_defers[di].prom, g_defers[di].val);
            di++;
            continue;
        }
        break;
    }
    rc = js_eval(ctx, "__as === 'PENDING' ? '!PENDING' : __av", "read", &tmp);
    render(ctx, rc, tmp, out, n);
    js_free(ctx);
}

/* ------------------------------------------------------------------ */

static int stub_random_bytes(void *user, uint8_t *out, unsigned long len)
{
    unsigned long i;
    uint8_t base = user ? *(const uint8_t *)user : 0;

    for (i = 0; i < len; i++)
        out[i] = (uint8_t)(base + i);
    return 0;
}

static int check_random_hook(void)
{
    js_config cfg;
    js_ctx *ctx;
    js_value r;
    uint8_t seed = 0x42;
    char out[256];
    int fails = 0;

    js_config_default(&cfg);
    cfg.user = &seed;
    cfg.random_bytes = stub_random_bytes;
    ctx = js_new(&cfg);
    if (!ctx) return 1;
    render(ctx,
           js_eval(ctx,
                   "const a = crypto.getRandomValues(new Uint8Array(4));"
                   "crypto.randomUUID() + '|' + Array.from(a).join(',')",
                   "rng_hook", &r),
           r, out, sizeof(out));
    if (strcmp(out, "42434445-4647-4849-8a4b-4c4d4e4f5051|66,67,68,69") != 0) {
        printf("FAIL [random_hook] got: %s\n", out);
        fails++;
    }
    js_free(ctx);
    return fails;
}

static int check_shared_execution_budget(void)
{
    js_config cfg;
    js_execution_budget budget;
    js_ctx *jobs = NULL, *limited = NULL, *later = NULL;
    js_value r;
    unsigned long before;
    const char *failure = "context allocation";

    js_execution_budget_init(&budget, 128);
    js_config_default(&cfg);
    cfg.execution_budget = &budget;
    cfg.max_steps = 0;
    jobs = js_new(&cfg);
    if (!jobs)
        goto fail;

    failure = "promise reaction did not use shared credit";
    if (js_eval(jobs, "var n = 0; var p = Promise.resolve();"
                    "p.then(() => { n++; }); p.then(() => { n++; });",
                "shared-jobs", &r) != JS_OK)
        goto fail;
    before = js_execution_budget_remaining(&budget);
    if (js_run_jobs(jobs, 1) != 1 ||
        js_execution_budget_remaining(&budget) >= before)
        goto fail;
    before = js_execution_budget_remaining(&budget);

    failure = "local limit charged a rejected step or reset shared credit";
    cfg.max_steps = 1;
    limited = js_new(&cfg);
    if (!limited ||
        js_eval(limited, "while (true) {}", "local-limit", &r) != JS_THROW ||
        !js_fatal(limited) ||
        strcmp(js_error_text(limited, r), "FatalError: script step limit exceeded") != 0 ||
        js_execution_budget_remaining(&budget) != before - 1)
        goto fail;
    js_reset_steps(limited);
    js_free(limited);
    limited = NULL;
    if (js_execution_budget_remaining(&budget) != before - 1 ||
        js_fatal(jobs))
        goto fail;

    failure = "later context renewed shared credit or caught exhaustion";
    cfg.max_steps = 0;
    later = js_new(&cfg);
    if (!later ||
        js_eval(later, "try { while (true) {} } catch (e) { 1; }",
                "shared-limit", &r) != JS_THROW ||
        !js_fatal(later) ||
        strcmp(js_error_text(later, r),
               "FatalError: shared script execution budget exhausted") != 0 ||
        js_execution_budget_remaining(&budget) != 0)
        goto fail;

    failure = "pending reaction escaped the exhausted pool";
    js_reset_steps(jobs);
    if (js_fatal(jobs) || js_run_jobs(jobs, 1) != -1 || !js_fatal(jobs) ||
        js_execution_budget_remaining(&budget) != 0)
        goto fail;
    failure = NULL;

fail:
    js_free(later);
    js_free(limited);
    js_free(jobs);
    if (failure)
        printf("FAIL [shared execution budget] %s\n", failure);
    else
        printf("PASS shared JS execution credit: local limit, later context, and Promise reactions\n");
    return failure != NULL;
}

static int check_shared_heap_budget(void)
{
    const unsigned long capacity = 4UL * 1024 * 1024;
    js_config cfg;
    js_heap_budget pool, small;
    js_ctx *first = NULL, *second = NULL, *replacement = NULL;
    js_value result;
    js_sbuf scratch = {0};
    unsigned long bootstrap, before;
    const char *failure = "contexts did not reserve separate backing";

    js_heap_budget_init(&pool, capacity);
    js_config_default(&cfg);
    cfg.heap_budget = &pool;
    first = js_new(&cfg);
    bootstrap = capacity - js_heap_budget_remaining(&pool);
    second = js_new(&cfg);
    if (!first || !second || bootstrap == 0 ||
        js_heap_budget_remaining(&pool) != capacity - 2 * bootstrap)
        goto done;

    failure = "a failed context initialization leaked its reservation";
    js_heap_budget_init(&small, bootstrap - 1);
    cfg.heap_budget = &small;
    replacement = js_new(&cfg);
    if (replacement || js_heap_budget_remaining(&small) != bootstrap - 1)
        goto done;
    cfg.heap_budget = &pool;

    failure = "temporary capacity was not charged or returned";
    before = js_heap_budget_remaining(&pool);
    js_sb_init(&scratch, first);
    if (js_sb_putc(&scratch, 'a') != JS_OK || scratch.cap == 0 ||
        js_heap_budget_remaining(&pool) != before - scratch.cap ||
        js_sb_puts(&scratch, "abcdefghijklmnopqrstuvwxyz"
                             "abcdefghijklmnopqrstuvwxyz"
                             "abcdefghijklmnopqrstuvwxyz") != JS_OK ||
        js_heap_budget_remaining(&pool) != before - scratch.cap)
        goto done;
    js_sb_free(&scratch);
    if (js_heap_budget_remaining(&pool) != before)
        goto done;

    failure = "shared exhaustion was catchable or consumed the rejected reservation";
    if (js_eval(first, "var keep = 42;", "keep", &result) != JS_OK ||
        js_eval(second, "try { new Uint8Array(8 * 1024 * 1024); } catch (e) { 1; }",
                "shared-heap-limit", &result) != JS_THROW ||
        !js_fatal(second) || js_heap_budget_remaining(&pool) == 0 ||
        strcmp(js_error_text(second, result),
               "FatalError: shared script heap budget exhausted") != 0)
        goto done;

    failure = "freeing a context did not restore credit without affecting its sibling";
    before = js_heap_budget_remaining(&pool);
    js_free(second);
    second = NULL;
    if (js_heap_budget_remaining(&pool) < before + bootstrap ||
        js_eval(first, "keep", "read-keep", &result) != JS_OK ||
        result.type != JS_NUMBER || result.u.num != 42)
        goto done;

    failure = "local cap stopped taking precedence over available shared credit";
    cfg.max_heap = capacity / 2;
    replacement = js_new(&cfg);
    before = js_heap_budget_remaining(&pool);
    if (!replacement || before < cfg.max_heap ||
        js_alloc_raw(replacement, cfg.max_heap) ||
        js_heap_budget_remaining(&pool) != before ||
        strcmp(js_error_text(replacement, js_undefined()),
               "FatalError: script heap limit exceeded") != 0)
        goto done;
    failure = NULL;

done:
    js_sb_free(&scratch);
    js_free(replacement);
    js_free(second);
    js_free(first);
    if (!failure && js_heap_budget_remaining(&pool) != capacity)
        failure = "credit remained charged after all contexts were destroyed";
    if (failure)
        printf("FAIL [shared heap budget] %s\n", failure);
    else
        printf("PASS shared JS heap credit: local cap, scratch growth, isolation and reuse\n");
    return failure != NULL;
}

int main(int argc, char **argv)
{
    int i, pass = 0, fail = 0, verbose = 0;
    char out[4096];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit") == 0)
            return emit_reference();
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        if (strcmp(argv[i], "--limits") == 0) {
            int f;
            printf("safety limits:\n");
            f = check_limits();
            printf("%s\n", f ? "LIMIT CHECKS FAILED" : "all four limits fired as designed");
            return f ? 1 : 0;
        }
        if (strcmp(argv[i], "--embed") == 0) {
            printf("embedding surface (what the DOM bindings will use):\n");
            return embed_demo();
        }
        if (strcmp(argv[i], "--fuzz") == 0) {
            int n = (i + 1 < argc) ? atoi(argv[i + 1]) : 20000;
            int c = fuzz(n);
            printf("fuzz: %d programs, %d contexts failed to allocate\n", n, c);
            return 0;
        }
        if (strcmp(argv[i], "--dump") == 0) {
            for (i = 0; cases[i].src; i++) {
                run_one(cases[i].src, out, sizeof(out));
                printf("%d\t", i);
                tsv_escape(out);
                putchar('\n');
            }
            return 0;
        }
    }

    for (i = 0; cases[i].src; i++) {
        run_one(cases[i].src, out, sizeof(out));
        if (strcmp(out, cases[i].want) == 0) {
            pass++;
            if (verbose) printf("ok   %s\n", cases[i].src);
        } else {
            fail++;
            printf("FAIL [%d] %s\n     want: %s\n     got:  %s\n",
                   i, cases[i].src, cases[i].want, out);
        }
    }
    for (i = 0; async_cases[i].src; i++) {
        run_async_one(async_cases[i].src, out, sizeof(out));
        if (strcmp(out, async_cases[i].want) == 0) {
            pass++;
            if (verbose) printf("ok   [async %d] %s\n", i, async_cases[i].src);
        } else {
            fail++;
            printf("FAIL [async %d] %s\n     want: %s\n     got:  %s\n",
                   i, async_cases[i].src, async_cases[i].want, out);
        }
    }
    if (check_random_hook() == 0)
        pass++;
    else
        fail++;
    if (check_shared_execution_budget() == 0)
        pass++;
    else
        fail++;
    if (check_shared_heap_budget() == 0)
        pass++;
    else
        fail++;
    printf("\n%d/%d passed, %d failed\n", pass, pass + fail, fail);
    return fail ? 1 : 0;
}
