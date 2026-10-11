# KEPP LAB Autonomous Worker

기존 `gh` CLI 인증을 사용해 명시적으로 선택한 저장소의 열린
`agent:ready` Issue 중 생성일이 가장 오래된 하나를 조회합니다.
Issue가 있으면 저장소 식별자와 Issue 번호가 포함된 브랜치의 독립 Worktree를 만든 뒤,
그 Worktree에서 Codex CLI를 비대화형으로 실행합니다. Codex에는 Issue 제목과
본문이 전달되며 `gpt-5.5` 모델과 `workspace-write` sandbox가 적용됩니다. 대상은
`--repo`로 선택하고 지정한 로컬 경로의 origin URL과 일치하는지 확인합니다.
`keplerisgone/kepp-lab`과 `keplerisgone/mini-shell`만 허용합니다.

Issue가 없으면 출력 없이 종료 코드 0으로 종료합니다. Worktree 또는 Codex 실행이
실패하면 오류와 종료 코드를 보고하고 종료 코드 1로 종료합니다. Codex가 성공하고
검증된 변경이 있을 때만 변경 경로를 stage하고 `git diff --cached --check`를 통과한 뒤
Issue 브랜치에 commit하여 origin에 비강제 push합니다. 이어서 기본 브랜치를 대상으로
`Closes #<번호>`를 포함한 Draft PR을 생성합니다. 같은 head 브랜치의 PR이 이미 있으면
작업을 건너뜁니다. Codex 실행 후 변경이 없거나 승인된 검증이 실패하면 종료 코드 1을
반환하고 commit, push, PR을 만들지 않습니다.

`.env` 계열, 일반적인 credential/key 파일, 절대 경로 및 상위 디렉터리 경로는
commit 대상에서 거부합니다. Worker는 기본 브랜치를 checkout하거나 수정하지 않고,
Worktree를 자동으로 제거하거나 초기화하지 않으며 자동 merge도 하지 않습니다. 중단된
작업은 저장된 상태와 Git/GitHub 상태가 모두 일치할 때만 `--resume`으로 재개합니다.

## 빌드 및 실행

C++17 컴파일러, Make, pkg-config, json-c 개발 파일이 필요합니다.
macOS에서 의존성이 없다면 `brew install pkg-config json-c`로 설치할 수 있습니다.
Linux에서는 배포판의 pkg-config 및 json-c 개발 패키지를 설치합니다.
`gh`, `git`, `codex` CLI와 기존 GitHub 및 Codex 인증도 필요합니다.

```sh
make -C worker
./worker/worker --repo keplerisgone/kepp-lab --path /path/to/kepp-lab
./worker/worker --repo keplerisgone/mini-shell --path /path/to/mini-shell
./worker/worker --repo keplerisgone/mini-shell --path /path/to/mini-shell --resume
```

로컬 경로는 환경 변수로 사전 등록할 수도 있습니다. 등록 후에는 `--path`를 생략합니다.

```sh
export KEPP_LAB_PATH=/path/to/kepp-lab
export MINI_SHELL_PATH=/path/to/mini-shell
./worker/worker --repo keplerisgone/kepp-lab
```

`--repo`를 생략하면 저장소를 추측하지 않고 사용법과 함께 실패합니다. Codex의 기본
제한 시간은 3600초이며 `--timeout SECONDS`로 변경할 수 있습니다. `--resume`은
등록된 Worktree, Issue 내용, 브랜치, origin, 기준 커밋, 변경 지문과 로컬 상태가 모두
일치할 때만 Codex를 다시 실행하지 않고 안전한 다음 단계부터 계속합니다. 상태 기록이
없거나 `CODEX_RUNNING`/`BLOCKED`이면 기존 파일을 보존하고 명시적인 복구를 요구합니다.
빌드는 실행을 자동으로 시작하지 않습니다.

승인된 자동 검증 명령은 Issue 본문에서 읽지 않고 Worker에 고정되어 있습니다.
`keplerisgone/kepp-lab`은 `worker/`에서 `make test`를 실행합니다.
`keplerisgone/mini-shell`에는 저장소별 자동 검증 명령을 강제하지 않습니다.

## 테스트

테스트는 GitHub에 접속하거나 실제 Git 저장소를 변경하지 않습니다.
가짜 `gh`, `git`, `codex`, `make` 프로세스를 사용해 Issue 없음, Worktree 명령,
Codex sandbox와 실행 위치, 프롬프트, 실패 처리, 변경 검증, commit/push,
Draft PR 생성 및 중복 방지를 검증합니다.
Python 3는 테스트에만 필요하며 worker 실행에는 필요하지 않습니다.

```sh
make -C worker test
# 또는 빌드 후 기존 테스트 명령
python3 -m unittest discover -s worker -p 'test_*.py'
```

`make -C worker clean`은 생성된 실행 파일만 제거합니다.
