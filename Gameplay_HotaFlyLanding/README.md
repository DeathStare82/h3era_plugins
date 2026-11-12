# Gameplay_HotaFlyLanding

비행(Fly)·수면보행(Water Walk) 상태에서 오브젝트 방문 규칙을 HotA처럼 바꾸는 ERA 플러그인.

- **규칙 A (착지 규칙)**: 오브젝트는 영웅이 실제로 *착지할 수 있는 칸*에서만 방문할 수 있다. 바위·나무 같은 장애물 위, 물 위, 미탐색 칸에 떠 있는 상태로는 들어갈 수 없다. 사방이 막힌 오브젝트는 비행으로도 방문 불가.
- **규칙 B (경비 규칙)**: 떠돌이 몬스터 옆 칸에 내려서 옆 칸에서 줍거나 방문하는 오브젝트(상자·유물·판도라·Quest Guard·영웅 등)를 건드리려면, 먼저 그 칸에 멈춰서 몬스터와 싸워야 한다. (HotA 문서의 "경비가 있는 오브젝트는 인접 칸에 착지해 경비와 싸워야 한다"에 해당)
- 예외: 아군 주둔지(Garrison)처럼 자유롭게 통과 가능한 오브젝트. 비행 중 오브젝트 입구 칸 *위로 지나가는* 것은 그대로 허용.

## 원인: 바닐라 길찾기가 하는 일

어드벤처 맵 길찾기는 `searchArray`(전역 포인터 `0x6992D4`)가 처리한다. 각 칸은 "착지 상태(canStop=1)"와 "공중 상태(canStop=0)" 두 층을 갖고, 장애물·물·안개 위의 칸은 공중 상태가 된다.

이웃 칸으로 확장하는 `testPossibleDirections`의 핵심 조건(homm3-decomp `src/findpath.cpp`):

```cpp
if (source.m_canStop || !destCell->m_isTrigger
    || (akAdvObjectTypeTraits[destCell->m_type].m_blocksLanding == 0 && destCell->m_type != TOWN))
    pushPoint(source, dest, ..., destCell->m_isTrigger);
```

즉 공중에 떠 있어도 `blocksLanding` 플래그가 없는 오브젝트는 들어갈 수 있다. 바닐라에서 공중 방문이 이미 막혀 있는 타입은 줍는 물건, 몬스터, 영웅, 감옥, 이벤트, Quest Guard(215), Border Guard(9), 학자, 선술집, 마을 등뿐이고, **Border Gate(212)·주둔지·광산·생산소·은행·성소 등은 공중에서 방문된다.** 관문을 Quest Guard로 막아도 옆 장애물 위에 떠서 들어가는 이유가 이것이다.

경비 쪽은 `seedPosition`이 비행 중인 칸에는 몬스터 지배 영역(ZOC) 제한을 걸지 않고(`!cell.m_flying && ...`), `endMoveHero`도 비행 중에는 멈출 때나 경로 끝 칸에서만 `checkAdjacentMon`을 부른다. 옆 칸에서 방문하는 오브젝트(`blocksLanding`)는 `handleStopOnTrigger`를 거치는데 여기서는 영웅이 움직이지 않으므로 경비 검사가 아예 없다. 그래서 경비 옆 평지 칸을 지나가는 도중에 바로 오브젝트를 집을 수 있다. 반면 광산·마을·관문처럼 입구 칸에 올라서는 오브젝트는 입구 칸이 경비 칸이면 바닐라에서도 전투가 나므로 규칙 B 대상이 아니다.

## 구현

| 훅 | ERA 주소 | 내용 |
| --- | --- | --- |
| `searchArray::testPossibleDirections` (HiHook, thiscall, 9 args) | `0x4B2060` | 호출 동안 `source` 셀·영웅·search type을 기억 |
| `searchArray::pushPoint` (HiHook, thiscall, 8 args) | `0x4B17D0` | 위 `source`에서 트리거 칸으로 넣는 push 하나만 규칙에 따라 거부 |

거부되는 것은 `testPossibleDirections`가 트리거 칸 자체에 대해 하는 push뿐이다. 비행 영웅용으로 바닐라가 추가로 넣는 "오브젝트 위를 지나가는 공중 칸" push(`isTrigger = 0`)는 그대로 둔다. 커서, 경로 화살표, AI 모두 같은 검색 결과를 쓰므로 별도 패치가 필요 없다.

### 주소 근거

함수 본문은 [homm3-decomp](https://github.com/philipmendocino/homm3-decomp)(GOG Complete 4.0 빌드 기준 매칭 디컴파일)에서 읽었다. 그 빌드와 ERA exe는 오브젝트 파일 단위로 주소가 일정하게 어긋난다.

- `findpath.obj`: ERA = decomp − `0x2A0` (H3API `GetMovementCost` 0x4B1620 ↔ decomp `getTerrainCost` 0x4B18C0, NH3API `searchArray` 생성자/소멸자 0x4B10D0/0x4B1140, `getTravelTime` 0x4B3C80가 모두 일치)
- `search.obj`: ERA = decomp + `0x2D0` (NH3API `buildPath` 0x56A3A0, `seedPosition` 0x56B710, 그리고 이 저장소 `ExtendedMonoliths`의 0x56B151 훅이 `enter_trigger`의 양방향 모노리스 분기, 0x56A6D0이 `enterLith`에 정확히 대응)

플러그인은 시작할 때 실행 파일 바이트로 한 번 더 확인한다: `testPossibleDirections` 안의 `pushPoint` 호출 수(예상 3), `seedPosition` 안의 두 함수 호출(각 1), 오브젝트 특성 테이블 포인터 `0x660428` 참조(예상 3). 하나라도 0이면 훅을 설치하지 않고 로그만 남긴다.

### pathCell의 canStop 비트

PC 리테일은 bit 9에 "시작 트리거" 플래그가 끼어 있어서 canStop이 bit 10이다(homm3-decomp). NH3API는 Dreamcast 기반 레이아웃이라 bit 9로 적혀 있다. 그래서 플러그인은 `seedPosition`이 시작 칸을 자기 자신에 push하는 순간(비행·수면보행이 아니거나 평지 위)에 어느 쪽인지 판별하고, 판별 전에는 아무것도 거부하지 않는다. 결과는 ERA 로그에 `pathCell.canStop detected at bit N`으로 남는다.

## 설정

ERA JSON(모드의 `Lang/*.json`)에서 읽는다. 키가 없으면 기본값.

```json
{
  "hota_fly_landing": {
    "enabled": 1,
    "landing_rule": 1,
    "landing_rule_on_water": 1,
    "guard_rule": 1,
    "apply_to_ai": 1,
    "allow_teleporters_from_air": 0,
    "open_border_gates_passable": 0,
    "can_stop_bit": -1
  }
}
```

| 키 | 기본 | 의미 |
| --- | --- | --- |
| `enabled` | 1 | 0이면 훅을 설치하지 않음 |
| `landing_rule` | 1 | 규칙 A |
| `landing_rule_on_water` | 1 | 물 위(수면보행/바다 위 비행)에서도 규칙 A 적용 |
| `guard_rule` | 1 | 규칙 B |
| `apply_to_ai` | 1 | AI 길찾기에도 적용 |
| `allow_teleporters_from_air` | 0 | 모노리스·지하 관문·소용돌이는 공중에서 진입 허용 |
| `open_border_gates_passable` | 0 | 열쇠를 가진 Border Gate를 아군 주둔지처럼 예외 처리. 1로 하면 열쇠 보유 영웅은 Quest Guard로 봉쇄된 관문을 공중에서 통과할 수 있음 |
| `can_stop_bit` | -1 | -1 자동 감지, 9 또는 10 강제 |

## 테스트 체크리스트

1. 게임 시작 후 ERA 로그에서 `HotaFlyLanding` 항목 확인: 호출 수 `3 / 1 / 1 / 3`, 그리고 `canStop detected at bit 10`(또는 9).
2. 장애물로 둘러싸이고 입구 앞 칸도 장애물인 광산/생산소 → 비행으로 클릭 시 도달 불가.
3. Quest Guard로 양쪽이 봉쇄된 Border Gate, 열쇠 보유 영웅 → 공중에서 통과 불가.
4. 입구 앞 칸이 평지인 같은 오브젝트 → 평소처럼 방문 가능.
5. 경비 몬스터 옆 평지 칸에서만 닿는 판도라 상자/유물 → 오브젝트를 클릭해도 경로가 생기지 않음. 그 칸을 클릭해 착지하면 전투, 승리 후 획득 가능.
6. 비행으로 몬스터 옆을 *지나가기만* 하는 경로 → 바닐라처럼 허용.
7. 아군 주둔지를 공중 칸에서 통과 → 허용.
8. 수면보행으로 물 위에서 해안 오브젝트 방문 → `landing_rule_on_water`에 따라 거부/허용.
9. AI 턴을 몇 번 돌려 AI가 비행으로 막힌 오브젝트를 노리며 제자리걸음하지 않는지 확인. 이상하면 `apply_to_ai: 0`.

## 범위 밖

- HotA의 차원의 문(Dimension Door) 규칙(경비 칸으로 순간이동하면 전투)은 길찾기가 아니라 주문 시전 처리라서 포함하지 않았다.
- 비행 이동을 키 입력으로 중간에 끊었을 때 그 칸이 경비 칸이면 전투가 나도록 한 HotA 수정도 포함하지 않았다(바닐라 `endMoveHero` 그대로).
