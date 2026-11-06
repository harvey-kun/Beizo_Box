#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MusicBox</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:18px;font:16px system-ui,-apple-system,sans-serif;background:#0f1220;color:#e8eaf6;display:flex;justify-content:center}
.wrap{width:100%;max-width:420px}
.card{background:#181c31;border-radius:16px;padding:18px;margin-bottom:14px}
h1{font-size:22px;margin:0 0 2px} h2{font-size:15px;margin:0 0 12px;color:#aab3e6;font-weight:600}
.sub{margin:0 0 16px;color:#98a0c8;font-size:14px}
button,.btn{display:inline-block;border:0;border-radius:12px;padding:12px 16px;font:inherit;font-weight:600;color:#fff;background:#3d5afe;cursor:pointer;text-align:center}
button:disabled{opacity:.4;cursor:default}
.btn.alt{background:#2a3054}
.row{display:flex;gap:10px} .row>*{flex:1}
.bar{height:8px;background:#2a3054;border-radius:6px;overflow:hidden;margin:12px 0 6px}
.bar>i{display:block;height:100%;width:0;background:#69f0ae;transition:width .3s}
.small{font-size:13px;color:#98a0c8;min-height:18px}
.err{color:#ff8a80}
#fname{font-size:14px;margin:10px 0 0;word-break:break-all;color:#cfd6ff}
.time{display:flex;justify-content:space-between;font-variant-numeric:tabular-nums}
</style></head><body><div class="wrap">
<div class="card"><h1>&#127925; MusicBox</h1>
<p class="sub">Send a song to the buzzer. Big files are converted in your phone, only a few KB are uploaded.</p>
<input id="file" type="file" accept="audio/*,.mid,.midi,.mp3,.wav,.m4a,.ogg,.flac" hidden>
<div class="row"><label class="btn alt" for="file">Choose music</label><button id="send" disabled>Convert &amp; send</button></div>
<div id="fname"></div>
<div class="bar"><i id="prog"></i></div>
<div id="msg" class="small">Pick a MIDI, MP3, WAV... file.</div></div>

<div class="card"><h2>Player</h2>
<div class="row"><button id="play" disabled>&#9654; Play</button><button id="pause" disabled>&#10074;&#10074; Pause</button><button id="stop" class="alt" disabled>&#9632; Stop</button></div>
<div class="bar"><i id="pbar"></i></div>
<div class="small time"><span id="el">0:00</span><span id="st">no song</span><span id="tot">0:00</span></div></div>
<div class="small" style="text-align:center">If the picker doesn't open in this popup, open <b>http://192.168.4.1</b> in Chrome/Safari.</div>
</div>
<script>
const $=id=>document.getElementById(id);
const MIN_HZ=300,MAX_HZ=3500,MAX_BYTES=300000;
const hz=m=>440*Math.pow(2,(m-69)/12);
function msg(t,e){const m=$('msg');m.textContent=t;m.className='small'+(e?' err':'');}
function fmt(ms){const s=Math.floor(ms/1000);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');}

// raw: [{f:Hz or 0 for rest, ms}] -> piezo friendly binary (uint16 freq, uint16 ms)
function finalize(raw){
  const out=[];
  for(const n of raw){
    let f=n.f;
    if(f>0){while(f<MIN_HZ)f*=2;while(f>MAX_HZ)f/=2;f=Math.round(f);}
    const last=out[out.length-1];
    if(last&&last.f===f)last.ms+=n.ms;
    else if(last&&n.ms<30)last.ms+=n.ms;
    else out.push({f:f,ms:n.ms});
  }
  while(out.length&&out[0].f===0)out.shift();
  while(out.length&&out[out.length-1].f===0)out.pop();
  const p=[];
  for(const n of out){let ms=Math.round(n.ms);while(ms>0){const d=Math.min(ms,65535);p.push(n.f,d);ms-=d;}}
  const u=new Uint8Array(p.length*2);
  for(let i=0;i<p.length;i+=2){const o=i*2;u[o]=p[i]&255;u[o+1]=p[i]>>8;u[o+2]=p[i+1]&255;u[o+3]=p[i+1]>>8;}
  return {bytes:u,notes:p.length/2,ms:out.reduce((a,n)=>a+n.ms,0)};
}

// ---- MIDI -> melody (highest sounding note wins, drums ignored) ----
function midiToRaw(buf){
  const d=new DataView(buf);let p=0;
  const str=n=>{let s='';for(let i=0;i<n;i++)s+=String.fromCharCode(d.getUint8(p++));return s;};
  const vlq=()=>{let v=0,b;do{b=d.getUint8(p++);v=(v<<7)|(b&127);}while(b&128);return v;};
  if(str(4)!=='MThd')throw new Error('Not a MIDI file');
  p+=4;p+=2;const ntr=d.getUint16(p);p+=2;const div=d.getUint16(p);p+=2;
  if(div&0x8000)throw new Error('SMPTE-timed MIDI not supported');
  const tempos=[{tick:0,uspq:500000}],notes=[];
  for(let t=0;t<ntr;t++){
    if(str(4)!=='MTrk')throw new Error('Bad MIDI track');
    const len=d.getUint32(p);p+=4;const end=p+len;let tick=0,run=0;const open=new Map();
    while(p<end){
      tick+=vlq();
      let st=d.getUint8(p);
      if(st&128){p++;if(st<0xF0)run=st;}else st=run;
      if(st===0xFF){
        const type=d.getUint8(p++);const l=vlq();
        if(type===0x51&&l===3)tempos.push({tick:tick,uspq:(d.getUint8(p)<<16)|(d.getUint8(p+1)<<8)|d.getUint8(p+2)});
        p+=l;
      }else if(st===0xF0||st===0xF7){const l=vlq();p+=l;}
      else{
        const hi=st&0xF0,ch=st&15;const a=d.getUint8(p++);let b=0;
        if(hi!==0xC0&&hi!==0xD0)b=d.getUint8(p++);
        if(ch!==9){
          const k=ch*128+a;
          if(hi===0x90&&b>0){if(!open.has(k))open.set(k,tick);}
          else if(hi===0x80||(hi===0x90&&b===0)){if(open.has(k)){notes.push({on:open.get(k),off:tick,pitch:a});open.delete(k);}}
        }
      }
    }
    p=end;
  }
  tempos.sort((a,b)=>a.tick-b.tick);
  const segs=[];let acc=0;
  for(let i=0;i<tempos.length;i++){
    const t=tempos[i];
    if(i>0){const q=segs[segs.length-1];acc=q.ms+(t.tick-q.tick)*q.uspq/1000/div;}
    segs.push({tick:t.tick,ms:acc,uspq:t.uspq});
  }
  const t2ms=t=>{let s=segs[0];for(const q of segs){if(q.tick<=t)s=q;else break;}return s.ms+(t-s.tick)*s.uspq/1000/div;};
  const ev=[];
  for(const n of notes){const a=t2ms(n.on),b=t2ms(n.off);if(b>a){ev.push({t:a,on:1,p:n.pitch},{t:b,on:0,p:n.pitch});}}
  ev.sort((x,y)=>x.t-y.t||x.on-y.on);
  const act=new Map(),raw=[];let lastT=0,lastTop=-1,i=0;
  while(i<ev.length){
    const t=ev[i].t;
    while(i<ev.length&&ev[i].t===t){const e=ev[i++];const c=(act.get(e.p)||0)+(e.on?1:-1);if(c>0)act.set(e.p,c);else act.delete(e.p);}
    let top=-1;for(const k of act.keys())if(k>top)top=k;
    if(top!==lastTop){if(t>lastT)raw.push({f:lastTop<0?0:hz(lastTop),ms:t-lastT});lastT=t;lastTop=top;}
  }
  return raw;
}

// ---- audio (mp3/wav/...) -> melody via pitch detection ----
async function audioToRaw(buf,prog){
  const AC=window.AudioContext||window.webkitAudioContext;let ctx;
  try{ctx=new AC({sampleRate:11025});}catch(e){ctx=new AC();}
  const ab=await new Promise((ok,no)=>ctx.decodeAudioData(buf,ok,no));
  try{ctx.close();}catch(e){}
  const sr=ab.sampleRate,dec=Math.max(1,Math.round(sr/11025)),rate=sr/dec;
  const chs=[];for(let c=0;c<ab.numberOfChannels;c++)chs.push(ab.getChannelData(c));
  const N=Math.floor(ab.length/dec),x=new Float32Array(N);let peak=0;
  for(let i=0;i<N;i++){
    let s=0;for(let k=0;k<dec;k++)for(let c=0;c<chs.length;c++)s+=chs[c][i*dec+k];
    s/=dec*chs.length;x[i]=s;const a=Math.abs(s);if(a>peak)peak=a;
  }
  const W=Math.round(rate*0.046),H=Math.round(rate*0.023);
  const minL=Math.max(2,Math.floor(rate/2200)),maxL=Math.floor(rate/150);
  const hopMs=H/rate*1000,frames=Math.max(0,Math.floor((N-W)/H));
  const midi=new Int16Array(frames),nsdf=new Float32Array(maxL+2);
  for(let fr=0;fr<frames;fr++){
    if(fr%150===0){prog(fr/frames);await new Promise(r=>setTimeout(r));}
    const s=fr*H;let e0=0;for(let i=0;i<W;i++)e0+=x[s+i]*x[s+i];
    if(Math.sqrt(e0/W)<0.04*peak){midi[fr]=-1;continue;}
    const m=W-maxL-1;let best=0;
    for(let l=minL;l<=maxL;l++){
      let r=0,q=0;
      for(let i=0;i<m;i++){const a=x[s+i],b=x[s+i+l];r+=a*b;q+=a*a+b*b;}
      const v=q>0?2*r/q:0;nsdf[l]=v;if(v>best)best=v;
    }
    let pick=-1;
    if(best>0.6){for(let l=minL+1;l<maxL;l++){if(nsdf[l]>=0.9*best&&nsdf[l]>=nsdf[l-1]&&nsdf[l]>=nsdf[l+1]){pick=l;break;}}}
    if(pick<0){midi[fr]=-1;continue;}
    const a=nsdf[pick-1],b=nsdf[pick],c=nsdf[pick+1],den=a-2*b+c;
    const off=den!==0?0.5*(a-c)/den:0;
    midi[fr]=Math.round(69+12*Math.log2(rate/(pick+off)/440));
  }
  const sm=new Int16Array(frames);
  for(let i=0;i<frames;i++){
    const a=midi[Math.max(0,i-1)],b=midi[i],c=midi[Math.min(frames-1,i+1)];
    sm[i]=a+b+c-Math.max(a,b,c)-Math.min(a,b,c);
  }
  const raw=[];let i=0;
  while(i<frames){let j=i;while(j<frames&&sm[j]===sm[i])j++;raw.push({f:sm[i]<0?0:hz(sm[i]),ms:(j-i)*hopMs});i=j;}
  return raw;
}

function upload(bytes){
  return new Promise((ok,no)=>{
    const fd=new FormData();fd.append('song',new Blob([bytes]),'song.bin');
    const x=new XMLHttpRequest();x.open('POST','/upload');
    x.upload.onprogress=e=>{if(e.lengthComputable){$('prog').style.width=(50+50*e.loaded/e.total)+'%';}};
    x.onload=()=>x.status===200?ok(JSON.parse(x.responseText)):no(new Error('Board rejected the file (too big or storage error)'));
    x.onerror=()=>no(new Error('Connection to the board was lost'));
    x.send(fd);
  });
}

$('file').onchange=()=>{const f=$('file').files[0];$('send').disabled=!f;$('fname').textContent=f?f.name+' ('+(f.size/1048576).toFixed(1)+' MB)':'';};

$('send').onclick=async()=>{
  const f=$('file').files[0];if(!f)return;
  $('send').disabled=true;$('prog').style.width='0';
  try{
    msg('Reading file...');
    const buf=await f.arrayBuffer();
    const isMidi=new TextDecoder().decode(new Uint8Array(buf,0,4))==='MThd';
    msg(isMidi?'Converting MIDI...':'Decoding audio and detecting melody...');
    const raw=isMidi?midiToRaw(buf):await audioToRaw(buf,p=>{$('prog').style.width=(p*50)+'%';});
    let r=finalize(raw);
    if(r.notes===0)throw new Error('No playable notes found. Try a MIDI file or a cleaner melody.');
    let bytes=r.bytes,note='';
    if(bytes.length>MAX_BYTES){bytes=bytes.slice(0,MAX_BYTES);note=' (trimmed to fit)';}
    $('prog').style.width='50%';
    msg('Sending '+(bytes.length/1024).toFixed(1)+' KB ('+r.notes+' notes)...');
    const res=await upload(bytes);
    $('prog').style.width='100%';
    msg('Done: '+res.notes+' notes, '+fmt(res.ms)+note+'. Press Play.');
    poll();
  }catch(e){msg(e.message||String(e),true);}
  $('send').disabled=false;
};

const cmd=c=>fetch('/'+c,{method:'POST'}).then(poll).catch(()=>{});
$('play').onclick=()=>cmd('play');$('pause').onclick=()=>cmd('pause');$('stop').onclick=()=>cmd('stop');

async function poll(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    const has=s.total>0;
    $('play').disabled=!has||s.state==='playing';
    $('pause').disabled=s.state!=='playing';
    $('stop').disabled=s.state==='idle';
    $('el').textContent=fmt(s.elapsed);$('tot').textContent=fmt(s.total);
    $('st').textContent=has?s.state:'no song';
    $('pbar').style.width=(has?Math.min(100,100*s.elapsed/s.total):0)+'%';
  }catch(e){$('st').textContent='offline';}
}
setInterval(poll,800);poll();
</script></body></html>)rawliteral";
