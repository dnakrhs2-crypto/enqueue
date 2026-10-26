import {createGuestClient,parseInvite} from '../../livemix/webjoin/guest-client.js';
let client,tap,context,chunks=[],pending=new Map(),nextId=0,flushed;
let evidence={states:[],rosters:[],errors:[],diagnostics:[]};
async function start(config) {
  chunks=[];evidence={states:[],rosters:[],errors:[],diagnostics:[],voice:3};
  const started=performance.now();
  // Load the tap before the public client creates its node (onAudioNode is synchronous).
  // prepareMicrophone creates the actual client AudioContext. This is R6's testing seam.
  client=createGuestClient({
    onState:s=>{evidence.states.push(s);if(s.state==='connected'&&!evidence.connectMs)evidence.connectMs=performance.now()-started;},
    onRoster:r=>evidence.rosters.push(r), onError:e=>evidence.errors.push(e),
    testing:{apiBase:config.apiBase,...(config.loopback?{iceServers:[]}:{}),
      onDiagnostics:d=>{evidence.diagnostics.push(d);if(evidence.diagnostics.length>100)evidence.diagnostics.shift();},
      onAudioNode:(node,ctx)=>{
        context=ctx;node.disconnect();
        tap=new AudioWorkletNode(ctx,'r9-capture',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2],
          channelCount:2,channelCountMode:'explicit',channelInterpretation:'discrete'});
        tap.port.onmessage=e=>{const m=e.data;
          if(m.type==='pcm')chunks.push(new Float32Array(m.buffer));
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
function mark(){return new Promise(resolve=>{pending.set(++nextId,resolve);tap.port.postMessage({type:'mark',id:nextId});});}
async function stop() {
  const frame=await new Promise(resolve=>{flushed=resolve;tap.port.postMessage({type:'flush'});});
  tap.disconnect();const copy=chunks;chunks=[];
  await client.leave();await client.dispose();
  const pcm=new Float32Array(copy.reduce((n,a)=>n+a.length,0));let p=0;
  for(const a of copy){pcm.set(a,p);p+=a.length;}
  const response=await fetch('/capture',{method:'POST',body:pcm.buffer});
  if(!response.ok)throw new Error('capture write failed');
  evidence.captureFrames=pcm.length/2;evidence.lastFrame=frame;return evidence;
}
window.r9={start,mark,stop,evidence:()=>evidence,gain:(person,gain)=>client.setPeerVolume(person,gain)};
