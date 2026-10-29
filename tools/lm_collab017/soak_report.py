"""Numeric soak summaries. Missing measurements stay null, never zero."""
import math
from pathlib import Path
import statistics
from telemetry import read_json, write_json

VOICES={'0':'방장','1':'LiveMix 1','2':'LiveMix 2','3':'웹'}


def trend(values):
    values=[v for v in values if isinstance(v,(int,float)) and math.isfinite(v)]
    return {'min':min(values),'median':statistics.median(values),'max':max(values),'samples':len(values)} if values else None


def summarize(folder, spec, network):
    result=read_json(folder/'result.json');timeline=read_json(folder/'timeline.json')
    raw=result.get('endpoint') or next((value for name in ('host','native','web')
                                      if (value:=read_json(folder/name/'result.json'))),{})
    row={'endpoint':f'{spec["role"]}@{spec["pc"]}','network':network,'status':result.get('status','missing'),
         'error':result.get('error'),'connectMs':raw.get('connectMs'),
         'cpu':result.get('cpu'),'continuity':[],'roundtrip':result.get('roundtrip'),
         'recordings':result.get('recordings',[]),'changes':timeline.get('changes',[]),'actions':timeline.get('actions',[]),
         'paths':[],'peerTrends':{},'cpuTrend':None,'resultFile':str(folder/'result.json')}
    segments=result.get('segments') or ([result] if result.get('monitor') else [])
    for index,segment in enumerate(segments):
        if row['connectMs'] is None:row['connectMs']=segment.get('endpoint',{}).get('connectMs')
        streams=[('monitor',segment.get('monitor',{}))]+[(f'ring{r["slot"]}',r) for r in segment.get('rings',[])]
        for stream,measured in streams:
            for voice,c in measured.get('continuity',{}).items():
                row['continuity'].append({'segment':index,'stream':stream,'voice':voice,'source':VOICES.get(voice,voice),**c,
                    'includesPlannedAbsence':voice=='3' and bool(segment.get('plannedWebAbsence'))})
    peers={};cpu=[];previous=None;disconnected=False;disconnects=reconnects=0
    for change in row['changes']:
        state=change.get('state');connected=state in (5,'connected')
        measuring=change.get('phase') in ('measuring','measured','absent','leaving')
        if measuring and previous is True and not connected:disconnects+=1;disconnected=True
        if measuring and connected and previous is False and disconnected:reconnects+=1
        previous=connected
    row.update(disconnects=disconnects,reconnects=reconnects)
    for sample in timeline.get('samples',[]):
        if sample.get('phase') not in ('measuring','measured','absent'):continue
        for person in (sample.get('session') or {}).get('people',[]):
            if person.get('self'):continue
            name=person.get('nick',str(person.get('id')))
            metrics=peers.setdefault(name,{})
            for key in ('rttMs','estimatedOneWayMs','bufferMs','lossPercent'):
                metrics.setdefault(key,[]).append(person.get(key))
        diagnostics=sample.get('diagnostics') or {}
        # Preserve all public per-person audio counters, even when the browser
        # does not expose RTT/loss as a percentage for individual participants.
        roster={m.get('participantId'):m.get('nick') for m in (sample.get('session') or {}).get('members',[])}
        for person in diagnostics.get('audio',{}).get('people',[]):
            pid=person.get('participantId')
            if not pid:continue
            name=roster.get(pid) or str(pid)
            metrics=peers.setdefault(name,{})
            for key,value in person.items():
                if key not in ('id','participantId') and isinstance(value,(int,float)):
                    metrics.setdefault(key,[]).append(value)
            # Browser per-person stats expose cumulative lost/received counters,
            # not the native ten-second loss metric. Label this formula explicitly.
            lost,received=person.get('lost'),person.get('received')
            if lost is not None and received is not None:
                metrics.setdefault('cumulativeLossPercent',[]).append(100*lost/(lost+received) if lost+received else None)
        for peer in (sample.get('rtc') or {}).get('peers',[]):
            pair=peer.get('selectedPair')
            if pair:
                local,remote=pair.get('local') or {},pair.get('remote') or {}
                path={'route':'relay' if 'relay' in (local.get('candidateType'),remote.get('candidateType')) else 'direct',
                      'local':local,'remote':remote}
                if path not in row['paths']:row['paths'].append(path)
                rtt=pair.get('currentRoundTripTime')
                peers.setdefault('WebRTC 호스트 연결',{}).setdefault('rttMs',[]).append(rtt*1000 if rtt is not None else None)
        c=sample.get('cpu')
        if isinstance(c,dict):
            if cpu:
                before=cpu[-1];wall=c['wallSeconds']-before['wallSeconds']
                if wall>0:c['percentOneCore']=100*(c['cpuSeconds']-before['cpuSeconds'])/wall
            cpu.append(c)
    row['peerTrends']={name:{key:trend(values) for key,values in metrics.items()} for name,metrics in peers.items()}
    row['cpuTrend']=trend([c.get('percentOneCore') for c in cpu])
    if result.get('forceRelay') and not any(p['route']=='relay' for p in row['paths']):row['status']='failed'
    return row


def number(v):return '미측정' if v is None else (f'{v:.3f}' if isinstance(v,float) else str(v))
def triple(v):return '/'.join(number(v[k]) for k in ('min','median','max')) if v else '미측정'
def cell(v):return str(v).replace('|','/').replace('\n',' ')


def write_summary(output, cases, seconds, waves):
    summary={'seconds':seconds,'waves':waves,'cases':cases,
             'ok':bool(cases) and all(c.get('status')=='passed' for c in cases),
             'note':'0이 아닌 값은 그대로 표시. 미측정은 0이 아님. GPU/실제 오디오 장치 지연은 측정하지 않음.'}
    write_json(output/'summary.json',summary)
    lines=['# 세 PC 합방 시험', '', f'요청 측정 시간: 케이스당 {seconds}초. 수치는 반올림 표시하며 원본 JSON에 전체 정밀도로 보존.', '',
           '| 케이스 | 역할@PC (망) | 결과 | 연결 ms | 세션 끊김/재연결 | 웹 경로 | 왕복 중앙/p95/최대 ms | CPU 한 코어 % | 녹음 |',
           '| --- | --- | --- | ---: | --- | --- | --- | ---: | --- |']
    for case in cases:
        if case.get('error'):lines.append(f'| {case["name"]} | 실행기 | 실패: {cell(case["error"])} | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |')
        for p in case.get('participants',[]):
            paths='; '.join(f'{"중계" if q["route"]=="relay" else "직접"} {q["local"].get("protocol","?")}/{q["local"].get("relayProtocol","-")}' for q in p['paths']) or '해당 없음/미측정'
            r=p.get('roundtrip') or {};rtt='/'.join(number(r.get(k)) for k in ('medianMs','p95Ms','maxMs'))
            recordings='; '.join(f'{"통과" if r["ok"] else "실패"} {len(r["files"])}파일' for r in p['recordings']) or '해당 없음'
            lines.append(f'| {case["name"]} | {p["endpoint"]} ({p["network"]}) | {p["status"]} | {number(p["connectMs"])} | {p["disconnects"]}/{p["reconnects"]} | {paths} | {rtt} | {number((p.get("cpu") or {}).get("percentOneCore"))} | {recordings} |')
    lines+=['','| 케이스 | 듣는 역할@PC | 구간/출력 | 소리 | 끊김(시계 보정) | 최장 ms | 무음 창 | 음량 저하 창 | 분별 끊김(보정) | 시계 차 ppm | 위상 원판정 끊김 | 비고 |',
            '| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- |']
    for case in cases:
        for p in case.get('participants',[]):
            for c in p['continuity']:
                comp=c.get('compensated') or {}
                minutes=max(1,math.ceil(c.get('measuredSeconds',0)/60-1e-9))
                buckets=[0]*minutes
                for ms in comp.get('eventStartMs',[]):buckets[min(minutes-1,int(ms//60000))]+=1
                ppm=(comp.get('driftPpm') or {}).get('median')
                lines.append(f'| {case["name"]} | {p["endpoint"]} | {c["segment"]}/{c["stream"]} | {c["source"]} | {number(comp.get("events"))} | {number(comp.get("longestMs"))} | {c.get("quietWindows")} | {c.get("levelDropWindows")} | {",".join(map(str,buckets)) if comp else "미측정"} | {number(ppm[0]) if ppm else "미측정"} | {c["events"]} | {"계획된 웹 부재 포함" if c["includesPlannedAbsence"] else ""} |')
    lines+=['','| 케이스 | 역할@PC | 상대 | 통계 | 최소/중앙/최대 |', '| --- | --- | --- | --- | --- |']
    for case in cases:
        for p in case.get('participants',[]):
            for name,metrics in p['peerTrends'].items():
                for metric,values in metrics.items():lines.append(f'| {case["name"]} | {p["endpoint"]} | {name} | {metric} | {triple(values)} |')
            lines.append(f'| {case["name"]} | {p["endpoint"]} | 프로세스 트리 | CPU 한 코어 % | {triple(p["cpuTrend"])} |')
    lines+=['','| 케이스 | 역할@PC | 경과 ms | 세션/사람 상태 변화 |','| --- | --- | ---: | --- |']
    for case in cases:
        for p in case.get('participants',[]):
            for c in p['changes']:
                people=', '.join(f'{x.get("nick",x.get("id",x.get("participantId")))}:{x.get("state","명단")}' for x in c.get('people',[]))
                lines.append(f'| {case["name"]} | {p["endpoint"]} | {number(c["elapsedMs"])} | {c.get("state")} / {people} |')
    lines+=['','끊김(시계 보정) = 10ms 창의 음량 저하·과대·무음 + 주변 1초 위상 진행의 중앙값을 뺀 위상 튐. 위상 원판정은 시계 차를 빼지 않은 기존 판정(헤드리스 가짜 마이크처럼 시계가 1,500ppm 넘게 다르면 거의 모든 창이 튐으로 잡힘).',
            '네이티브 세션 5=연결, 6=끊김, 7=오류. 사람 상태 2=연결. 웹 RTT는 호스트 연결의 ICE RTT이며 표시음 왕복과 별도.',
            '세션 끊김/재연결 횟수는 측정 구간만 집계. 전체 통계·데이터 채널 메시지 수·상태 이벤트는 각 timeline.json에 보존. 계획된 웹 부재의 끊김도 0으로 바꾸지 않음.','']
    (output/'summary.md').write_text('\n'.join(lines),encoding='utf-8')
    return summary
