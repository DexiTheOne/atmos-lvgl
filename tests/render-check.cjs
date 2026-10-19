// Run with ATMOS_CANVAS_MODULE pointing to an installed @napi-rs/canvas.
// Exercises the real renderer without a browser or production-only test API.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const {createCanvas,GlobalFonts}=require(process.env.ATMOS_CANVAS_MODULE||'@napi-rs/canvas');
for(const font of ['arial.ttf','arialbd.ttf'])if(fs.existsSync('C:/Windows/Fonts/'+font))GlobalFonts.registerFromPath('C:/Windows/Fonts/'+font,'Arial');
const source=fs.readFileSync('app.js','utf8').replace('  draw();requestAnimationFrame(tick);',`  window.test={advance,draw,go,pause,normalized,historyX,historyValue,
  settle(id){current=id;transition=null;queued=null;auto=false;numbers.clear();draw();},
  transition(from,to,p,w=false){transition={from,to,p,warning:w,duration:1.4};draw();},
  automatic(){auto=true;age=0;},tick};
  draw();requestAnimationFrame(tick);`);
function setup(){
  const els=new Map(),listeners={};let raf;
  function el(id){if(!els.has(id)){const e=id==='#display'?createCanvas(400,300):{};e.dataset={};e.events={};e.addEventListener=(n,f)=>e.events[n]=f;e.setAttribute=()=>{};els.set(id,e);}return els.get(id);}
  const document={hidden:false,querySelector:el,querySelectorAll:()=>[],createElement:()=>createCanvas(400,300),documentElement:{style:{setProperty(){}}},addEventListener:(n,f)=>listeners[n]=f};
  const context={document,window:{},innerWidth:1000,innerHeight:900,performance,requestAnimationFrame:f=>raf=f,console};vm.runInNewContext(source,context);
  const test=context.window.test,state=context.window.atmos.getState;
  const pixels=()=>Buffer.from(el('#display').getContext('2d').getImageData(0,0,400,300).data);
  const advance=seconds=>{for(let i=0;i<Math.round(seconds*40);i++)test.advance(.025);};
  const scenario=value=>el('#scenario').events.change({target:{value}});
  return {test,state,el,pixels,advance,scenario,document,listeners};
}
const r=setup();assert.equal(r.state().scene,0);assert.equal(r.state().transition,null);assert.equal(r.state().history.length,0);
r.test.settle(0);r.advance(2.1);assert.equal(r.state().history.length,1);
const first=JSON.stringify(r.state().history[0]);const x=r.test.historyX(2);r.advance(10);assert.equal(JSON.stringify(r.state().history[0]),first);assert.ok(r.test.historyX(2)<x);
for(const name of ['calm','occupied','cooking','fresh']){
  r.scenario(name);r.advance(6);const st=r.state();assert.equal(st.scenario,name);for(const key of ['co2','pm','voc','nox','temp','rh'])assert.equal(st.sensors[key],st.history.at(-1)[key]);
}
r.advance(100);assert.ok(r.state().history.length<=61);assert.ok(r.state().history.every(h=>h.at>=r.state().time-120));
for(const key of ['co2','pm','voc','nox','temp','rh']){assert.equal(r.test.normalized(key,-10000),0);assert.equal(r.test.normalized(key,10000),1);}
r.test.settle(0);r.test.go(1);r.test.go(2);r.test.go(4);assert.equal(r.state().transition.to,1);assert.equal(r.state().queued,4);r.advance(1.425);assert.equal(r.state().transition.to,4);r.advance(1.2);assert.equal(r.state().scene,4);
r.test.settle(2);r.test.draw();const frozen=r.pixels();r.test.pause(true);assert.deepEqual(r.pixels(),frozen);const pausedTime=r.state().time;r.test.tick(1000);r.test.tick(2000);assert.equal(r.state().time,pausedTime);r.test.pause(false);
r.document.hidden=true;r.listeners.visibilitychange();r.test.tick(3000);r.test.tick(9000);assert.equal(r.state().time,pausedTime);r.document.hidden=false;r.listeners.visibilitychange();r.test.tick(10000);assert.equal(r.state().time,pausedTime);
let checked=0;const durations=[];
for(const [from,to] of [[0,1],[1,2],[2,3],[3,4],[4,0],[0,4],[4,3],[3,2],[2,1],[1,0]])for(const warning of [false,true]){
  r.test.settle(from);const start=r.pixels();r.test.settle(to);const end=r.pixels();
  r.test.transition(from,to,0,warning);assert.deepEqual(r.pixels(),start,`start ${from}->${to}`);
  r.test.transition(from,to,1,warning);assert.deepEqual(r.pixels(),end,`end ${from}->${to}`);
  for(let j=0;j<=10;j++){
    const time=performance.now();r.test.transition(from,to,j/10,warning);durations.push(performance.now()-time);
    const p=r.pixels();for(let i=0;i<p.length;i+=4)assert.ok((p[i]===48||p[i]===198)&&p[i]===p[i+1]&&p[i]===p[i+2]&&p[i+3]===255);
    checked++;
  }
}
// Native-size scenes after a recorded quiet -> occupied -> cooking -> fresh history.
const v=setup();v.test.settle(0);
for(const name of ['calm','occupied','cooking','fresh']){v.scenario(name);v.advance(30);}
v.advance(.7);
const sheet=createCanvas(800,900),sc=sheet.getContext('2d');sc.fillStyle='#c6c6c6';sc.fillRect(0,0,800,900);
for(let id=0;id<5;id++){v.test.settle(id);sc.drawImage(v.el('#display'),id%2*400,Math.floor(id/2)*300);}
v.test.transition(0,1,.5,true);sc.drawImage(v.el('#display'),400,600);
fs.writeFileSync('artifacts/journey-review.png',sheet.toBuffer('image/png'));
// One complete cycle, exported as individual frames for an optional GIF review.
if(process.env.ATMOS_CAPTURE==='1'){
  fs.mkdirSync('artifacts/journey-frames',{recursive:true});v.test.settle(0);v.test.automatic();
  for(let i=0;i<400;i++){v.advance(.1);v.test.draw();fs.writeFileSync(`artifacts/journey-frames/${String(i).padStart(4,'0')}.png`,v.el('#display').toBuffer('image/png'));}
}
durations.sort((a,b)=>a-b);
console.log(JSON.stringify({passed:true,transitionFrames:checked,drawMedianMs:durations[Math.floor(durations.length*.5)],drawP95Ms:durations[Math.floor(durations.length*.95)],checks:['empty startup','shared samples','immutable history','window pruning','ranges','queued navigation','pause','hidden freeze','20 transition endpoints','two-tone output']},null,2));
