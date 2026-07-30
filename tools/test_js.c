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
{ "2**0", "!SyntaxError" },

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
    printf("\n%d/%d passed, %d failed\n", pass, pass + fail, fail);
    return fail ? 1 : 0;
}
