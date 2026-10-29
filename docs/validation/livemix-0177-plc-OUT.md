**LiveMix 0.17.7 PLC 구현 및 검증 (2026-10-29)**

이 문서는 최초 PLC 구현의 시험 기록이다. 최신 수정·시험·Probe 정보는 [FIX2 OUT](livemix-0177-plc-fix2-OUT.md)에 있다. [FIX1 OUT](livemix-0177-plc-fix1-OUT.md)은 첫 검토 수정 당시 기록이다.

브랜치 `livemix-0177-plc`, 기준 `dae954ba228849d0824474c0e5a6f8e74096a68a`. 커밋하지 않았다. `release0176_done.flag`는 11:02:16 KST에 생성됐고 60초 확인 루프에서 11:03:12에 확인했다. 모든 빌드·시험은 그 뒤 `rb.py`를 통해 roomc에서 실행했다.

빈 패킷과 underrun 뒤 refill에 같은 PLC를 적용했다. 정상 샘플은 그대로 전달하면서 ASRC 뒤의 기록에 복사하고, 손실 때만 출력 파형을 대체한다. native와 web 모두 기존 `CollabPlayout` 출력을 사용하므로 모니터·OBS 상대 링·사람별 녹음에도 적용된다. `livemix/webjoin` 변경은 없다.

| 바꾼 파일 | 함수 및 역할 |
|---|---|
| `livemix/src/CollabPlc.h`, `.cpp` | `prepare`, `reset`, `remember`, `beginLoss`, `findPitch`, `sample`, `loop`, `synthesize`, `conceal`, `receive`. 기록·피치 탐색·주기 반복·감쇠·복구 겹침을 처리한다. 조정값은 `CollabPlc::Tuning`에 모았다. |
| `livemix/src/CollabPlayout.h`, `.cpp` | `prepare`, `beginRefill`, `startPlayback`, `loadPacket`, `pull`. PLC를 연결하고, 빈 패킷과 refill의 연속 숨김 시간을 유지한다. 기존 fade 카운터와 refill의 release 카운터는 유지하며, 복구 음성의 이중 페이드만 피한다. 제어에 쓰는 마지막 출력은 합성 전 값으로 보존한다. |
| `CMakeLists.txt` | `CollabPlc.cpp`를 `LiveMixCore`에 포함한다. |
| `tests/LiveMixCollabPlcTests.cpp` | 기준 재생기와 샘플·통계 비교, 42개 손실 조합의 SNR·무음·이음 측정, 장치율·분할 콜백·CPU 검증. |
| `tests/reference/CollabPlayout0176.h`, `.cpp` | 기준 커밋의 재생기를 클래스 이름만 바꿔 보존한 시험 전용 기준본. 이름과 첫 주석을 되돌리면 기준 소스와 일치한다. 제품에는 포함하지 않는다. |
| `tests/LiveMixCollabPlcRealtime.cpp`, `tests/CMakeLists.txt` | Debug CRT로 PLC 정상·손실·복구·장기 무음 경로의 힙 작업을 계수하는 작은 실행 파일과 시험 소스를 추가한다. |
| `tools/livemix_plc_verify.ps1` | roomc 확인 후 Release 빌드, PLC 집중 시험, 전체 Collab/WebGuest 회귀, Debug 할당 시험을 실행하고 로그를 남긴다. |

알고리즘은 [ITU-T G.711 부록 I의 I.2.3~I.2.7](https://www.itu.int/rec/T-REC-G.711-199909-I!AppI/en)의 피치 추정·파형 반복·긴 손실 감쇠·복구 겹침 방식을 추가 대기 없이 적용했다.

| 항목 | 구현 수치 |
|---|---|
| 기록과 탐색 | 장치율 기준 약 48.8ms 보관. 탐색은 최근 15ms와 최대 15ms 이전 구간을 비교하므로 최근 약 30ms를 사용한다. |
| 피치 | 2.5~15ms. L+R 평균에서 평균값을 제거한 정규화 상관. 상관 0.7 이상, RMS 0.0001 초과. |
| 탐색 비용 | 약 8kHz로 평균 다운샘플 후 거친 탐색, 주변 원래 샘플에서 정밀 탐색, 포물선 보간으로 소수 샘플 주기 추정. 48kHz에서는 6:1. 연속 손실 시작에 한 번만 실행한다. |
| 시작 이음 | 마지막 주기의 위상에서 이어 재생하고, 끝값 차이는 1/4주기에 걸쳐 보정한다. 실제 소리를 뒤로 미루거나 이미 출력한 소리를 수정하지 않는다. |
| 긴 손실 | 첫 10ms는 한 주기, 10ms부터 두 주기, 20ms부터 세 주기. 기록이 충분한 범위에서 늘리고 1/4주기로 연결한다. |
| 감쇠 | 첫 10ms 100%. 이후 10ms당 원래 크기의 20%를 선형으로 감소: 20/30/40/50/60ms에 80/60/40/20/0%. 60ms부터 정확한 0. |
| 피치 불확실·잡음·무음·기록 부족 | 마지막 샘플에서 2.5ms 페이드 아웃. |
| 복구 | 숨김 소리를 한 주기 더 만들며 실제 소리와 겹친다. `u*u*(3-2*u)` 비율로 양 끝 기울기를 0으로 만들어 ASRC 초기 과도응답을 줄인다. fallback은 2.5ms 복구. 추가 버퍼 대기 0. |
| 스테레오 | 한 피치·진행 위치·겹침 비율을 공유하고 각 채널의 파형을 따로 이어 붙인다. |
| 실시간 제약 | 저장 공간은 `prepare`에서만 할당. 콜백의 새 경로에는 할당·잠금이 없다. |

**전후 합성 신호 수치**

48kHz/120프레임 패킷. 순음은 137Hz, peak 0.4. 배음 신호는 173.3Hz의 1·2·3·5배음과 3.1Hz 진폭 변조. 잡음은 결정적인 백색잡음, peak 0.35. R은 L의 0.63배다. SNR·RMS·샘플 변화량은 L채널을 측정했고, R=0.63L 유지도 별도로 검사했다. SNR은 숨김 구간에서 손실 없는 재생 출력을 기준으로 계산했다. refill 복구 후에는 원래 재생기가 선택한 새 시작 프레임을 그대로 사용하고, 복구 이음은 별도로 측정했다.

아래는 `0.17.6 → 수정 후` SNR(dB)이다. 전체 42개 조합의 SNR, 첫 60ms 안의 최소 10ms RMS, 시작·복구 최대 샘플 변화량은 [metrics.csv](../../build/plc0177/metrics.csv)에 있다. 10ms보다 짧은 손실의 10ms 창 수치는 `n/a`다.

| native 손실 | 순음 | 배음 | 잡음 |
|---|---:|---:|---:|
| 1패킷, 2.5ms | 1.932 → 86.986 | 2.628 → 40.338 | -0.147 → -0.777 |
| 2패킷, 5ms | 1.211 → 77.646 | 1.436 → 39.763 | -0.076 → -0.403 |
| 4패킷, 10ms | 0.496 → 74.319 | 0.655 → 34.531 | -0.039 → -0.209 |
| refill 10ms | 0.516 → 82.090 | 0.128 → 29.588 | -0.085 → -0.037 |
| refill 20ms | 0.244 → 21.349 | 0.057 → 25.418 | -0.044 → -0.019 |
| refill 40ms | 0.127 → 10.696 | 0.029 → 11.865 | -0.022 → -0.010 |
| refill 80ms | 0.063 → 3.453 | 0.014 → 3.449 | -0.011 → -0.005 |

| web 손실 | 순음 | 배음 | 잡음 |
|---|---:|---:|---:|
| 1패킷, 2.5ms | 1.933 → 88.622 | 2.631 → 40.297 | -0.146 → -0.584 |
| 2패킷, 5ms | 1.212 → 78.133 | 1.436 → 39.734 | -0.075 → -0.300 |
| 4패킷, 10ms | 0.496 → 74.494 | 0.655 → 34.516 | -0.038 → -0.155 |
| refill 10ms | 0.517 → 40.009 | 0.057 → 28.147 | -0.796 → -0.394 |
| refill 20ms | 0.244 → 21.317 | 0.026 → 24.833 | -0.427 → -0.207 |
| refill 40ms | 0.127 → 10.688 | 0.013 → 11.820 | -0.221 → -0.106 |
| refill 80ms | 0.063 → 3.452 | 0.006 → 3.445 | -0.110 → -0.053 |

순음·배음의 첫 60ms에는 10ms 무음 창이 없었다. 모든 해당 창의 최소 RMS는 0.018748이었다. 기존 방식은 20/40/80ms refill에서 10ms 창 RMS가 0이었다. 80ms 손실의 60~80ms 출력은 두 프로필 모두 샘플마다 정확히 0이었다. 잡음은 피치 반복을 사용하지 않고 2.5ms에 0으로 갔다. 백색잡음의 SNR 개선은 보장되지 않으며, 패킷 손실에서는 최대 0.630dB 낮아졌다.

| 이음의 최대 인접 샘플 변화량 | 기존 | 수정 후 |
|---|---:|---:|
| 순음, 시작 | 0.007155 | 0.007154 |
| 순음, 복구 | 0.007996 | 0.008391 |
| 배음, 시작 | 0.002729 | 0.013721 |
| 배음, 복구 | 0.021391 | 0.021391 |

변화량은 정규화 PCM의 절댓값이다. 시작에서 실제 파형을 계속하는 배음의 변화량도 복구 뒤 정상 파형의 최대 변화량보다 작았다. 복구 허용치는 순음 0.012, 배음 0.04로 검사했다. 잡음은 원래 인접 샘플 변화가 크므로 이 음성용 기준을 적용하지 않고 짧은 페이드 및 무음 전환을 검사했다.

**무손실 동일성·통계·CPU**

native/web × wire 44.1/48kHz × device 44.1/48/96kHz × block 120/256/1024의 36개 조합에서 9,028,800개 채널 샘플이 기준본과 비트 단위로 같았다. 시작 페이드와 첫 측정 이후 목표 변경도 포함한다. 손실 조합에서도 매 pull의 state, targetMs, fillMs, correctionPpm, lossPercent10s, received, lost, late, underruns가 기준본과 모두 같았다. 장치율 44.1/48/96/192kHz에서 콜백 분할과 60ms 무음 전환도 검사했다.

roomc는 AMD Ryzen 7 9700X. 다음 CPU 수치는 최종 PLC 집중 시험의 Release `pull(120)` 경과 시간이다. 6,000회 평균을 3번 측정한 최솟값이며 패킷 생성·push는 제외했다. 손실 조건은 첫 1,000블록 이후 매 100패킷 하나를 누락해 전체 50개를 누락한 것이다.

| CPU 측정 | 기존 | 수정 후 | 차이 |
|---|---:|---:|---:|
| 정상 120프레임 pull | 4.935µs | 4.951µs | +0.33% |
| 위 손실 조건의 평균 pull | 4.930µs | 5.112µs | +3.69% |
| PLC 시작 탐색+120프레임 합성 | 해당 없음 | p50 20.4µs, p99 24.3µs | 300회 측정 |

Debug CRT에서 44.1/48/96/192kHz 각각 정상·짧은 손실·복구·긴 refill·reset을 20회 반복했고 할당·재할당·해제는 모두 0회였다. 새 코드의 잠금 호출은 없다.

**최종 빌드·회귀·Probe**

| roomc 시험 | 통과 | 실패 | 시간 |
|---|---:|---:|---:|
| 최종 PLC 집중 시험 | 20,554 | 0 | 1.91초 |
| 기존 LiveMix Collab playout | 65 | 0 | 2.12초 |
| 최종 LiveMix Collab 전체 (PLC 포함) | 40,023 | 0 | 227.47초 |
| 최종 LiveMix WebGuest 전체 | 436 | 0 | 3.79초 |
| Debug CRT 할당 검사 | 4개 장치율 모두 힙 작업 0회 | 0 | 별도 작은 실행 파일 |

기존 시험의 기대값은 수정하지 않았다. `LiveMix`, `EnqueueTests`, `LiveMixCollabProbe` Release 빌드와 `LiveMixCollabPlcRealtimeTest` Debug 빌드가 성공했다. 마지막 음성 코드 변경 이후 전체 회귀를 다시 통과했다.

로그와 숫자:

- [최종 PLC 집중 시험](../../build/plc0177/LiveMix-Collab-PLC.log), [42개 조합 CSV](../../build/plc0177/metrics.csv)
- [기존 playout](../../build/plc0177/LiveMix-Collab-playout.log), [Collab 전체](../../build/plc0177/LiveMix-Collab.log), [WebGuest 전체](../../build/plc0177/LiveMix-WebGuest.log)
- [Debug CRT](../../build/plc0177/realtime.log), [실행 시간](../../build/plc0177/timing.txt), [Release 빌드](../../build/plc0177/build.log), [Debug 빌드](../../build/plc0177/realtime-build.log)

`rb.py --fetch build/vs2022/LiveMixCollabProbe_artefacts/Release`로 최종 Probe를 이 PC에 가져왔고, roomc와 이 PC의 SHA-256이 일치했다.

- 실행 파일: [LiveMixCollabProbe.exe](../../build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe)
- 크기: 13,322,752 bytes
- SHA-256: `03FFE4F488FF668A12FF1FCF7AE6F47EE36D46860A7B99CEA7DCBCB124DBCD8A`
- `LIVEMIX_VERSION`은 기존 `0.17.6`을 유지했다. 이 PLC 시험용 바이너리는 경로와 위 해시로 구분한다.

실제 세 PC 10분 망 비교는 아직 수행하지 않았다. 위 Probe가 그 비교에 사용할 최종 빌드다.
