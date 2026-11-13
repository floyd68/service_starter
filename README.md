# service_starter

English first; the Korean version follows below. / 영문 다음에 한글본이 이어집니다.

A C++20 library for running **the same code** as a console program during development and as a system service in deployment. It wraps Windows SCM (Service Control Manager) and Linux systemd in one lifecycle and has no external dependencies (standard library + Win32 / POSIX).

Origin: a rewrite that carries over the design intent of an in-house, Win32-only `ServiceStarter.h` from 2017 (template `ServiceStarter<Server>`, console fallback, install/uninstall). No original code was copied, and that code's defects (thread race, SHUTDOWN not accepted, missing failure exit codes, unisolated exceptions, no console Ctrl+C handling, and others) are fixed in this implementation.

## Lifecycle contract

An application implements three steps. The host guarantees the order: **construct → `initialize` → `run` → `shutdown` → destroy**.

```cpp
struct MyServer
{
    void initialize(const service_starter::HostContext& ctx); // Prepare config/DB/listeners. Throw to fail.
    int  run(std::stop_token stop);                          // Block until stop.stop_requested(). Return exit code.
    void shutdown();                                         // Release resources. Called even if run threw.
};
static_assert(service_starter::ServiceApplication<MyServer>);

int wmain(int argc, wchar_t** argv)      // Linux: int main(int argc, char** argv)
{
    service_starter::ServiceOptions options;
    options.name = "XTankServer";
    options.unit_user = "xtank";
    return service_starter::run_main<MyServer>(argc, argv, options);
}
```

- `HostContext`: run mode (`console`/`service`), service name, executable path, application arguments (UTF-8), `Progress*`, `Logger`.
- Call `ctx.progress->report("status")` during long `initialize`/`shutdown` steps. The host also reports automatic checkpoints every `progress_interval`, but **only until `progress_timeout` (default 60 s) after the last explicit `report()`**. A step that stays silent longer is exposed to the manager's own timeout, so a hang is not hidden (this avoids the "increment the checkpoint unconditionally" pattern Microsoft warns about). Report long work as it actually progresses.
- To stop from inside `run`, call `service_starter::request_stop()` or simply return from `run`.
- If you need a virtual interface, derive from `service_starter::Application` and pass a factory to `run_main(argc, argv, options, factory)`.

## Command line

```
<exe>                     auto: service when started by the service manager, otherwise console
<exe> --console           force console mode
<exe> --service           force service mode (exit code 2 on Windows when SCM did not start the process)
<exe> install [NAME]      register the service (Windows: CreateService, Linux: write a unit file)
<exe> uninstall [NAME]    remove the service
<exe> --help
<exe> ... -- <app args>   everything after "--" goes to the application and is stored with an installed service
```

Exit codes: the application's `run` return value is passed through. Host codes: `exit_usage=2`, `exit_initialize_failed=10`, `exit_run_failed=11`, `exit_shutdown_failed=12`, `exit_host_failed=13`.

## Platform behavior

| | Windows | Linux |
|---|---|---|
| Service detection | Falls back to console only when `StartServiceCtrlDispatcher` returns 1063 (`ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`); any other error throws | `NOTIFY_SOCKET` or `INVOCATION_ID` environment variable |
| Stop signals | Console: Ctrl+C/Ctrl+Break, window close/logoff/shutdown (waits up to 4.5 s). Service: STOP/SHUTDOWN/PRESHUTDOWN | SIGINT/SIGTERM/SIGHUP, consumed by a dedicated thread with `sigtimedwait` (no async-signal-safety constraints). SIGPIPE ignored. Signals that start as SIG_IGN are reset to SIG_DFL so they are not discarded |
| Status reporting | `SERVICE_START_PENDING`/`STOP_PENDING` checkpoints, `RUNNING`, `STOPPED`. On failure `ERROR_SERVICE_SPECIFIC_ERROR` + exit code (triggers recovery actions). If `SetServiceStatus` fails, the host does not keep running unmanaged: it enters the stop sequence and exits with `exit_host_failed` | `Type=notify`: `STATUS=`, `READY=1`, `STOPPING=1`, `EXTEND_TIMEOUT_USEC=` on every progress report while pending, `WATCHDOG=1` at half of `WATCHDOG_USEC` when set. Non-blocking socket (a full receive queue never stalls). No libsystemd |
| Default logger | Console: stdout/stderr (UTF-8). Service: `OutputDebugString` plus Application event log for warnings/errors | Console: `[level]` prefix. Service: journald priority prefixes (`<6>` etc.) |
| install | Quoted path + `--service -- args`, description, recovery actions (3 restarts, `restart_delay`; registered with `SERVICE_START` access, installation rolled back on failure), account (default `LocalService`), dependencies | Writes `unit_directory/NAME.service` (never overwrites). `systemctl enable` remains an operator step |
| uninstall | Requests stop, waits `stop_timeout + 5 s`, `DeleteService` | Removes the unit file. Stop/disable an active unit first |
| Working directory | With `change_directory_to_executable=true` (default) the process changes to the executable's directory in **every mode** (no relative-path differences between console and service) | Same, and `WorkingDirectory=` is written into the unit |

When initialization fails, the application object is destroyed before `STOPPED` is reported (the manager may kill the process right after). A throwing logger never affects the lifecycle. Calling `Progress::report()` from a `std::stop_callback` is safe (stop requests are issued after internal locks are released).

Applications must not handle SIGINT/SIGTERM/SIGHUP or Windows console control events themselves (for example, do not register the same signals in an Asio `signal_set`). The host owns them and delivers them through the `stop_token`. One host per process.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

- `tests/lifecycle_tests.cpp`: ordering, failure codes, `request_stop`, argument parsing, unit text. On Linux it also verifies SIGTERM delivery and `READY/STOPPING/WATCHDOG` reception through a fake `NOTIFY_SOCKET`.
- `tests/windows_scm_tests.cpp`: SCM lifecycle driven through a fake SCM API (`service_starter::windows::ScmApi`), no privileges required.
- `examples/example_service.cpp`: heartbeat example for smoke tests, e.g. `--console -- --exit-after 1500` or `install my-svc -- --period 1000`. The `SERVICE_STARTER_UNIT_DIR` environment variable redirects unit-file writes for non-root runs.

In a parent project, `add_subdirectory(third_party/service_starter)` and link `service_starter::service_starter`. `SERVICE_STARTER_BUILD_TESTS`/`SERVICE_STARTER_BUILD_EXAMPLES` default to ON only for a top-level build.

## Out of scope

- Other SCM controls such as PAUSE/CONTINUE and session changes.
- Self-daemonizing via fork/double-fork. On Linux only the model where systemd supervises a foreground process is supported.
- Automatic detection of the systemd unit name. In service mode `service_name` is `ServiceOptions::name` (on Windows it is the name given by SCM).
- The Windows `char**` entry point treats arguments as UTF-8. Use `wmain` for Unicode arguments.

---

# service_starter (한국어)

개발 중에는 콘솔 프로그램으로, 배포 환경에서는 시스템 서비스로 **같은 코드**를 실행하기 위한 C++20 라이브러리입니다. Windows SCM(서비스 제어 관리자)과 Linux systemd를 하나의 라이프사이클로 묶고, 외부 의존성이 없습니다(표준 라이브러리 + Win32 / POSIX).

출처: 2017년에 사내에서 쓰던 Win32 전용 `ServiceStarter.h`(템플릿 `ServiceStarter<Server>`, 콘솔 fallback, install/uninstall)의 설계 의도를 이어받아 새로 작성했습니다. 원본 코드는 복사하지 않았으며, 그 코드의 결함(스레드 레이스, SHUTDOWN 미수용, 실패 종료 코드 누락, 예외 미격리, 콘솔 Ctrl+C 미처리 등)은 이 구현에서 해결했습니다.

## 라이프사이클 계약

애플리케이션은 세 단계를 구현합니다. 호스트가 순서를 보장합니다: **생성 → `initialize` → `run` → `shutdown` → 소멸**.

```cpp
struct MyServer
{
    void initialize(const service_starter::HostContext& ctx); // 설정·DB·리스너 준비. 실패 시 throw
    int  run(std::stop_token stop);                          // stop.stop_requested()까지 블록. 종료 코드 반환
    void shutdown();                                         // 자원 해제. run이 throw해도 호출됨
};
static_assert(service_starter::ServiceApplication<MyServer>);

int wmain(int argc, wchar_t** argv)      // Linux: int main(int argc, char** argv)
{
    service_starter::ServiceOptions options;
    options.name = "XTankServer";
    options.unit_user = "xtank";
    return service_starter::run_main<MyServer>(argc, argv, options);
}
```

- `HostContext`: 실행 모드(`console`/`service`), 서비스 이름, 실행 파일 경로, 애플리케이션 인수(UTF-8), `Progress*`, `Logger`.
- `initialize`/`shutdown`이 길면 `ctx.progress->report("상태")`를 호출합니다. 호스트는 `progress_interval`마다 자동 체크포인트도 보고하지만, **마지막 명시적 `report()` 이후 `progress_timeout`(기본 60초)까지만** 보고합니다. 그 이상 침묵하는 단계는 관리자의 타임아웃에 노출되어 교착이 숨겨지지 않습니다(Microsoft가 경고하는 "무조건 체크포인트 증가" 패턴을 피함). 긴 작업은 실제 진행에 맞춰 `report()`를 호출하세요.
- `run` 중에 스스로 종료하려면 `service_starter::request_stop()`를 호출하거나 `run`에서 반환합니다.
- 가상 인터페이스가 필요하면 `service_starter::Application`을 상속하고 `run_main(argc, argv, options, factory)`에 팩토리를 넘깁니다.

## 명령줄

```
<exe>                     자동: 서비스 관리자가 시작했으면 서비스, 아니면 콘솔
<exe> --console           콘솔 강제
<exe> --service           서비스 강제 (Windows에서 SCM이 시작하지 않았으면 종료 코드 2)
<exe> install [NAME]      서비스 등록 (Windows: CreateService, Linux: 유닛 파일 작성)
<exe> uninstall [NAME]    서비스 제거
<exe> --help
<exe> ... -- <app args>   "--" 뒤는 애플리케이션 인수. install 시 서비스에 함께 저장됨
```

종료 코드: 애플리케이션 `run` 반환값 그대로. 호스트 코드는 `exit_usage=2`, `exit_initialize_failed=10`, `exit_run_failed=11`, `exit_shutdown_failed=12`, `exit_host_failed=13`.

## 플랫폼 동작

| | Windows | Linux |
|---|---|---|
| 서비스 감지 | `StartServiceCtrlDispatcher`가 1063(`ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`)을 돌려줄 때만 콘솔로 전환. 다른 오류는 예외 | `NOTIFY_SOCKET` 또는 `INVOCATION_ID` 환경 변수 |
| 정지 신호 | 콘솔: Ctrl+C/Ctrl+Break, 창 닫기·로그오프·시스템 종료(최대 4.5초 대기). 서비스: STOP/SHUTDOWN/PRESHUTDOWN | SIGINT/SIGTERM/SIGHUP. 전용 스레드가 `sigtimedwait`로 소비(async-signal-safe 제약 없음). SIGPIPE 무시. SIG_IGN으로 시작된 신호는 SIG_DFL로 되돌려 버려지지 않게 함 |
| 상태 보고 | `SERVICE_START_PENDING`/`STOP_PENDING` 체크포인트 증가, `RUNNING`, `STOPPED`. 실패 시 `ERROR_SERVICE_SPECIFIC_ERROR` + 종료 코드(복구 작업 발동). `SetServiceStatus`가 실패하면 관리되지 않는 상태로 계속 돌지 않고 즉시 정지 절차로 들어가 `exit_host_failed` | `Type=notify`: `STATUS=`, `READY=1`, `STOPPING=1`, pending 중 진행 보고마다 `EXTEND_TIMEOUT_USEC=`, `WATCHDOG_USEC`가 있으면 절반 주기로 `WATCHDOG=1`. 비블로킹 소켓(수신 큐가 가득 차도 멈추지 않음). libsystemd 불필요 |
| 기본 로거 | 콘솔: stdout/stderr(UTF-8). 서비스: `OutputDebugString` + 경고/오류는 Application 이벤트 로그 | 콘솔: `[level]` 접두. 서비스: journald 우선순위 접두(`<6>` 등) |
| install | 따옴표 처리된 경로 + `--service -- args`, 설명, 복구 작업(재시작 3회, `restart_delay`; `SERVICE_START` 권한으로 등록, 실패 시 설치 롤백 후 오류), 계정(기본 `LocalService`), 의존성 | `unit_directory/NAME.service` 작성(기존 파일 덮어쓰지 않음). `systemctl enable`은 운영자가 실행 |
| uninstall | 정지 요청 후 `stop_timeout + 5초` 대기, `DeleteService` | 유닛 파일 삭제. 활성 유닛은 먼저 stop/disable |
| 작업 디렉터리 | `change_directory_to_executable=true`(기본)면 **모든 모드**에서 실행 파일 디렉터리로 변경 (콘솔/서비스 간 상대 경로 차이 제거) | 같음. 유닛에도 `WorkingDirectory=` 기록 |

초기화가 실패하면 애플리케이션 객체를 먼저 소멸시킨 뒤 `STOPPED`를 보고합니다(관리자가 보고 직후 프로세스를 종료할 수 있으므로). 로거가 예외를 던져도 라이프사이클은 영향받지 않습니다. `std::stop_callback`에서 `Progress::report()`를 호출해도 안전합니다(정지 요청은 내부 잠금을 해제한 뒤 전달).

애플리케이션은 SIGINT/SIGTERM/SIGHUP과 Windows 콘솔 제어 이벤트를 직접 처리하지 않아야 합니다(예: Asio `signal_set`에 같은 신호를 등록하지 않음). 호스트가 소유하고 `stop_token`으로 전달합니다. 프로세스당 호스트는 하나입니다.

## 빌드·테스트

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

- `tests/lifecycle_tests.cpp`: 순서·실패 코드·`request_stop`·인수 파싱·유닛 텍스트. Linux에서는 SIGTERM 전달과 가짜 `NOTIFY_SOCKET`을 통한 `READY/STOPPING/WATCHDOG` 수신까지 검증.
- `tests/windows_scm_tests.cpp`: 가짜 SCM API(`service_starter::windows::ScmApi`)로 권한 없이 SCM 라이프사이클 검증.
- `examples/example_service.cpp`: 하트비트 예제. `--console -- --exit-after 1500`, `install my-svc -- --period 1000` 등으로 스모크 테스트. `SERVICE_STARTER_UNIT_DIR` 환경 변수로 비루트 환경에서 유닛 작성 위치를 바꿀 수 있습니다.

상위 프로젝트에서는 `add_subdirectory(third_party/service_starter)` 후 `service_starter::service_starter`를 링크합니다. `SERVICE_STARTER_BUILD_TESTS`/`SERVICE_STARTER_BUILD_EXAMPLES`는 최상위 빌드일 때만 기본 ON입니다.

## 범위 밖

- PAUSE/CONTINUE, 세션 변경 등 그 밖의 SCM 제어.
- 자체 fork/double-fork 데몬화. Linux는 systemd가 foreground 프로세스를 감독하는 모델만 지원합니다.
- systemd 유닛 이름 자동 감지. 서비스 모드의 `service_name`은 `ServiceOptions::name`입니다(Windows는 SCM이 준 이름).
- Windows `char**` 진입점은 인수를 UTF-8로 간주합니다. 유니코드 인수가 필요하면 `wmain`을 사용하세요.
