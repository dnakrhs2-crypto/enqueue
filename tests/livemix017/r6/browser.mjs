import { createGuestClient } from '../../../livemix/webjoin/guest-client.js';
import { encodeAudio, encodeRoster, readPacket, decodeAudio, Kind, CAPABILITIES } from '../../../livemix/webjoin/audio/packet.js';

const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const check=(condition,label)=>{if(!condition)throw new Error(label);};
function rpcWorker(worker) {
  const pending=new Map();let id=0;
  worker.addEventListener('message',e=>{const request=pending.get(e.data.id);if(request){pending.delete(e.data.id);request(e.data.result);}});
  return (type,fields={})=>new Promise(resolve=>{pending.set(++id,resolve);worker.postMessage({type,id,...fields});});
}
function observe(context,node) {
  const observer=new AudioWorkletNode(context,'r6-observer',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2],channelCount:2,channelCountMode:'explicit',channelInterpretation:'discrete'});
  node.connect(observer);observer.connect(context.destination);
  return {observer,measure(seconds){return new Promise(resolve=>{observer.port.onmessage=e=>resolve({...e.data,pcm:new Float32Array(e.data.buffer)});observer.port.postMessage({type:'measure',seconds});});}};
}
function amplitude(pcm,channel,center,rate) {
  let maximum=0;
  // Windowed tone search tolerates the injected clock drift and device timing. Flat-top window (SRS coefficients):
  // under 0.1 % amplitude error when the drifted tone falls between the 0.5 Hz search steps, where a Hann window
  // loses up to 15 % and made the per-person volume ratios flaky (Edge 156, 10/26).
  const frames=pcm.length/2,c=[0.21557895,0.41663158,0.277263158,0.083578947,0.006947368];
  for(let frequency=center-3;frequency<=center+3;frequency+=0.5) {
    let x=0,y=0,weight=0;
    for(let i=0;i<frames;i++) {const p=2*Math.PI*i/(frames-1),w=c[0]-c[1]*Math.cos(p)+c[2]*Math.cos(2*p)-c[3]*Math.cos(3*p)+c[4]*Math.cos(4*p),a=2*Math.PI*frequency*i/rate;
      x+=pcm[i*2+channel]*w*Math.cos(a);y+=pcm[i*2+channel]*w*Math.sin(a);weight+=w;}
    maximum=Math.max(maximum,2*Math.hypot(x,y)/weight);
  }
  return maximum;
}
function analyze(result) {
  const {pcm,rate,...metadata}=result;delete metadata.buffer;
  return {...metadata,a:amplitude(pcm,0,700,rate),b:amplitude(pcm,0,1700,rate),
    rightA:amplitude(pcm,1,1100,rate),rightB:amplitude(pcm,1,2300,rate),cross:amplitude(pcm,1,700,rate)};
}
async function audioTest({rate=48000,driftSeconds=120}={}) {
  const evidence={rate,ok:false};window.r6Evidence=evidence;
  const stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:false,noiseSuppression:false,autoGainControl:false},video:false});
  const context=new AudioContext({latencyHint:'interactive'});
  const host=new Worker(new URL('./audio-host.js',import.meta.url),{type:'module'});const rpc=rpcWorker(host);
  let node,observer;
  try {
    evidence.trackSettings=stream.getAudioTracks()[0].getSettings();evidence.deviceRate=context.sampleRate;
    await context.audioWorklet.addModule(new URL('../../../livemix/webjoin/audio/worklet.js',import.meta.url));
    await context.audioWorklet.addModule(new URL('./observer.js',import.meta.url));
    node=new AudioWorkletNode(context,'livemix-guest',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2],channelCount:2,channelCountMode:'explicit',channelInterpretation:'discrete',processorOptions:{generation:1,wireRate:rate}});
    const port=new MessageChannel();
    node.port.postMessage({type:'audioPort',generation:1,wireRate:rate,deviceRate:context.sampleRate,port:port.port1},[port.port1]);
    context.createMediaStreamSource(stream).connect(node);const monitor=observe(context,node);observer=monitor.observer;
    await context.resume();
    const startup=monitor.measure(1);
    host.postMessage({type:'start',rate,deviceRate:context.sampleRate,port:port.port2},[port.port2]);
    const initial=await startup;evidence.startupMs=1000*(initial.firstAudible-initial.start);
    await sleep(Math.max(0,driftSeconds-1)*1000);
    evidence.drift=await rpc('status');
    const measure=async()=>{await rpc('window');const audio=await monitor.measure(2);return {output:analyze(audio),upstream:await rpc('window')};};
    evidence.full=await measure();
    node.port.postMessage({type:'peerGain',generation:1,participantId:1,gain:0.5});await sleep(1000);evidence.half=await measure();
    node.port.postMessage({type:'peerGain',generation:1,participantId:1,gain:0});await sleep(1000);evidence.zero=await measure();
    const {full,half,zero}=evidence;
    check(full.output.a>0.15 && full.output.b>0.15 && full.output.rightA>0.15 && full.output.rightB>0.15,'both origins stereo');
    check(full.output.cross/full.output.a<0.01,'LR separation');
    check(Math.abs(half.output.a/full.output.a-0.5)<0.06 && zero.output.a<0.003,'per-person volume');
    check(Math.abs(half.output.b/full.output.b-1)<0.06 && Math.abs(zero.output.b/full.output.b-1)<0.06,'other person unchanged');
    check(full.upstream.rms>0.02 && Math.abs(half.upstream.rms/full.upstream.rms-1)<0.05 && Math.abs(zero.upstream.rms/full.upstream.rms-1)<0.05,'upstream unchanged');
    host.postMessage({type:'mic',enabled:false});await sleep(1000);evidence.muted=await measure();
    check(evidence.muted.upstream.max===0 && evidence.muted.upstream.packets>100,'mute clock and zero');
    host.postMessage({type:'mic',enabled:true});
    node.port.postMessage({type:'peerGain',generation:1,participantId:1,gain:1});await sleep(1000);
    const before=await rpc('status');await rpc('remove');await sleep(1000);evidence.removed=await measure();const after=await rpc('status');
    check(evidence.removed.output.a>0.15 && evidence.removed.output.b<0.003,'roster removal');
    const peer=s=>s.metrics.audio.people.find(p=>p.participantId===1);
    check(peer(before).underruns===peer(after).underruns,'remaining peer continuity');
    // Both audio threads keep running while the window thread is occupied for 2 s.
    const busyMeasure=monitor.measure(3);const end=performance.now()+2000;while(performance.now()<end){}
    evidence.mainBusy=analyze(await busyMeasure);
    check(evidence.mainBusy.longestSilenceMs===0,'window load interrupted direct audio ports');
    check([full,half,zero].every(v=>v.output.longestSilenceMs===0),'steady output dropout');
    evidence.final=await rpc('status');
    const drift=evidence.drift.metrics.audio.people.filter(p=>p.participantId);
    check(drift.length===2 && drift[0].driftPpm>100 && drift[1].driftPpm< -50,'independent drift direction');
    evidence.ok=true;return evidence;
  } finally {host.postMessage({type:'close'});host.terminate();node?.disconnect();observer?.disconnect();stream.getTracks().forEach(t=>t.stop());await context.close();}
}

// Full public client API against real R2 HTTP and a browser host PeerConnection.
// The runner supplies only a local API base and the allowed host management token.
async function connectionTest(config) {
  const evidence={states:[],errors:[],microphoneErrors:[],diagnostics:[],rosters:[],ok:false};window.r6Evidence=evidence;
  const post=async(path,body)=>{const r=await fetch(config.apiBase+path,{method:'POST',headers:{'Content-Type':'text/plain'},credentials:'omit',body:JSON.stringify({apiVersion:2,code:config.code,...body})});const b=await r.json();if(!r.ok)throw new Error(b.error);return b;};
  const pc=new RTCPeerConnection({iceServers:[]});const channel=pc.createDataChannel('livemix-audio-v2',{protocol:'livemix-pcm-v2',ordered:false,maxRetransmits:0});
  let accepted,token,generation,signalId=0,hostCursor='0',running=true,timer,rosterTimer,captures=0,stereoSent=0,ack=false,seq=0;
  let signalTail=Promise.resolve();const candidateQueue=[];
  let captureWindow=null,lastMeter=0;
  const decodedCapture=new Float32Array(240);
  const send=signal=>{signalTail=signalTail.then(()=>post('web/signal',{connectionToken:token,connectionGeneration:generation,signalId:++signalId,signal}));return signalTail;};
  pc.onicecandidate=e=>{const signal=e.candidate?{type:'candidate',candidate:e.candidate.candidate,sdpMid:e.candidate.sdpMid,sdpMLineIndex:e.candidate.sdpMLineIndex}:{type:'endOfCandidates'};if(token)void send(signal);else candidateQueue.push(signal);};
  const client=createGuestClient({onState:s=>evidence.states.push(s),onError:e=>evidence.errors.push(e),onRoster:r=>evidence.rosters.push(r),
    onMicrophoneError:e=>evidence.microphoneErrors.push(e),onMeter:m=>{lastMeter=m.peak;},
    testing:{apiBase:config.apiBase,iceServers:[],forceRelay:config.forceRelay,onDiagnostics:d=>evidence.diagnostics.push(d)}});
  const deadline=async(predicate,seconds=15)=>{const stop=performance.now()+seconds*1000;while(!predicate()){if(performance.now()>stop)throw new Error('timeout');if(evidence.errors.length)throw new Error(evidence.errors[0].code);await sleep(25);}};
  let poll;
  try {
    evidence.inspect=await client.inspect(config.invite);await client.prepareMicrophone();client.setPeerVolume(1,0.5);
    await client.requestJoin(config.invite,'R6 browser');
    const snapshot=await post('web/events',{hostToken:config.hostToken,cursor:'0'});
    const request=snapshot.events[0].data.pendingWebRequests.find(r=>r.nick==='R6 browser');check(request,'pending after microphone');
    accepted=await post('web/accept',{hostToken:config.hostToken,requestId:request.requestId});token=accepted.connectionToken;generation=accepted.connectionGeneration;
    poll=(async()=>{while(running){const response=await post('web/events',{connectionToken:token,connectionGeneration:generation,cursor:hostCursor});for(const event of response.events){if(event.type!=='signal')continue;const s=event.data.signal;
      if(s.type==='answer'){await pc.setRemoteDescription(s);for(const c of remoteCandidates)await pc.addIceCandidate(c);remoteCandidates.length=0;}
      else if(s.type==='candidate' || s.type==='endOfCandidates'){const c=s.type==='candidate'?{candidate:s.candidate,sdpMid:s.sdpMid,sdpMLineIndex:s.sdpMLineIndex}:null;if(pc.remoteDescription)await pc.addIceCandidate(c);else remoteCandidates.push(c);}}
      hostCursor=response.cursor;}})().catch(e=>{if(running)evidence.errors.push({code:e.message});});
    const remoteCandidates=[];
    channel.binaryType='arraybuffer';channel.onmessage=e=>{
      const h=readPacket(e.data,accepted.room.session);
      if(h.kind===Kind.rosterAck){if(!ack){ack=true;void send({type:'connected'});}}
      else if(h.kind===Kind.audio){
        check(h.flags===2 && h.frames===120,'mono capture packet');captures++;
        if(captureWindow){
          decodeAudio(h,decodedCapture);
          const w=captureWindow,now=performance.now();let peak=0;
          for(const value of decodedCapture){w.energy+=value*value;peak=Math.max(peak,Math.abs(value));}
          w.samples+=decodedCapture.length;w.packets++;w.max=Math.max(w.max,peak);
          if(peak<0.0001)w.silentPackets++;
          w.longestGapMs=Math.max(w.longestGapMs,now-w.lastAt);w.lastAt=now;
        }
      }
    };
    const h={session:accepted.room.session,origin:1,linkSender:1,seq:0,firstFrame:0n,streamEpoch:1,rosterRevision:1};
    const rosterMembers=accepted.room.members.map(m=>({...m,capabilities:31,deviceRate:accepted.room.wireRate}));
    const sendRoster=()=>{if(channel.readyState==='open')channel.send(encodeRoster(h,accepted.room.wireRate,rosterMembers));};
    channel.onopen=()=>{sendRoster();rosterTimer=setInterval(()=>{if(!ack)sendRoster();},200);const data=new Float32Array(240);timer=setInterval(()=>{if(!ack||channel.readyState!=='open')return;for(let f=0;f<120;f++){data[f*2]=0.15*Math.sin(2*Math.PI*700*(seq*120+f)/accepted.room.wireRate);data[f*2+1]=0.15*Math.sin(2*Math.PI*1100*(seq*120+f)/accepted.room.wireRate);}channel.send(encodeAudio({...h,seq,firstFrame:BigInt(seq++)*120n},data,false));stereoSent++;},3);};
    await pc.setLocalDescription(await pc.createOffer());await send({type:'offer',sdp:pc.localDescription.sdp});for(const s of candidateQueue)await send(s);
    await deadline(()=>evidence.states.some(s=>s.state==='connected')&&captures>100&&stereoSent>100);
    await sleep(2000);
    if(config.microphoneSwitch){
      const resources={context:client.context,node:client.node,worker:client.worker,pc:client.pc,channel:client.channel};
      const generation=client.generation,gains=JSON.stringify(client.gains),originalId=client.microphoneId;
      const inputs=await client.listMicrophones();
      const nextId=inputs.find(d=>d.deviceId!==originalId)?.deviceId || (originalId!=='default'?'default':null);
      check(nextId,'a second input id or the default alias is required');
      const steady=()=>{
        check(client.state==='connected' && client.generation===generation,'call remains connected');
        for(const [name,resource] of Object.entries(resources))check(client[name]===resource,name+' replaced during mic switch');
        check(JSON.stringify(client.gains)===gains,'per-person gains changed');
        check(evidence.errors.length===0,'microphone caused a fatal error');
      };
      const measure=async silent=>{
        // Let only the switch boundary settle, then inspect the actual PCM arriving at the other peer.
        await sleep(500);
        captureWindow={packets:0,samples:0,energy:0,max:0,silentPackets:0,longestGapMs:0,lastAt:performance.now()};
        await sleep(1000);
        const w=captureWindow;captureWindow=null;
        w.longestGapMs=Math.max(w.longestGapMs,performance.now()-w.lastAt);
        const result={packets:w.packets,rms:Math.sqrt(w.energy/Math.max(1,w.samples)),max:w.max,
          silentPackets:w.silentPackets,longestGapMs:w.longestGapMs};
        check(result.packets>100 && result.longestGapMs<250,'capture stopped after switch');
        if(silent)check(result.max===0,'failed or muted microphone is not silent');
        else check(result.rms>0.02 && result.silentPackets===0,'steady microphone audio dropped out');
        steady();return result;
      };
      const switches={originalId,nextId};evidence.microphoneSwitch=switches;
      switches.before=await measure(false);
      const oldTrack=client.stream.getAudioTracks()[0];
      await client.switchMicrophone(nextId);
      check(oldTrack.readyState==='ended' && oldTrack.onended===null,'old track not released');
      check(client.microphoneInfo.trackSettings.deviceId===client.stream.getAudioTracks()[0].getSettings().deviceId,'stale diagnostics');
      switches.after=await measure(false);
      client.setMicEnabled(false);await client.switchMicrophone(originalId);
      check(!client.micEnabled,'switch unmuted microphone');switches.muted=await measure(true);
      client.setMicEnabled(true);switches.unmuted=await measure(false);
      const failed=await client.switchMicrophone('r6-missing-microphone').then(()=>null,e=>e.code);
      check(['microphone_missing','microphone_constraints'].includes(failed),'missing input must fail');
      check(client.stream===null && client.source===null,'failed input remains attached');
      switches.failed=await measure(true);check(lastMeter===0,'failed input meter not zero');
      await client.switchMicrophone(nextId);switches.recovered=await measure(false);
      // track.stop() does not emit ended itself; simulate unplugging on the actual capture track.
      const ended=client.stream.getAudioTracks()[0];ended.stop();ended.dispatchEvent(new Event('ended'));
      check(evidence.microphoneErrors.at(-1)?.code==='microphone_ended','ended not reported to the picker');
      switches.ended=await measure(true);check(lastMeter===0,'ended input meter not zero');
      await client.switchMicrophone(originalId);switches.afterEnded=await measure(false);
      switches.ok=true;
    }
    if(config.forceRelay) {
      const end=performance.now()+2000;while(performance.now()<end){}
      await sleep(2000);
      const metrics=evidence.diagnostics.filter(d=>d.type==='metrics').at(-1);
      check(metrics.transport.dropped>0,'relay must discard old audio after window stall');
      evidence.relayStallDrops=metrics.transport.dropped;
    }
    evidence.captures=captures;evidence.stereoSent=stereoSent;evidence.path=evidence.diagnostics.find(d=>d.type==='transport');
    check(evidence.path && (config.forceRelay?evidence.path.path==='main':true),'channel path');
    running=false;
    if(config.remoteClose) {pc.close();await deadline(()=>evidence.states.some(s=>s.state==='ended'));}
    else await client.leave();
    check(evidence.errors.length===0,'normal close errors');evidence.ok=true;return evidence;
  } finally {running=false;clearInterval(timer);clearInterval(rosterTimer);pc.close();await client.dispose();if(poll)await Promise.race([poll,sleep(100)]);}
}
window.r6={audioTest,connectionTest};
