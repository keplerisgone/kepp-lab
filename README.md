# KEPP LAB Autonomous Worker

기존 `gh` CLI 인증을 사용해 이 저장소의 열린 `agent:ready` Issue 중
생성일이 가장 오래된 하나의 번호, 제목, 본문을 출력합니다.
Issue가 없으면 출력 없이 종료 코드 0으로 종료하고, 조회 오류 시
표준 오류에 오류를 출력하고 종료 코드 1로 종료합니다.

## 빌드 및 실행

C++17 컴파일러, Make, pkg-config, json-c 개발 파일이 필요합니다.
macOS에서 의존성이 없다면 `brew install pkg-config json-c`로 설치할 수 있습니다.
Linux에서는 배포판의 pkg-config 및 json-c 개발 패키지를 설치합니다.
`gh` CLI와 기존 GitHub 인증도 필요합니다.

```sh
make -C worker
./worker/worker
```

호출한 디렉터리와 관계없이 실행 파일이 있는
`worker/`의 상위 저장소를 기준으로 `gh`를 실행합니다.
인자는 기존과 같이 무시합니다. 빌드는 실행을 자동으로 시작하지 않습니다.

## 테스트

테스트는 GitHub에 접속하거나 데이터를 변경하지 않습니다.
가짜 `gh` 프로세스를 사용해 인자, 저장소 기준 실행 위치, 출력과 종료 코드를 검증합니다.
Python 3는 테스트에만 필요하며 worker 실행에는 필요하지 않습니다.

```sh
make -C worker test
# 또는 빌드 후 기존 테스트 명령
python3 -m unittest discover -s worker -p 'test_*.py'
```

`make -C worker clean`은 생성된 실행 파일만 제거합니다.
