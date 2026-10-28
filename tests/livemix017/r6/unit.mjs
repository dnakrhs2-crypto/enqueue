import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import { ASRC, Drift } from '../../../livemix/webjoin/audio/asrc.js';
import { FrameFIFO, TransferPool, normalChannelClose, expired, audioBudget, audioPackets } from '../../../livemix/webjoin/audio/buffers.js';
import * as packet from '../../../livemix/webjoin/audio/packet.js';
import { Playout } from '../../../livemix/webjoin/audio/playout.js';
import { GuestAudio } from '../../../livemix/webjoin/audio/worklet.js';
import { GuestWorker } from '../../../livemix/webjoin/audio/worker.js';
import { Signaling } from '../../../livemix/webjoin/signaling.js';
import { parseInvite, createGuestClient } from '../../../livemix/webjoin/guest-client.js';

const results = [];
async function test(name, fn) {
  const start = performance.now();
  try { const numbers = await fn(); results.push({ name, ok: true, ms: performance.now() - start, ...numbers }); console.log('PASS', name, JSON.stringify(numbers ?? {})); }
  catch (e) { results.push({ name, ok: false, error: e.stack }); console.error('FAIL', name, e); }
}
const session = 0x123456789abcdef0n;
const header = (seq = 0, origin = 1) => ({ session, origin, linkSender: origin, seq, firstFrame: BigInt(seq) * 120n, streamEpoch: 1, rosterRevision: 1 });
const members = [
  { participantId: 1, streamEpoch: 1, nick: 'host', kind: 'native', state: 'connected' },
  { participantId: 2, streamEpoch: 1, nick: 'web', kind: 'web', state: 'connected' },
  { participantId: 3, streamEpoch: 1, nick: 'native', kind: 'native', state: 'connected' }
];
const pcm = new Float32Array(240).fill(0.2);
const message = seq => ({ generation: 1, origin: 1, streamEpoch: 1, seq, firstFrame: BigInt(seq) * 120n, frames: 120, arrivalNs: BigInt(seq) * 2500000n });
const tone = (n, rate, hz, amp = 0.2) => Float32Array.from({ length: n }, (_, i) => amp * Math.sin(2 * Math.PI * hz * i / rate));
function amplitude(data, frequency, rate) {
  let x = 0, y = 0;
  for (let i = 0; i < data.length; i++) { const a = 2 * Math.PI * frequency * i / rate; x += data[i] * Math.cos(a); y += data[i] * Math.sin(a); }
  return 2 * Math.hypot(x, y) / data.length;
}

await test('invite fragment and API surface', () => {
  const hash = '#v=2&code=0042&invite=' + 'ab'.repeat(32) + '&generation=4294967295';
  assert.deepEqual(parseInvite(hash), { code: '0042', inviteToken: 'ab'.repeat(32), inviteGeneration: 4294967295 });
  for (const bad of [hash + '&code=0042', hash.replace('0042', '４８２７'), hash.replace('4294967295', '4294967296'), hash.replace('v=2', 'v=1')]) assert.throws(() => parseInvite(bad));
  const client = createGuestClient({});
  for (const key of ['inspect','prepareMicrophone','requestJoin','setMicEnabled','setPeerVolume','cancel','leave','dispose']) assert.equal(typeof client[key], 'function');
  assert.throws(() => client.setPeerVolume(1, 2));
});
await test('PCM16/24 finite clamp rounding and L/R', () => {
  let max16 = 0, max24 = 0;
  for (const mono of [true, false]) {
    const input = tone(mono ? 120 : 240, 48000, 1700, 0.75);
    input.set([-Infinity, NaN, Infinity, -2, 2, -0.5 / (mono ? 32768 : 8388608)]);
    const b = packet.encodeAudio(header(17), input, mono);
    const h = packet.readPacket(b, session), output = packet.decodeAudio(h, new Float32Array(240));
    assert.equal(b.byteLength, mono ? 288 : 768); assert.equal(h.firstFrame, 2040n);
    assert.equal(output[0], 0); assert.equal(output[mono ? 6 : 3], -1);
    assert.equal(output[mono ? 10 : 5], -1 / (mono ? 32768 : 8388608));
    for (let i = 6; i < input.length; i++) {
      const error = Math.abs(output[mono ? i * 2 : i] - input[i]);
      if (mono) { max16 = Math.max(max16, error); assert.equal(output[i * 2], output[i * 2 + 1]); }
      else max24 = Math.max(max24, error);
    }
  }
  assert.ok(max16 <= 1 / 65536); assert.ok(max24 <= 1 / 16777216);
  return { max16, max24, monoLRDifference: 0 };
});
await test('malformed headers and payload sizes rejected', () => {
  const good = packet.encodeAudio(header(), pcm, false);
  const mutations = [[0,0],[4,1],[5,2],[6,3],[16,0],[32,128],[34,0],[36,0],[40,0],[44,0],[24,1]];
  for (const [offset,value] of mutations) { const b = good.slice(0); new Uint8Array(b)[offset] = value; assert.throws(() => packet.readPacket(b, session), String(offset)); }
  assert.throws(() => packet.readPacket(good, 1n));
  assert.throws(() => packet.readPacket(good.slice(0, -1)));
  const padded = new Uint8Array(769); padded.set(new Uint8Array(good)); assert.throws(() => packet.readPacket(padded));
  return { rejected: mutations.length + 3 };
});
await test('roster validation, ACK and old revision audio', () => {
  const b = packet.encodeRoster({ ...header(91), firstFrame: 0n }, 48000, members);
  const h = packet.readPacket(b), r = packet.readRoster(h, { selfId: 2, wireRate: 48000, allowedIds: [1,2,3] });
  assert.equal(r.members.length, 3);
  const ack = packet.readPacket(packet.rosterAck(h, 2));
  assert.equal(ack.seq, 91); assert.equal(ack.origin, 2); assert.equal(ack.rosterRevision, 1); assert.equal(ack.payloadBytes, 0);
  for (const invalid of [[members[0],members[0],members[1]], [members[1],members[2]],
    [members[0],members[1],{...members[2],kind:'web'}], [members[0],{...members[1],streamEpoch:0}]]) {
    assert.throws(() => packet.readRoster(packet.readPacket(packet.encodeRoster(header(),48000,invalid)), { selfId: 2 }));
  }
  const broken = b.slice(0); new Uint8Array(broken)[86] = 0xff;
  assert.throws(() => packet.readRoster(packet.readPacket(broken), { selfId: 2 }));
  const audio = packet.readPacket(packet.encodeAudio({ ...header(0,3), linkSender:1 }, pcm, false));
  assert.ok(packet.validDownlink(audio,2,2,r.members));
  assert.ok(!packet.validDownlink({...audio,rosterRevision:3},2,2,r.members));
  assert.ok(!packet.validDownlink({...audio,streamEpoch:2},2,2,r.members));
  assert.ok(!packet.validDownlink(audio,2,2,r.members.slice(0,2)));
});
await test('128/120 and variable render-size FIFO continuity', () => {
  const fifo = new FrameFIFO(1024); const out = new Float32Array(120); let inputFrame = 0, outputFrame = 0;
  for (let n = 0; n < 1000; n++) {
    const size = [128,128,64,191,256][n%5]; const input = Float32Array.from({length:size}, () => inputFrame++);
    assert.ok(fifo.push(input,size));
    while (fifo.pop(out,120)) for (let i = 0; i < 120; i++) assert.equal(out[i],outputFrame++);
  }
  assert.equal(inputFrame - outputFrame,fifo.size);
  return {inputFrames:inputFrame,outputFrames:outputFrame,remainder:fifo.size};
});
function resample(input, inputRate, outputRate) {
  const asrc = new ASRC(1,inputRate,outputRate);
  const output = new Float32Array(Math.ceil(input.length*outputRate/inputRate)+4);
  const made = asrc.process(input,input.length,output,output.length);
  return output.slice(0,made);
}
await test('ASRC 44.1/48 kHz round trips with fractional group delay', () => {
  const errors = [];
  for (const [a,b] of [[44100,48000],[48000,44100]]) {
    const input = tone(a,a,1000,0.5); const output = resample(resample(input,a,b),b,a);
    const delay = 16 + 16*a/b; let squared = 0, count = 0;
    for (let i = 200; i < output.length-200; i++) { const expected = 0.5*Math.sin(2*Math.PI*1000*(i-delay)/a); squared += (output[i]-expected)**2; count++; }
    const rms = Math.sqrt(squared/count); assert.ok(rms < 0.0001, String(rms)); errors.push({ inputRate:a,viaRate:b,rms,delayFrames:delay });
  }
  return {errors};
});
await test('lm_drift PI follows independent +/- clock errors', () => {
  const measurements=[];
  for (const wanted of [550,-370,1000,-1000]) {
    const d = new Drift(960); let fill=960;
    for (let i=0;i<12000;i++) { const ppm=d.update(fill,0.01); fill += 48000*0.01*(wanted-ppm)*1e-6; }
    assert.ok(Math.abs(d.ppm-wanted)<30, `${wanted}: ${d.ppm}`);
    measurements.push({sourcePpm:wanted,correctionPpm:d.ppm,bufferMs:fill/48});
  }
  return {measurements};
});
await test('playout reorder, duplicate, late discard, PLC and rebuffer', () => {
  // Packet mechanics from a 20 ms start; a call's 100 ms start is in the stream tests below.
  const p = new Playout(48000,48000,{startMs:20}); p.reset(1);
  for (const n of [2,0,1,4,3,5,6,7,8,9,10,12,13,14,15]) assert.ok(p.push(message(n),pcm));
  assert.ok(!p.push(message(5),pcm));
  const left = new Float32Array(128), right = new Float32Array(128);
  for (let i=0;i<5;i++) p.render(left,right,128);
  assert.ok(p.playing); assert.ok(!p.push(message(0),pcm)); assert.equal(p.late,1);
  for (let i=0;i<25;i++) p.render(left,right,128);
  assert.ok(p.lost>=1); assert.equal(p.underruns,1); assert.ok(!p.playing);
  for (let n=30;n<44;n++) p.push(message(n),pcm);
  for (let i=0;i<4;i++) p.render(left,right,128);
  assert.ok(p.playing);
  // A prolonged interruption before the first buffer fills must not leave that
  // very first packet pinning the playback cursor seconds behind the sender.
  const startup=new Playout(48000,48000,{startMs:20});startup.reset(1);startup.push(message(0),pcm);
  for(let n=2000;n<2014;n++)startup.push(message(n),pcm);
  for(let i=0;i<4;i++)startup.render(left,right,128);
  assert.ok(startup.playing);assert.ok(startup.cursor>=240000);
  return p.stats({});
});
// A host sends each device block as one burst after a fixed network delay; a stall holds every
// arrival inside [at, at + ms) until its end, the way a phone's LTE/Wi-Fi link stops and catches up.
function streamPlayout({ seconds, block = 1024, rate = 48000, stalls = [], skip = null, startMs, probe = () => {} }) {
  const p = new Playout(rate, 48000, startMs === undefined ? {} : { startMs }); p.reset(1);
  const data = new Float32Array(240).fill(0.25), left = new Float32Array(120), right = new Float32Array(120);
  const arrivals = []; let produced = 0, seq = 0;
  for (let b = 1; b * block * 1000 / rate < seconds * 1000 + 200; b++) {
    const sent = b * block * 1000 / rate;
    produced += block;
    while ((seq + 1) * 120 <= produced) {
      let at = sent + 20;
      for (const s of stalls) if (at >= s.at && at < s.at + s.ms) at = s.at + s.ms;
      if (!skip || seq < skip.from || seq >= skip.from + skip.packets) arrivals.push({ seq, at });
      seq++;
    }
  }
  let k = 0, next = 1;
  for (let i = 0; i < seconds * 48000 / 128; i++) {
    const now = (i + 1) * 128 / 48;
    while (k < arrivals.length && arrivals[k].at <= now) {
      const a = arrivals[k++];
      p.push({ streamEpoch: 1, frames: 120, firstFrame: BigInt(a.seq) * 120n, arrivalNs: BigInt(Math.round(a.at * 1e6)) }, data);
    }
    p.render(left, right, 120); p.render(left, right, 8);
    if (now >= next * 1000) probe(next++, p.stats({}));
  }
  return p.stats({});
}
await test('a network stall sizes the buffer once: the next stalls of that size play through', () => {
  const stalls = [{ at: 10000, ms: 100 }, { at: 40000, ms: 100 }, { at: 70000, ms: 80 }];
  let afterFirst = null;
  const s = streamPlayout({ seconds: 90, stalls, startMs: 20, probe: (second, st) => { if (second === 12) afterFirst = st; } });
  assert.equal(afterFirst.underruns, 1, JSON.stringify(afterFirst));
  assert.ok(afterFirst.spreadMs >= 100 && afterFirst.targetMs >= 100, JSON.stringify(afterFirst));
  assert.equal(s.underruns, 1, JSON.stringify(s)); assert.equal(s.late, 0); assert.equal(s.lost, 1);
  return { underruns: s.underruns, spreadMs: s.spreadMs, targetMs: s.targetMs };
});
await test('a call starts at 100 ms; a calm link steps down to the 20 ms floor within 90 s, a 2048-frame host block to its burst', () => {
  const calm = streamPlayout({ seconds: 90, block: 256 });
  assert.equal(calm.underruns, 0); assert.equal(calm.targetMs, 20, JSON.stringify(calm));
  const big = streamPlayout({ seconds: 90, block: 2048, rate: 44100 });
  assert.equal(big.underruns, 0, JSON.stringify(big)); assert.ok(big.targetMs > 40 && big.targetMs < 60, JSON.stringify(big));
  return { calmTargetMs: calm.targetMs, block2048TargetMs: big.targetMs, block2048SpreadMs: big.spreadMs };
});
await test("a phone's first Wi-Fi stalls up to 90 ms play through: the call starts with 100 ms", () => {
  const s = streamPlayout({ seconds: 40, stalls: [{ at: 4000, ms: 90 }, { at: 9000, ms: 60 }, { at: 21000, ms: 85 }] });
  assert.equal(s.underruns, 0, JSON.stringify(s)); assert.equal(s.lost, 0);
  return { underruns: s.underruns, targetMs: s.targetMs, spreadMs: s.spreadMs };
});
await test('100 ms the host skipped is silence once, not a bigger buffer', () => {
  // Nothing exists to play for those 100 ms; the packets after them are on time.
  const s = streamPlayout({ seconds: 20, block: 256, startMs: 20, skip: { from: 2000, packets: 40 } });
  assert.equal(s.underruns, 1, JSON.stringify(s)); assert.ok(s.targetMs <= 22.5, JSON.stringify(s)); assert.ok(s.spreadMs < 6, JSON.stringify(s));
  return { lost: s.lost, targetMs: s.targetMs, spreadMs: s.spreadMs };
});
await test('five calm minutes after a stall step the buffer back down, never below what it still needs', () => {
  const marks = {};
  streamPlayout({ seconds: 380, startMs: 20, stalls: [{ at: 5000, ms: 100 }], probe: (second, st) => { if ([300, 380].includes(second)) marks[second] = st; } });
  assert.ok(marks[300].targetMs >= 100, JSON.stringify(marks[300]));
  assert.ok(marks[380].targetMs <= 30 && marks[380].targetMs >= 20, JSON.stringify(marks[380]));
  assert.equal(marks[380].underruns, 1);
  return { targetAt300s: marks[300].targetMs, targetAt380s: marks[380].targetMs };
});
await test('capture 120 frames, mute zeros, volume does not change upstream', () => {
  const audio = new GuestAudio(7,44100,48000); let count=0, last=-120n, mutePackets=0;
  audio.port = {postMessage(m) {
    if(m.type !== 'capture') return;
    assert.equal(m.frames,120); assert.equal(m.buffer.byteLength,480); assert.equal(m.firstFrame,last+120n);
    last=m.firstFrame; count++;
    if(!audio.enabled && count>30) { assert.ok(new Float32Array(m.buffer).every(v=>v===0)); mutePackets++; }
    audio.message({type:'recycle',pool:'capture',generation:7,buffer:m.buffer});
  }};
  audio.message({type:'configure',generation:7,wireRate:44100,deviceRate:48000,streamEpoch:1});
  const input = tone(256,48000,440), left=new Float32Array(256),right=new Float32Array(256);
  for(let i=0;i<10;i++) audio.process(input,left,right);
  audio.message({type:'mic',generation:7,enabled:false});
  for(let i=0;i<25;i++) audio.process(input,left,right);
  assert.ok(mutePackets>10); assert.equal(audio.captureDropped,0);
  return {packets:count,mutePackets,lastFrame:String(last),captureDropped:audio.captureDropped};
});
await test('two origins, stereo separation, gain ramp, epoch and removal isolation', () => {
  // Gain and separation from a 20 ms start: a call's step down from 100 ms shifts pitch by ~2 cents for a minute,
  // which a 2-second single-bin amplitude reads as a level change.
  const audio=new GuestAudio(1,48000,48000,{startMs:20}), left=new Float32Array(128),right=new Float32Array(128);
  audio.setGain(3,0.5); audio.people([{participantId:1,streamEpoch:1},{participantId:3,streamEpoch:1}]);
  assert.equal(audio.peers[1].gain,0.5); audio.setGain(3,1);
  const data=new Float32Array(240); let seq=0;
  const measure = (seconds) => {
    const L=[],R=[];
    for(let block=0;block<Math.round(seconds*48000/128);block++) {
      while(seq*120 < (block+1)*128 + measure.frame + 1200) {
        for(const [index,frequencies] of [[0,[700,1100]],[1,[1700,2300]]]) {
          if (!audio.peers[index].id || audio.peers[index].retiring) continue;
          for(let f=0;f<120;f++) for(let c=0;c<2;c++) data[f*2+c]=0.2*Math.sin(2*Math.PI*frequencies[c]*(seq*120+f)/48000);
          audio.peers[index].playout.push({...message(seq),streamEpoch:audio.peers[index].epoch},data);
        }
        seq++;
      }
      audio.process(null,left,right);
      if(block>100) { L.push(...left); R.push(...right); }
    }
    measure.frame+=Math.round(seconds*48000/128)*128;
    return {a:amplitude(L,700,48000),b:amplitude(L,1700,48000),rightA:amplitude(R,1100,48000),rightB:amplitude(R,2300,48000),cross:amplitude(R,700,48000)};
  };
  measure.frame=0;
  const full=measure(2); audio.setGain(1,0.5); const half=measure(2); audio.setGain(1,0); const zero=measure(2);
  assert.ok(Math.abs(half.a/full.a-0.5)<0.02); assert.ok(zero.a<0.002);
  assert.ok(Math.abs(half.b/full.b-1)<0.02); assert.ok(Math.abs(zero.b/full.b-1)<0.02);
  assert.ok(full.cross/full.a<0.01); assert.ok(full.rightA>0.18 && full.rightB>0.18);
  const retained=audio.peers[1].playout; const before=retained.underruns;
  audio.people([{participantId:3,streamEpoch:1}]); const removed=measure(2);
  assert.equal(audio.peers[1].playout,retained); assert.equal(retained.underruns,before); assert.ok(removed.b>0.18);
  audio.people([{participantId:3,streamEpoch:2}]); assert.equal(audio.peers[1].gain,1);
  return {full,half,zero,removed,remainingUnderruns:retained.underruns};
});
await test('120 seconds of two independently drifting playout clocks', () => {
  const runs=[];
  for(const rate of [44100,48000]) {
  const audio=new GuestAudio(1,rate,48000), left=new Float32Array(128),right=new Float32Array(128);
  audio.people([{participantId:1,streamEpoch:1},{participantId:3,streamEpoch:1}]);
  const ppm=[550,-370],seq=[0,0],data=new Float32Array(240);let silent=0,firstAudible=null;
  for(let block=0;block<45000;block++) {
    const time=(block+1)*128/48000;
    for(let peer=0;peer<2;peer++) {
      const count=Math.floor(time*rate*(1+ppm[peer]*1e-6)/120);
      while(seq[peer]<count) {
        const n=seq[peer]++;
        for(let f=0;f<120;f++) {data[f*2]=0.2*Math.sin(2*Math.PI*(peer?1700:700)*(n*120+f)/rate);data[f*2+1]=0.2*Math.sin(2*Math.PI*(peer?2300:1100)*(n*120+f)/rate);}
        audio.peers[peer].playout.push(message(n),data);
      }
    }
    audio.process(null,left,right);
    let peak=0;for(let f=0;f<128;f++)peak=Math.max(peak,Math.abs(left[f]),Math.abs(right[f]));
    if(firstAudible===null&&peak>0.001)firstAudible=time*1000;
    if(time>2&&peak<0.00001)silent++;
  }
  const people=audio.peers.map((p,i)=>({sourcePpm:ppm[i],...p.playout.stats({})}));
  for(const p of people){assert.equal(p.underruns,0);assert.equal(p.lost,0);assert.ok(Math.abs(p.sourcePpm-p.driftPpm)<150,JSON.stringify(p));}
  assert.equal(silent,0);
  runs.push({wireRate:rate,deviceRate:48000,seconds:120,firstAudibleMs:firstAudible,silentBlocks:silent,people});
  }
  return {runs};
});
await test('peer gain changes preserve every upstream PCM sample', () => {
  let compared=0;
  for(const rate of [44100,48000]) {
    const a=new GuestAudio(1,rate,48000),b=new GuestAudio(1,rate,48000),captured=[[],[]];
    for(const [i,audio]of [a,b].entries()) {
      audio.port={postMessage(m){if(m.type==='capture'){captured[i].push(new Float32Array(m.buffer).slice());audio.message({type:'recycle',generation:1,pool:'capture',buffer:m.buffer});}}};
      audio.message({type:'configure',generation:1,wireRate:rate,deviceRate:48000,streamEpoch:1});
    }
    const input=new Float32Array(128),l=new Float32Array(128),r=new Float32Array(128);
    for(let block=0;block<750;block++) {
      for(let f=0;f<128;f++)input[f]=0.1*Math.sin(2*Math.PI*440*(block*128+f)/48000);
      if(block%250===0)b.setGain(1,[1,0.5,0][block/250]);
      a.process(input,l,r);b.process(input,l,r);
    }
    assert.equal(captured[0].length,captured[1].length);
    for(let p=0;p<captured[0].length;p++){assert.deepEqual(captured[0][p],captured[1][p]);compared+=120;}
  }
  return {comparedSamples:compared,unequalSamples:0};
});
await test('transferred buffer leases recycle without growing the pool', () => {
  const pool=new TransferPool(8,480,'capture',1),foreign=[];
  for(let i=0;i<8;i++){const slot=pool.take();slot.floats[0]=i;foreign.push(structuredClone(slot.message,{transfer:slot.transfer}));assert.equal(slot.buffer.byteLength,0);}
  assert.equal(pool.take(),null);
  for(let i=7;i>=0;i--){const returned=structuredClone(foreign[i],{transfer:[foreign[i].buffer]});assert.ok(pool.recycle(returned.buffer));}
  assert.equal(pool.slots.length,8);assert.ok(pool.slots.every(s=>!s.busy&&s.buffer.byteLength===480));
});
await test('capture pools bound stalls, sample clock never rewinds', () => {
  const audio=new GuestAudio(1,48000,48000), captured=[];
  audio.port={postMessage(m){if(m.type==='capture') captured.push({...m});}};
  audio.message({type:'configure',generation:1,wireRate:48000,deviceRate:48000,streamEpoch:1});
  const input=new Float32Array(128),l=new Float32Array(128),r=new Float32Array(128);
  for(let i=0;i<1000;i++) audio.process(input,l,r);
  assert.equal(captured.length,audioPackets(48000)); assert.ok(audio.captureDropped>500);
  const oldFrame=audio.frame;
  for(const m of captured) audio.message({type:'recycle',pool:'capture',generation:1,buffer:m.buffer});
  captured.length=0; audio.process(input,l,r);
  assert.ok(captured[0].firstFrame>=BigInt(oldFrame));
  // 10/28: a phone pausing 20 ms threw its own voice away - the page now keeps 400 ms (160 packets) on the way out.
  assert.ok(expired(0,401)); assert.ok(!expired(0,400)); assert.equal(audioPackets(48000),160); assert.equal(audioBudget(48000),46080);
  return {maxInFlight:audioPackets(48000),dropped:audio.captureDropped,resumedFrame:String(captured[0].firstFrame)};
});
await test('HTTP text/plain, stable signalId retry and accepted cursor switch', async () => {
  const calls=[];let failSignal=true; let s;
  const response=body=>({ok:true,status:200,json:async()=>({apiVersion:2,...body})});
  const fetchImpl=async function(url,options){
    // Like a browser's global fetch: called with this = another object it throws "Illegal invocation" (10/26 Chrome 156).
    if(this!==undefined&&this!==globalThis)throw new TypeError('Illegal invocation');
    const b=JSON.parse(options.body);calls.push({path:url.pathname,options,body:b});
    assert.equal(options.credentials,'omit');assert.deepEqual(options.headers,{'Content-Type':'text/plain'});
    assert.equal(b.apiVersion,2);
    if(url.pathname.endsWith('/request')) return response({requestToken:'ab'.repeat(32),cursor:'10',state:'pending'});
    if(url.pathname.endsWith('/events')) {
      if(b.requestToken)return response({cursor:'99',events:[{id:'11',type:'accepted',data:{connectionToken:'cd'.repeat(32),connectionGeneration:7}}]});
      assert.equal(b.cursor,'11');return response({cursor:'99',events:[{id:'12',type:'signal',data:{connectionGeneration:7,signal:{type:'offer'}}}]});
    }
    if(url.pathname.endsWith('/signal')) {if(failSignal){failSignal=false;throw new TypeError('lost response');}return response({signalId:b.signalId});}
    return response({ok:true});
  };
  s=new Signaling({apiBase:'http://127.0.0.1/api/',fetchImpl,onEvent:async e=>{if(e.type==='signal')s.auth=null;}});
  await s.join({code:'0042',inviteToken:'ef'.repeat(32),inviteGeneration:1},'web','01'.repeat(16),packet.CAPABILITIES);
  await s.poll();
  s.auth={connectionToken:'cd'.repeat(32),connectionGeneration:7};
  await s.send({type:'answer',sdp:'fixture'},7);
  const signals=calls.filter(c=>c.path.endsWith('/signal'));
  assert.equal(signals.length,2);assert.deepEqual(signals[0].body,signals[1].body);
  s.close();return {signalAttempts:2,signalId:signals[0].body.signalId,connectionCursor:'11'};
});
await test('worker validates roster before audio, duplicate ACK and generation', () => {
  const posts=[],audioMessages=[],wire=[];
  const worker=new GuestWorker(m=>posts.push(m));
  try {
    worker.configureConnection({participantId:2,streamEpoch:1,connectionGeneration:1,room:{session:'123456789abcdef0',wireRate:48000,roomMode:'v2',members}});
    worker.deviceRate=48000;
    worker.audio={postMessage(m){audioMessages.push(m);},close(){}};
    worker.channel={readyState:'open',bufferedAmount:0,send(b){wire.push(b.slice(0));},close(){}};
    const audio=packet.encodeAudio(header(),pcm,false);
    worker.receive(audio,0n); assert.equal(worker.metrics.received,0);
    const roster=packet.encodeRoster(header(),48000,members);
    worker.receive(roster,0n); worker.receive(roster,0n);
    assert.equal(audioMessages.filter(m=>m.type==='people').length,1); assert.equal(wire.length,2);
    worker.receive(audio,0n); assert.equal(worker.metrics.received,1);
    worker.message({type:'mic',generation:9,enabled:false}); assert.equal(worker.mic,true);
    assert.ok(normalChannelClose({error:{name:'OperationError',message:'User-Initiated Abort'}}));
    assert.ok(!normalChannelClose({error:{name:'OperationError',message:'SCTP failure'}}));
  } finally {worker.close();}
});
await test('main relay age/buffer limits recycle leases and normal abort ends cleanly', async () => {
  const realNow=Date.now;Date.now=()=>10000;
  const states=[],errors=[],sent=[];let workerPort;
  const client=createGuestClient({onState:s=>states.push(s),onError:e=>errors.push(e)});
  client.generation=1;client.wireRate=48000;
  client.worker={postMessage(m){if(m.type==='transportPort')workerPort=m.port;},terminate(){workerPort.close();}};
  const channel={label:'livemix-audio-v2',protocol:'livemix-pcm-v2',ordered:false,maxRetransmits:0,maxPacketLifeTime:null,negotiated:false,
    readyState:'open',bufferedAmount:0,send(b){sent.push(b.slice(0));},close(){this.readyState='closed';}};
  client.attachRelay(channel,1);
  const deliver=createdAtMs=>new Promise(resolve=>{
    workerPort.onmessage=e=>{if(e.data.type==='recycleTransport')resolve(e.data);};
    const buffer=new ArrayBuffer(288);workerPort.postMessage({type:'packet',generation:1,audio:true,createdAtMs,buffer},[buffer]);
  });
  try {
    let result=await deliver(Date.now()-1000);assert.equal(result.sent,false);assert.equal(result.buffer.byteLength,288);
    channel.bufferedAmount=audioBudget(48000);result=await deliver(Date.now());assert.equal(result.sent,false);
    channel.bufferedAmount=2304;result=await deliver(Date.now()-100);assert.equal(result.sent,true);   // 10/28: a 100 ms old packet behind 8 queued ones still goes
    sent.length=0;
    channel.bufferedAmount=0;result=await deliver(Date.now());assert.equal(result.sent,true);assert.equal(sent.length,1);
    channel.onerror({error:{name:'OperationError',message:'User-Initiated Abort'}});
    await client.finishTask;assert.equal(states.at(-1).state,'ended');assert.equal(errors.length,0);
  } finally {await client.dispose();workerPort?.close();Date.now=realNow;}
  return {oldAudioSent:0,overBudgetAudioSent:0,normalAudioSent:2,recycledBytes:4*288,normalAbortErrors:0};
});
await test('microphone permission precedes join and cleanup releases actual resources', async () => {
  const navigatorDescriptor=Object.getOwnPropertyDescriptor(globalThis,'navigator');
  const contextDescriptor=Object.getOwnPropertyDescriptor(globalThis,'AudioContext');
  let stopped=0,closed=0,constraints,requests=0;const errors=[];
  const track={readyState:'live',getSettings:()=>({channelCount:1,sampleRate:44100,echoCancellation:false,noiseSuppression:false,autoGainControl:false}),stop(){stopped++;this.readyState='ended';}};
  Object.defineProperty(globalThis,'navigator',{configurable:true,value:{mediaDevices:{
    async getUserMedia(c){constraints=c;return {getTracks:()=>[track],getAudioTracks:()=>[track]};},async enumerateDevices(){return [{kind:'audioinput',deviceId:'test'}];}
  }}});
  Object.defineProperty(globalThis,'AudioContext',{configurable:true,value:class {constructor(){this.sampleRate=48000;this.state='suspended';this.audioWorklet={addModule:async()=>{}};}async resume(){this.state='running';}async close(){closed++;this.state='closed';}}});
  const client=createGuestClient({onError:e=>errors.push(e)});
  try {
    await assert.rejects(client.requestJoin({},'web'),/microphone_required/);
    await client.prepareMicrophone('test');
    assert.equal(client.state,'ready');assert.equal(constraints.audio.channelCount,1);
    assert.equal(constraints.audio.echoCancellation,false);assert.equal(constraints.audio.noiseSuppression,false);assert.equal(constraints.audio.autoGainControl,false);
    assert.deepEqual(constraints.audio.deviceId,{exact:'test'});
    assert.equal(client.microphoneInfo.sampleRate,48000);assert.equal(client.microphoneInfo.trackSettings.sampleRate,44100);
    client.worker={postMessage(m){if(m.type==='join'||m.type==='leave'){requests++;const p=client.pending.get(m.id);client.pending.delete(m.id);p.resolve(null);}},terminate(){}};
    await client.requestJoin({},'web');assert.equal(requests,1);
    await client.dispose();assert.equal(stopped,1);assert.equal(closed,1);assert.equal(errors.length,0);
    navigator.mediaDevices.getUserMedia=async()=>{throw new DOMException('denied','NotAllowedError');};
    const denied=createGuestClient({onError:e=>errors.push(e)});
    await assert.rejects(denied.prepareMicrophone(),/microphone_denied/);
    assert.equal(denied.worker,null);assert.equal(denied.state,'error');
    assert.equal(errors.at(-1).code,'microphone_denied');await denied.dispose();
  } finally {
    await client.dispose();
    if(navigatorDescriptor)Object.defineProperty(globalThis,'navigator',navigatorDescriptor);else delete globalThis.navigator;
    if(contextDescriptor)Object.defineProperty(globalThis,'AudioContext',contextDescriptor);else delete globalThis.AudioContext;
  }
  return {tracksStopped:stopped,contextsClosed:closed,deviceRate:48000,trackRate:44100};
});

const summary={ok:results.every(r=>r.ok),passed:results.filter(r=>r.ok).length,total:results.length,results};
if(process.argv[2]) await writeFile(process.argv[2],JSON.stringify(summary,null,2));
console.log(JSON.stringify({ok:summary.ok,passed:summary.passed,total:summary.total}));
process.exitCode=summary.ok?0:1;
