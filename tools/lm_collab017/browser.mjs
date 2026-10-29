import {createGuestClient,parseInvite} from '../../livemix/webjoin/guest-client.js';
let client,tap,context,chunks=[],pending=new Map(),nextId=0,flushed;
let evidence={states:[],rosters:[],errors:[],diagnostics:[]};
let upload=Promise.resolve(),queuedBytes=0,uploadedBytes=0,bufferedBytes=0,uploadError=null;
let captureFinished=false;
function uploadChunk() {
  if(!chunks.length)return;
  const body=new Blob(chunks), offset=queuedBytes;
  chunks=[];bufferedBytes=0;queuedBytes+=body.size;
  upload=upload.then(async()=>{
    if(uploadError)return;
    try {
      const response=await fetch(`/capture?offset=${offset}`,{method:'POST',body});
      if(!response.ok)throw new Error(`capture HTTP ${response.status}`);
      uploadedBytes+=body.size;
    } catch(error) { uploadError=String(error); }
  });
}
async function start(config) {
  chunks=[];evidence={states:[],rosters:[],errors:[],diagnostics:[],voice:3};
  upload=Promise.resolve();queuedBytes=uploadedBytes=bufferedBytes=0;uploadError=null;
  captureFinished=false;
  const started=performance.now();
  // Load the tap before the public client creates its node (onAudioNode is synchronous).
  // prepareMicrophone creates the actual client AudioContext. This is R6's testing seam.
  client=createGuestClient({
    onState:s=>{evidence.states.push({...s,atMs:performance.now()});if(s.state==='connected'&&!evidence.connectMs)evidence.connectMs=performance.now()-started;},
    onRoster:r=>evidence.rosters.push({...r,atMs:performance.now()}), onError:e=>evidence.errors.push(e),
    testing:{apiBase:config.apiBase,...(config.loopback?{iceServers:[]}:{}),
      onDiagnostics:d=>{evidence.diagnostics.push(d);if(evidence.diagnostics.length>100)evidence.diagnostics.shift();},
      onAudioNode:(node,ctx)=>{
        context=ctx;node.disconnect();
        tap=new AudioWorkletNode(ctx,'r9-capture',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2],
          channelCount:2,channelCountMode:'explicit',channelInterpretation:'discrete'});
        tap.port.onmessage=e=>{const m=e.data;
          if(m.type==='pcm') {
            chunks.push(new Float32Array(m.buffer));bufferedBytes+=m.buffer.byteLength;
            if(bufferedBytes>=1024*1024)uploadChunk();
          }
          else if(m.type==='mark'){pending.get(m.id)?.(m.frame);pending.delete(m.id);}
          else if(m.type==='flushed')flushed?.(m.frame);
        };
        node.connect(tap);tap.connect(ctx.destination);
        if(config.reflect) {
          // Explicit laboratory acoustic loop, one render quantum of delay, still no speaker.
          const delay=ctx.createDelay(1);delay.delayTime.value=128/ctx.sampleRate;
          node.connect(delay);delay.connect(node);evidence.reflectDelayFrames=128;
        }
        evidence.rate=ctx.sampleRate;
      }
    }});
  const invite=parseInvite(config.invite.split('#')[1]);
  await client.inspect(invite);await client.prepareMicrophone();
  await client.context.audioWorklet.addModule(new URL('./observer.js',import.meta.url));
  await client.requestJoin(invite,'R9-Web');
  return true;
}
function bounded(promise,label){return new Promise((resolve,reject)=>{
  const timer=setTimeout(()=>reject(new Error(label+' timed out')),5000);
  promise.then(v=>{clearTimeout(timer);resolve(v);},e=>{clearTimeout(timer);reject(e);});
});}
function mark(){return bounded(new Promise(resolve=>{pending.set(++nextId,resolve);tap.port.postMessage({type:'mark',id:nextId});}),'capture mark');}
async function finishCapture() {
  if(captureFinished||!tap)return evidence;
  const frame=await bounded(new Promise(resolve=>{flushed=resolve;tap.port.postMessage({type:'flush'});}), 'capture flush');
  tap.disconnect();uploadChunk();
  evidence.rtc=await window.__soakRTC?.();
  await upload;
  if(uploadError)throw new Error(uploadError);
  evidence.captureFrames=uploadedBytes/8;evidence.captureBytes=uploadedBytes;evidence.lastFrame=frame;
  evidence.captureComplete=evidence.captureFrames===frame;
  captureFinished=true;
  return evidence;
}
async function stop() {
  await finishCapture();
  await client?.leave();await client?.dispose();
  return evidence;
}
async function snapshot(detailed) {
  return {state:evidence.states.at(-1)?.state,session:evidence.rosters.at(-1),connectMs:evidence.connectMs,
    diagnostics:evidence.diagnostics.at(-1),errors:evidence.errors,states:evidence.states,
    ...(detailed?{rtc:await window.__soakRTC?.()}:{}),captureBytes:uploadedBytes,uploadError};
}
window.r9={start,mark,stop,finishCapture,snapshot,evidence:()=>evidence,gain:(person,gain)=>client.setPeerVolume(person,gain)};
