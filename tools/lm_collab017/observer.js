// Test-only downstream tap. All hardware-bound output is zero.
class R9Capture extends AudioWorkletProcessor {
  constructor() {
    super(); this.frame=0; this.pcm=new Float32Array(8192); this.used=0;
    this.port.onmessage=e=>{
      if(e.data.type==='mark')this.port.postMessage({type:'mark',id:e.data.id,frame:this.frame});
      if(e.data.type==='flush') {this.flush();this.port.postMessage({type:'flushed',frame:this.frame});}
    };
  }
  flush() {
    if(!this.used)return;
    const buffer=this.pcm.slice(0,this.used).buffer;
    this.port.postMessage({type:'pcm',buffer},[buffer]);this.used=0;
  }
  process(inputs,outputs) {
    const a=inputs[0];const n=outputs[0][0].length;
    for(let i=0;i<n;i++) {
      this.pcm[this.used++]=a[0]?.[i]??0;this.pcm[this.used++]=a[1]?.[i]??0;
      if(this.used===this.pcm.length)this.flush();
    }
    for(const ch of outputs[0])ch.fill(0);
    this.frame+=n;return true;
  }
}
registerProcessor('r9-capture',R9Capture);
