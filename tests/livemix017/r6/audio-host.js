import { GuestWorker } from '../../../livemix/webjoin/audio/worker.js';
import { encodeAudio, encodeRoster, readPacket, decodeAudio, Kind } from '../../../livemix/webjoin/audio/packet.js';

let engine, timer, rate, start, members, seqs = [0,0], latestStats, revision = 1;
let sendSecond = true, totals, window;
const ppms = [550,-370];
const tones = [[700,1100],[1700,2300]];
const data = new Float32Array(240), decoded = new Float32Array(240);
const base = origin => ({session:0x123456789abcdef0n,origin,linkSender:1,seq:0,firstFrame:0n,streamEpoch:1,rosterRevision:revision});
function resetWindow() { window={packets:0,squared:0,samples:0,max:0,first:null,last:null}; }
function received(buffer) {
  const h=readPacket(buffer);
  if(h.kind===Kind.rosterAck) return;
  if(h.kind!==Kind.audio || h.flags!==2 || h.frames!==120 || h.origin!==2 || h.linkSender!==2) throw new Error('upstream');
  if(totals.last!==null && h.firstFrame<=totals.last) throw new Error('clock');
  totals.last=h.firstFrame; totals.packets++;
  decodeAudio(h,decoded);
  window.packets++; window.first??=String(h.firstFrame); window.last=String(h.firstFrame);
  for(let f=0;f<120;f++) { const v=decoded[f*2]; if(v!==decoded[f*2+1]) throw new Error('mono'); window.squared+=v*v;window.samples++;window.max=Math.max(window.max,Math.abs(v)); }
}
function roster() { engine.receive(encodeRoster(base(1),rate,members),BigInt(Math.round(performance.now()*1e6))); }
function tick() {
  const elapsed=(performance.now()-start)/1000;
  for(let peer=0;peer<2;peer++) {
    if(peer===1 && !sendSecond) continue;
    const generated=Math.floor(elapsed*rate*(1+ppms[peer]*1e-6)/120);
    // A timer stall must not dump arbitrarily old PCM into the engine.
    if(generated-seqs[peer]>8) {totals.sourceDropped+=generated-seqs[peer]-8;seqs[peer]=generated-8;}
    while(seqs[peer]<generated) {
      const seq=seqs[peer]++, h={...base(peer===0?1:3),seq,firstFrame:BigInt(seq)*120n};
      for(let f=0;f<120;f++) for(let c=0;c<2;c++) data[f*2+c]=0.2*Math.sin(2*Math.PI*tones[peer][c]*(seq*120+f)/rate);
      engine.receive(encodeAudio(h,data,false),BigInt(Math.round(performance.now()*1e6)));
    }
  }
}
self.onmessage=e=>{
  const m=e.data;
  if(m.type==='start') {
    rate=m.rate; totals={packets:0,last:null,sourceDropped:0};resetWindow();
    members=[{participantId:1,streamEpoch:1,kind:'native',nick:'host',state:'connected'},
      {participantId:2,streamEpoch:1,kind:'web',nick:'web',state:'connected'},
      {participantId:3,streamEpoch:1,kind:'native',nick:'native',state:'connected'}];
    engine=new GuestWorker(value=>{if(value.type==='metrics')latestStats=value;});
    engine.configureConnection({participantId:2,streamEpoch:1,connectionGeneration:1,room:{session:'123456789abcdef0',wireRate:rate,roomMode:'v2',members}});
    engine.message({type:'audioPort',generation:1,wireRate:rate,deviceRate:m.deviceRate,port:m.port});
    engine.channel={readyState:'open',bufferedAmount:0,send:received,close(){}};
    roster(); start=performance.now(); timer=setInterval(tick,1);
    self.postMessage({type:'ready'});
  } else if(m.type==='window') { const result={...window,rms:Math.sqrt(window.squared/Math.max(1,window.samples))}; resetWindow();self.postMessage({id:m.id,result}); }
  else if(m.type==='mic') engine.message({type:'mic',generation:1,enabled:m.enabled});
  else if(m.type==='remove') {sendSecond=false;members=members.slice(0,2);engine.room.members=members;revision++;roster();self.postMessage({id:m.id,result:true});}
  else if(m.type==='status') self.postMessage({id:m.id,result:{metrics:latestStats,totals:{...totals,last:String(totals.last)},sourcePpm:ppms}});
  else if(m.type==='close') {clearInterval(timer);engine.close();self.close();}
};
