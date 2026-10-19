'use strict';
(() => {
  const W=400,H=300,FPS=40,PERIOD=6.5,TAU=Math.PI*2;
  const INK=48,PAPER=198,ink='#000000',paper='#ffffff';
  const $=s=>document.querySelector(s),display=$('#display');
  const output=display.getContext('2d',{alpha:false});
  const make=()=>{const c=document.createElement('canvas');c.width=W;c.height=H;return c;};
  const buffer=make(),a=make(),b=make();
  const ctx=buffer.getContext('2d',{willReadFrequently:true}),ac=a.getContext('2d'),bc=b.getContext('2d');
  const clamp=(n,min=0,max=1)=>Math.max(min,Math.min(max,n));
  const ease=p=>p*p*(3-2*p),lerp=(a,b,p)=>a+(b-a)*p;
  const glyphs=new Map(),numbers=new Map();
  const profiles={
    calm:{co2:624,pm:3.2,voc:78,nox:1,temp:22.4,rh:46,mood:'Settled.',title:'Room to breathe.',note:'A QUIET MORNING'},
    occupied:{co2:1380,pm:7.8,voc:126,nox:4,temp:24.1,rh:58,mood:'Busy.',title:'A room with life.',note:'PEOPLE IN THE ROOM'},
    cooking:{co2:925,pm:38.6,voc:218,nox:27,temp:25.6,rh:63,mood:'Active.',title:'Something in the air.',note:'COOKING IN PROGRESS'},
    fresh:{co2:438,pm:1.4,voc:47,nox:1,temp:20.6,rh:42,mood:'Fresh.',title:'Let the outside in.',note:'WINDOWS WIDE OPEN'}
  };
  let target=profiles.calm,origin={...target},sensor={...target},scenario='calm',changedAt=-10;
  let t=0,current=0,age=0,paused=false,auto=true,transition=null,queued=null;
  let warning=false,statusAt=0;
  let previous=0,nextFrame=0,fpsSince=0,frames=0,sampleAt=0,rendered=0;
  const history=[];
  const keys=['co2','pm','voc','nox','temp','rh'];
  const ranges={co2:[400,2000],pm:[0,75],voc:[0,500],nox:[0,50],temp:[15,35],rh:[0,100]};
  const normalized=(key,value)=>clamp((value-ranges[key][0])/(ranges[key][1]-ranges[key][0]));
  let visual={...sensor},visualOrigin={...sensor},visualAt=-10;
  const historyX=at=>388-(t-at)*312/120;
  const topic=['AIR PORTRAIT','CARBON DIOXIDE','PARTICULATE MATTER','GAS SIGNATURES','MICROCLIMATE'];

  // Type is rasterized and snapped to whole pixels, without dithered edges.
  function text(c,str,x,y,size=10,weight=400,align='left',color=ink,mono=false){
    const font=`${weight} ${size}px ${mono?'Consolas, monospace':'Arial, sans-serif'}`;
    const key=`${font}|${str}|${color===paper}`;let glyph=glyphs.get(key);
    if(!glyph){
      // Draw scientific subscripts explicitly: consistent even without fallback fonts.
      const runs=Array.from(String(str),ch=>{
        const special=ch==='₂'||ch==='³',runSize=special?Math.round(size*.65):size;
        const runFont=`${weight} ${runSize}px ${mono?'Consolas, monospace':'Arial, sans-serif'}`;
        c.font=runFont;const value=ch==='₂'?'2':ch==='³'?'3':ch;
        return {value,font:runFont,width:c.measureText(value).width,dy:ch==='₂'?size*.12:ch==='³'?-size*.35:0};
      });
      const width=runs.reduce((sum,run)=>sum+run.width,0),baseline=Math.ceil(size*1.25);
      const tile=document.createElement('canvas');tile.width=Math.ceil(width)+4;tile.height=Math.ceil(size*1.7)+4;
      const g=tile.getContext('2d',{willReadFrequently:true});g.fillStyle=color;let cursor=2;
      for(const run of runs){g.font=run.font;g.fillText(run.value,cursor,baseline+run.dy);cursor+=run.width;}
      const p=g.getImageData(0,0,tile.width,tile.height);
      for(let i=3;i<p.data.length;i+=4)p.data[i]=p.data[i]>=105?255:0;
      g.putImageData(p,0,0);glyph={tile,width,baseline};if(glyphs.size>1400)glyphs.clear();glyphs.set(key,glyph);
    }
    const offset=align==='center'?glyph.width/2:align==='right'?glyph.width:0;
    c.drawImage(glyph.tile,Math.round(x-offset-2),Math.round(y-glyph.baseline));
  }
  function label(c,str,x,y,align='left',color=ink){text(c,str,x,y,18,600,align,color);}
  function reading(c,id,value,x,y,size,color=ink){
    let state=numbers.get(id);
    if(!state){state={value,old:value,at:-10};numbers.set(id,state);}
    if(state.value!==value){state.old=state.value;state.value=value;state.at=t;}
    const p=clamp((t-state.at)/.6),q=ease(p),h=Math.ceil(size*1.15);
    c.save();c.beginPath();c.rect(x-3,y-h,245,h+7);c.clip();
    if(p<1)text(c,state.old,x,y-Math.round(h*q),size,600,'left',color);
    text(c,value,x,y+Math.round(h*(1-q)),size,600,'left',color);c.restore();
  }
  function line(c,x1,y1,x2,y2,color=ink,width=3){c.beginPath();c.strokeStyle=color;c.lineWidth=width;c.moveTo(x1,y1);c.lineTo(x2,y2);c.stroke();}
  function dot(c,x,y,r=2,color=ink){c.beginPath();c.fillStyle=color;c.arc(x,y,r,0,TAU);c.fill();}
  function circle(c,x,y,r,color=ink,width=1){c.beginPath();c.strokeStyle=color;c.lineWidth=width;c.arc(x,y,r,0,TAU);c.stroke();}
  function base(c){c.fillStyle=paper;c.fillRect(0,0,W,H);}
  function heading(c,title,unit='',reverse=false){
    if(reverse){c.fillStyle=ink;c.fillRect(0,0,W,54);}
    const color=reverse?paper:ink;
    text(c,title,20,38,24,700,'left',color);text(c,unit,380,37,18,600,'right',color);
  }
  function metric(c,x,name,value,unit='',color=ink){
    label(c,name,x,236,'left',color);reading(c,'metric-'+name,value,x,275,36,color);
    c.font='600 36px Arial';text(c,unit,x+c.measureText(value).width+7,274,16,600,'left',color);
  }
  function sample(at=t){
    visualOrigin={...visual};visualAt=t;
    const p=ease(clamp((at-changedAt)/3));
    for(const [i,key] of ['co2','pm','voc','nox','temp','rh'].entries()){
      const amp={co2:7,pm:.22,voc:2,nox:0,temp:.1,rh:.5}[key];
      sensor[key]=lerp(origin[key],target[key],p)+Math.sin(at*.16+i)*amp;
    }
    history.push(Object.freeze({at,...Object.fromEntries(keys.map(key=>[key,sensor[key]]))}));
    // Illustrative demo cues, not a regulatory AQI or hardware alarm policy.
    const nextWarning=sensor.co2>1000||sensor.pm>20;
    if(nextWarning!==warning){warning=nextWarning;statusAt=t;}
  }

  function portrait(c){
    heading(c,'AIR QUALITY');
    const mood=warning?'CHECK AIR':'GOOD';c.font='600 18px Arial';
    const badgeWidth=c.measureText(mood).width+20;c.fillStyle=ink;c.fillRect(380-badgeWidth,16,badgeWidth,29);
    text(c,mood,370,37,18,600,'right',paper);
    label(c,'CO₂',20,84);
    const value=String(Math.round(sensor.co2));reading(c,'home-co2',value,16,160,78);text(c,'ppm',20,186,18,600);
    const cx=308,cy=132,breath=Math.sin(t*2.2),inner=39+breath*3;
    // A moving aperture makes the status mark the entrance to the entire journey.
    dot(c,cx,cy,73);dot(c,cx,cy,67,paper);
    for(let i=0;i<7;i++){
      const q=i*TAU/7+t*.85,r=65+Math.sin(t*2+i)*2;
      c.beginPath();c.moveTo(cx+Math.cos(q)*inner,cy+Math.sin(q)*inner);
      c.bezierCurveTo(cx+Math.cos(q+.1)*r,cy+Math.sin(q+.1)*r,cx+Math.cos(q+.75)*r,cy+Math.sin(q+.75)*r,cx+Math.cos(q+.65)*inner,cy+Math.sin(q+.65)*inner);
      c.closePath();c.fillStyle=ink;c.fill();
    }
    dot(c,cx,cy,35,paper);
    c.save();c.translate(cx,cy);
    if(warning){
      const pulse=1+.11*Math.sin(t*4.2);c.scale(pulse,pulse);
      c.beginPath();c.moveTo(0,-25);c.lineTo(27,23);c.lineTo(-27,23);c.closePath();c.strokeStyle=ink;c.lineWidth=5+Math.sin(t*4.2);c.lineJoin='round';c.stroke();
      line(c,0,-8,0,7,ink,6);dot(c,0,15,3.2);
    }else{
      const phase=(t-statusAt)%4.5,p=clamp(phase/.7),fade=1-clamp((phase-3.8)/.7);
      c.beginPath();c.rect(-35,-35,70,70*fade);c.clip();
      const pts=[[-23,0],[-6,18],[26,-20]],lengths=[25,50];let remaining=p*75;
      c.beginPath();c.moveTo(...pts[0]);
      for(let i=0;i<2;i++){const f=clamp(remaining/lengths[i]);c.lineTo(lerp(pts[i][0],pts[i+1][0],f),lerp(pts[i][1],pts[i+1][1],f));remaining-=lengths[i];if(f<1)break;}
      c.strokeStyle=ink;c.lineWidth=8;c.lineCap='round';c.lineJoin='round';c.stroke();
    }
    c.restore();
    c.fillStyle=ink;c.fillRect(0,211,400,89);line(c,211,226,211,282,paper,2);
    const footers=[[
      ['PM2.5',sensor.pm.toFixed(1),'µg/m³'],['TEMP',sensor.temp.toFixed(1),'°C']
    ],[['VOC',String(Math.round(sensor.voc)),'index'],['HUMIDITY',String(Math.round(sensor.rh)),'% RH']]];
    footers.forEach((items,i)=>{
      const time=t+i*.65,k=Math.floor(time/4.5)%2,p=ease(clamp(((time%4.5)-3.85)/.65)),direction=i===0?1:-1;
      c.save();c.beginPath();c.rect(i===0?0:212,212,i===0?211:188,88);c.clip();
      c.save();c.translate(0,-Math.round(p*92)*direction);metric(c,i===0?20:236,...items[k],paper);c.restore();
      if(p>0){c.translate(0,Math.round((1-p)*92)*direction);metric(c,i===0?20:236,...items[1-k],paper);}
      c.restore();
    });
  }

  function historyFrame(c){
    line(c,16,180,384,180,ink,2);
    const duration=history.length?Math.min(120,Math.max(0,t-2)):0;
    text(c,duration>=120?'2 MIN AGO':`${Math.floor(duration)} SEC AGO`,16,198,16,600);
    text(c,'NOW',388,198,16,600,'right');
    c.save();c.beginPath();c.rect(76,204,312,96);c.clip();
  }
  const sampleStrength=(item,key)=>normalized(key,item[key]);
  function bubbles(c,x,y,width,height,strength,seed,arrival=1){
    const count=width>10?4+Math.round(strength*12):1+Math.round(strength*5);
    for(let j=0;j<count;j++){
      const phase=(t*(.12+j*.007)+j*.618+seed*.17)%1;
      const r=(width>10?4+j%4*2:2.2)*arrival;
      const bx=x+width*(.25+.5*((j*.381+seed*.11)%1))+Math.sin(t*1.7+j+seed)*Math.min(1.4,width*.12);
      const by=y+height*(1-phase*phase);
      if(r>.1)circle(c,bx,by,r,ink,width>10?2:1.4);
    }
  }
  function fan(c,x,y){
    circle(c,x,y,27,ink,3);
    for(let i=0;i<5;i++){
      const q=t*3+i*TAU/5;
      c.beginPath();[[5,q],[23,q+.25],[20,q+.9],[6,q+1.1]].forEach(([r,a],j)=>{
        const px=x+Math.cos(a)*r,py=y+Math.sin(a)*r;j?c.lineTo(px,py):c.moveTo(px,py);
      });c.closePath();c.fillStyle=ink;c.fill();
    }
    dot(c,x,y,5,paper);dot(c,x,y,3);
  }
  function co2(c){
    heading(c,'CO₂','ppm');
    reading(c,'detail-co2',String(Math.round(sensor.co2)),16,143,88);
    c.save();c.beginPath();c.rect(285,53,99,113);c.clip();
    bubbles(c,289,56,88,108,normalized('co2',visual.co2),0);c.restore();
    historyFrame(c);
    for(const item of history){
      const x=historyX(item.at);
      bubbles(c,x-5.2,205,5.2,91,sampleStrength(item,'co2'),item.at,ease(clamp((t-item.at)/.6)));
    }
    c.restore();text(c,'CO₂',16,253,18,700);
  }
  function particles(c){
    heading(c,'PARTICLES','PM2.5');
    reading(c,'pm',sensor.pm.toFixed(1),16,120,72);text(c,'µg/m³',203,118,18,600);
    fan(c,345,104);
    [sensor.pm*.69,sensor.pm*1.18,sensor.pm*1.42].forEach((v,i)=>{
      const x=16+i*128;text(c,['PM1','PM4','PM10'][i],x,149,16,600);text(c,v.toFixed(1),x+47,173,24,700);
    });
    historyFrame(c);
    // Flow paths suggest the chamber, while particle positions retain time.
    for(let j=0;j<3;j++){
      c.beginPath();for(let x=76;x<=388;x+=4){const y=223+j*27+Math.sin(x*.023-t*.8+j)*5;x===76?c.moveTo(x,y):c.lineTo(x,y);}
      c.strokeStyle=ink;c.lineWidth=1;c.stroke();
    }
    for(const item of history){
      const x=historyX(item.at),n=1+Math.round(sampleStrength(item,'pm')*9),arrival=ease(clamp((t-item.at)/.6));
      for(let j=0;j<n;j++){
        const y=211+((j*.618+item.at*.13)%1)*81+Math.sin(t*2+j)*2;
        dot(c,x-2.6+Math.sin(t*3+j+item.at)*1.2,y,2*arrival);
      }
    }
    c.restore();fan(c,39,251);
  }
  // Interpolate only between actual samples; never extend into unmeasured time.
  function historyValue(at,key){
    if(!history.length)return sensor[key];
    for(let i=1;i<history.length;i++)if(at<=history[i].at){
      const left=history[i-1],right=history[i];
      const value=i===history.length-1?lerp(left[key],right[key],ease(clamp((t-right.at)/.6))):right[key];
      return lerp(left[key],value,clamp((at-left.at)/(right.at-left.at)));
    }
    const last=history.at(-1),before=history.at(-2)||last;
    return lerp(before[key],last[key],ease(clamp((t-last.at)/.6)));
  }
  function sculpt(c,key,cy,phase,climateMode=false){
    if(!history.length)return;
    const start=Math.max(76,historyX(history[0].at)),end=388;
    const patches=[];
    for(let x=start;x<end;x+=3){
      const vertices=[];
      for(const xx of [x,Math.min(x+3,end)]){
        const at=t-(388-xx)*120/312,n=normalized(key,historyValue(at,key));
        const arrival=ease(clamp((t-history[0].at)/.6));
        const twist=xx*(climateMode?.035:.055)+phase;
        const amplitude=climateMode&&key==='temp'?3+n*12:7;
        const y=cy+(climateMode?Math.sin(twist):Math.cos(twist))*amplitude;
        const width=(climateMode?(key==='temp'?10:6+n*22):8+n*22)*arrival;
        const half=width*(.3+.7*Math.abs(Math.cos(twist)))/2;
        const edge=clamp(Math.min(xx-start,end-xx)/24);
        const projectedX=xx+(climateMode?0:Math.sin(twist)*24*edge);
        vertices.push({x:projectedX,y,half,lean:Math.sin(twist)*width*.35,z:Math.sin(twist)});
      }
      patches.push({vertices,z:vertices[0].z});
    }
    // Rear facets first, then front facets, separated by a reflective seam.
    patches.sort((a,b)=>a.z-b.z);
    for(const {vertices:v,z} of patches){
      c.beginPath();c.moveTo(v[0].x-v[0].lean,v[0].y-v[0].half);c.lineTo(v[1].x-v[1].lean,v[1].y-v[1].half);
      c.lineTo(v[1].x+v[1].lean,v[1].y+v[1].half);c.lineTo(v[0].x+v[0].lean,v[0].y+v[0].half);c.closePath();c.fillStyle=ink;c.fill();
      if(z>0)line(c,v[0].x-v[0].lean*.25,v[0].y-v[0].half*.25,v[1].x-v[1].lean*.25,v[1].y-v[1].half*.25,paper,2);
      if(climateMode&&Math.floor(v[0].x/3)%4===0)line(c,v[0].x-v[0].lean,v[0].y-v[0].half,v[0].x+v[0].lean,v[0].y+v[0].half,paper,2);
    }
  }
  function gas(c){
    heading(c,'GASES','INDEX');
    label(c,'VOC',20,78);label(c,'NOx',224,78);
    line(c,204,62,204,162,ink,2);
    reading(c,'voc',String(Math.round(sensor.voc)),16,151,72);
    reading(c,'nox',String(Math.round(sensor.nox)),220,151,72);
    historyFrame(c);sculpt(c,'voc',226,t*.8);sculpt(c,'nox',278,-t*.7+1);c.restore();
    text(c,'VOC',16,233,16,700);text(c,'NOx',16,285,16,700);
  }
  function climate(c){
    heading(c,'CLIMATE');label(c,'TEMP',20,78);label(c,'HUMIDITY',224,78);
    line(c,204,62,204,162,ink,2);
    reading(c,'temp',sensor.temp.toFixed(1),16,145,62);text(c,'°C',160,169,18,600);
    reading(c,'rh',String(Math.round(sensor.rh)),220,145,62);text(c,'%',361,169,18,600);
    historyFrame(c);sculpt(c,'temp',226,t*.65,true);sculpt(c,'rh',278,t*.65+1.5,true);c.restore();
    text(c,'TEMP',16,233,16,700);text(c,'RH',16,285,16,700);
  }
  const sceneDraw=[portrait,co2,particles,gas,climate];
  function portal(c,home,detail,q,isWarning){
    const px=lerp(308,200,q),py=lerp(132,150,q),zoom=1+q*q*12;
    c.save();c.translate(px,py);c.scale(zoom,zoom);c.drawImage(home,-308,-132);c.restore();
    const opening=ease(clamp((q-.08)/.92));
    if(opening<=0)return;
    c.save();c.beginPath();
    if(isWarning){
      const r=opening*850;c.moveTo(px,py-r);c.lineTo(px+r*.95,py+r*.8);c.lineTo(px-r*.95,py+r*.8);c.closePath();
    }else c.arc(px,py,opening*430,0,TAU);
    c.clip();
    const scale=1+.18*(1-q);c.translate(200,150);c.scale(scale,scale);c.drawImage(detail,-200,-150);c.restore();
  }
  function journey(c){
    const {from,to,p,warning:portalWarning}=transition,q=ease(clamp(p));
    base(ac);sceneDraw[from](ac);base(bc);sceneDraw[to](bc);
    if(from===0)portal(c,a,b,q,portalWarning);
    else if(to===0)portal(c,b,a,1-q,portalWarning);
    else{
      const direction=to>from?1:-1,y=Math.round(q*H)*direction;
      c.drawImage(a,0,-y);c.drawImage(b,0,direction*H-y);
    }
  }
  function draw(){
    const drawStarted=performance.now();
    base(ctx);
    if(transition)journey(ctx);else sceneDraw[current](ctx);
    const frame=ctx.getImageData(0,0,W,H),p=frame.data;
    // Hard monochrome threshold: no dither noise, antialias shades, or tiny specks.
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){
      const i=(y*W+x)*4,v=p[i]>=128?PAPER:INK;p[i]=p[i+1]=p[i+2]=v;p[i+3]=255;
    }
    output.putImageData(frame,0,0);
    if(rendered===0){
      const actual=output.getImageData(0,0,W,H).data,colors=new Set();
      for(let i=0;i<actual.length;i+=4)colors.add([actual[i],actual[i+1],actual[i+2],actual[i+3]].join(','));
      display.dataset.pixelColors=[...colors].sort().join(';');display.dataset.colorCount=String(colors.size);
    }
    rendered++;
    display.dataset.scene=String(current);
    display.dataset.transition=transition?(transition.from===0||transition.to===0?'status-portal':'continuous-scroll'):'none';
    display.dataset.frames=String(rendered);
    display.dataset.drawMs=(performance.now()-drawStarted).toFixed(1);
  }
  const destination=()=>queued??(transition?transition.to:current);
  function go(index){
    index=(index+5)%5;display.dataset.navigation=String(index);
    if(paused){current=index;transition=null;queued=null;draw();}
    else if(transition){queued=index;}
    else if(index!==current){transition={from:current,to:index,p:0,warning,duration:current===0||index===0?1.4:1.1};}
    age=0;
    document.querySelectorAll('button[data-scene]').forEach((el,i)=>el.setAttribute('aria-pressed',String(i===index)));
  }
  function pause(value){paused=value;previous=0;frames=0;fpsSince=performance.now();$('#play').textContent=value?'Play':'Pause';$('#play').setAttribute('aria-label',value?'Resume animation':'Pause animation');$('#fps').textContent=value?'0':'40';draw();}
  $('#play').addEventListener('click',()=>pause(!paused));
  $('#previous').addEventListener('click',()=>go(destination()-1));$('#next').addEventListener('click',()=>go(destination()+1));
  display.addEventListener('click',()=>go(destination()+1));document.querySelectorAll('button[data-scene]').forEach(el=>el.addEventListener('click',()=>go(Number(el.dataset.scene))));
  let wheelDistance=0;
  display.addEventListener('wheel',event=>{
    event.preventDefault();
    wheelDistance+=event.deltaY;
    if(Math.abs(wheelDistance)>65){go(destination()+(wheelDistance>0?1:-1));wheelDistance=0;}
  },{passive:false});
  $('#auto').addEventListener('change',e=>{auto=e.target.checked;age=0;});
  $('#scenario').addEventListener('change',e=>{
    const value=e.target.value;if(!profiles[value])return;origin={...sensor};scenario=value;target=profiles[value];changedAt=t;
  });
  function scale(value){
    const n=clamp(Math.round(Number(value)),1,3);document.documentElement.style.setProperty('--scale',n);$('#scale').value=String(n);
    $('#pixelInfo').textContent=`${n}× · EACH PIXEL IS ${n} × ${n} CSS PIXEL${n===1?'':'S'}`;
  }
  $('#scale').addEventListener('change',e=>scale(e.target.value));scale(innerWidth>=850&&innerHeight>=790?2:1);
  $('#snapshot').addEventListener('click',()=>{const link=document.createElement('a');link.download=`atmos-${topic[current].toLowerCase().replaceAll(' ','-')}-400x300.png`;link.href=display.toDataURL('image/png');link.click();});
  document.addEventListener('keydown',e=>{
    if(e.target.matches('input,select,button,a'))return;
    if(e.code==='Space'){e.preventDefault();pause(!paused);}if(e.code==='ArrowLeft'){e.preventDefault();go(destination()-1);}if(e.code==='ArrowRight'){e.preventDefault();go(destination()+1);}
  });
  function advance(dt){
    t+=dt;age+=dt;
    if(t-sampleAt>=2){sampleAt+=2;sample(sampleAt);}
    const blend=ease(clamp((t-visualAt)/.6));
    for(const key of keys)visual[key]=lerp(visualOrigin[key],sensor[key],blend);
    while(history.length&&history[0].at<t-120)history.shift();
    if(transition){
      transition.p+=dt/transition.duration;
      if(transition.p>=1){
        current=transition.to;transition=null;age=0;
        if(queued!==null){const next=queued;queued=null;go(next);}
      }
    }
    if(auto&&age>=PERIOD&&!transition)go(current+1);
  }
  document.addEventListener('visibilitychange',()=>{previous=0;nextFrame=0;});
  function tick(now){
    requestAnimationFrame(tick);if(!previous){previous=now;nextFrame=now;fpsSince=now;}const dt=Math.min((now-previous)/1000,.1);previous=now;
    if(!paused&&!document.hidden){
      advance(dt);
      if(now>=nextFrame){draw();frames++;nextFrame+=1000/FPS;if(now-nextFrame>50)nextFrame=now+1000/FPS;}
    }else nextFrame=now;
    if(now-fpsSince>=1500){if(!paused)$('#fps').textContent=String(Math.round(frames*1000/(now-fpsSince)));frames=0;fpsSince=now;}
  }
  window.atmos={getState:()=>({width:W,height:H,colors:['#303030','#c6c6c6'],targetFps:FPS,rendered,scene:current,scenario,paused,sensors:{...sensor},history:history.map(item=>({...item})),time:t,transition:transition?{...transition}:null,queued})};
  draw();requestAnimationFrame(tick);
})();
