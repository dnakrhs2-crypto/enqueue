import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import { Signaling } from '../../../livemix/webjoin/signaling.js';
import { CAPABILITIES } from '../../../livemix/webjoin/audio/packet.js';
const base=process.argv[2],output=process.argv[3];
const calls=[];
async function post(path,body,origin=null) {
  const r=await fetch(base+path,{method:'POST',headers:{'Content-Type':'text/plain',...(origin?{Origin:origin}:{})},body:JSON.stringify({apiVersion:2,...body})});
  const data=await r.json();calls.push({path,status:r.status});
  return {r,data};
}
const unwrap=async(path,body)=>(await post(path,body)).data;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(predicate){for(let n=0;n<200;n++){if(predicate())return;await sleep(25);}throw new Error('timeout');}
let room;const clients=[];
const result={ok:false};
try {
  room=await unwrap('create',{nick:'R6 host',inst:'r6-http-host',port:38471,appVersion:'0.17.0',protocols:[1,2],capabilities:CAPABILITIES,connectionId:'1a'.repeat(16),wireRate:48000});
  assert.equal(room.apiVersion,2);
  await unwrap('confirm',{code:room.code,hostToken:room.hostToken,reachable:false});
  const invited=await unwrap('web/invite',{code:room.code,hostToken:room.hostToken});
  const invite={code:room.code,inviteToken:invited.inviteToken,inviteGeneration:invited.inviteGeneration};
  const allowed=await post('web/request',{...invite,action:'inspect'},'https://dnakrhs2-crypto.github.io');
  assert.equal(allowed.r.headers.get('access-control-allow-origin'),'https://dnakrhs2-crypto.github.io');
  const other=await post('web/request',{...invite,action:'inspect'},'http://127.0.0.1');
  assert.equal(other.r.headers.get('access-control-allow-origin'),null);
  let accepted,offer,ended;
  const events=[];
  const client=new Signaling({apiBase:base,onEvent:e=>{events.push(e.type);if(e.type==='accepted')accepted=e.data;if(e.type==='signal')offer=e.data.signal;if(e.type==='ended')ended=e.data.reason;},onError:code=>{if(!['expired','closed'].includes(code))throw new Error(code);}});clients.push(client);
  assert.equal((await client.inspect(invite)).hostNick,'R6 host');
  const before=await unwrap('web/events',{code:room.code,hostToken:room.hostToken,cursor:'0'});
  assert.equal(before.events[0].data.pendingWebRequests.length,0);
  const request=await client.join(invite,'R6 web','2b'.repeat(16),CAPABILITIES);
  client.startPolling();
  const acceptedHost=await unwrap('web/accept',{code:room.code,hostToken:room.hostToken,requestId:request.requestId});
  assert.equal(acceptedHost.state,'accepted');
  const auth={code:room.code,connectionToken:acceptedHost.connectionToken,connectionGeneration:acceptedHost.connectionGeneration};
  await unwrap('web/signal',{...auth,signalId:1,signal:{type:'offer',sdp:'R6 HTTP fixture'}});
  await until(()=>accepted&&offer);
  assert.notEqual(accepted.connectionToken,acceptedHost.connectionToken);
  assert.equal(offer.sdp,'R6 HTTP fixture');
  await client.send({type:'answer',sdp:'R6 HTTP answer'},accepted.connectionGeneration);
  const received=await unwrap('web/events',{...auth,cursor:'0'});
  assert.equal(received.events.filter(e=>e.type==='signal')[0].data.signal.sdp,'R6 HTTP answer');
  await unwrap('web/signal',{...auth,signalId:2,signal:{type:'connected'}});
  await client.leave();
  const after=await unwrap('web/events',{code:room.code,hostToken:room.hostToken,cursor:'0'});
  assert.equal(after.events[0].data.room.members.length,1);
  const cancel=new Signaling({apiBase:base});clients.push(cancel);
  const pending=await cancel.join(invite,'cancel','3c'.repeat(16),CAPABILITIES);await cancel.leave();
  const bad=await post('web/events',{code:room.code,requestToken:pending.requestToken,cursor:pending.cursor});
  assert.equal(bad.r.status,410);
  const reject=new Signaling({apiBase:base,onEvent:e=>{if(e.type==='ended')ended=e.data.reason;}});clients.push(reject);
  const rejected=await reject.join(invite,'reject','4d'.repeat(16),CAPABILITIES);reject.startPolling();
  await unwrap('web/reject',{code:room.code,hostToken:room.hostToken,requestId:rejected.requestId});
  await until(()=>ended==='rejected');
  result.ok=true;result.requests=calls.length;result.events=events;result.cors={allowed:true,otherOrigin:false};result.pendingBeforeJoin=0;result.membersAfterLeave=1;
} catch(error) {result.error=error.stack;}
finally {for(const c of clients)c.close();if(room?.hostToken)await unwrap('close',{code:room.code,hostToken:room.hostToken});}
if(output)await writeFile(output,JSON.stringify(result,null,2));
console.log(JSON.stringify(result,null,2));process.exitCode=result.ok?0:1;
