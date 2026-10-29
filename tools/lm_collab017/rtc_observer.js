// Test-page init script. Never modifies the shipped guest client or logs ICE URLs.
(() => {
  const Original = window.RTCPeerConnection;
  const peers = [], events = [];
  window.RTCPeerConnection = class extends Original {
    constructor(config, constraints) {
      super(window.__soakForceRelay ? {...config, iceTransportPolicy: 'relay'} : config, constraints);
      peers.push(this);
      for (const event of ['connectionstatechange', 'iceconnectionstatechange', 'icegatheringstatechange']) {
        this.addEventListener(event, () => events.push({atMs: performance.now(), event,
          pc: peers.indexOf(this), state: this.connectionState, iceState: this.iceConnectionState,
          gatheringState: this.iceGatheringState}));
      }
    }
  };
  const candidate = c => c ? Object.fromEntries(['candidateType', 'protocol', 'relayProtocol']
    .filter(k => c[k] !== undefined).map(k => [k, c[k]])) : null;
  window.__soakRTC = async () => {
    const results = [];
    for (const [index, pc] of peers.entries()) {
      const stats = await pc.getStats();
      let pair;
      for (const s of stats.values()) if (s.type === 'transport' && s.selectedCandidatePairId)
        pair = stats.get(s.selectedCandidatePairId);
      if (!pair) for (const s of stats.values())
        if (s.type === 'candidate-pair' && s.state === 'succeeded' && s.nominated) pair = s;
      const channels = [...stats.values()].filter(s => s.type === 'data-channel').map(s => ({
        label: s.label, state: s.state, messagesSent: s.messagesSent, messagesReceived: s.messagesReceived,
        bytesSent: s.bytesSent, bytesReceived: s.bytesReceived}));
      results.push({pc: index, state: pc.connectionState, iceState: pc.iceConnectionState,
        selectedPair: pair ? {local: candidate(stats.get(pair.localCandidateId)),
          remote: candidate(stats.get(pair.remoteCandidateId)), currentRoundTripTime: pair.currentRoundTripTime,
          bytesSent: pair.bytesSent, bytesReceived: pair.bytesReceived} : null, dataChannels: channels});
    }
    return {peers: results, events};
  };
})();
