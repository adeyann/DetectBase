# GRPC `protoc` 코드 생성 안내

DetectBase 의 proto 파일은 [`MgenProto.proto`](protos/MgenProto.proto) 한 개.
`MgenProto.{pb,grpc.pb}.{h,cc}` 가 자동 생성됨.

## 자동 (권장)

상위 경로의 `detectbase.sh init` 가 proto 재생성을 자동 처리. proto 수정 후:

```bash
./detectbase.sh init        # 컨테이너 안에서 protoc 호출 → 4개 파일 갱신
./detectbase.sh compile     # 재빌드
```

`init` 은 `build` 내부에서 자동 호출되므로 일반적으로 별도 호출 불필요.

## 수동 (디버깅 등)

컨테이너 내부에서 직접 호출:

```bash
cd /DetectBase/code/Protocol/GRPC/protos
protoc --cpp_out=. --grpc_out=. \
    --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
    MgenProto.proto
```

## 다른 언어 (참고)

DetectBase 는 C++ 만 사용하지만, 다른 노드 / 외부 client 가 다른 언어로 구현될 경우:

### C#
```bash
protoc -I=. --csharp_out=. \
    --plugin=protoc-gen-grpc=$(which grpc_csharp_plugin) \
    --grpc_out=. \
    MgenProto.proto
```

### Python
```bash
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. MgenProto.proto
```

## proto 파일 변경 시 주의

- proto / grpc 버전이 컨테이너 내부 (Dockerfile.build) 기준으로 일치해야 함
- 외부에서 .pb.cc 를 미리 생성해서 컨테이너에 넣으면 ABI 불일치 가능 → 항상 컨테이너 안에서 재생성 (init)
- proto 변경 후 컴파일 안 하면 새 RPC handler 가 binary 에 들어가지 않음 → `compile` 잊지 말 것

## 현재 정의된 RPC

`MgenProto.proto` 의 `service DETECTBASE_GRPC`:

| RPC | 방향 | 용도 |
|---|---|---|
| `SendEventOnlyJson` / `SendEventWithImages` | 단방향 | 이벤트 push |
| `RequestEventOnlyJson` / `RequestEventWithImages` | 양방향 | 이벤트 query → 응답 |
| `SendCounterDelta` | 단방향 | counter 변화량 push (샘플) |
| `RequestCounterSnapshot` | 양방향 | counter 누적 query (샘플) |
| `SendHeartbeat` | 단방향 | peer 살아있음 ping (샘플) |

샘플 RPC 들은 인프라만 갖춤. 실제 사용 / 메시지 형식은 분기 프로젝트가 결정.
