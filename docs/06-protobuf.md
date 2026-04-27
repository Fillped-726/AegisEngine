# Protobuf 协议 — 消息定义与 MsgID 枚举

> 对应目录: `shared/proto/`

---

## 1. 文件结构

```
shared/proto/
├── common.proto          # 通用类型：PBVector3 / PlayerState / PBPlayerInfo
├── ids.proto             # MsgID 枚举（所有消息的 ID 映射）
├── cs_lobby.proto        # 大厅/登录/营地管理 (CS: Client→Server, SC/ S2C: Server→Client)
├── cs_battle.proto       # 战斗/移动/技能 (CS/SC)
└── ss_bridge.proto       # 服务端内部 RPC (SS: Server→Server)
```

---

## 2. 通用定义 (`common.proto`)

```protobuf
package aegis.common;

message PBVector3 {
    float x = 1; float y = 2; float z = 3;
}

enum PlayerState {
    IDLE = 0; MOVE = 1; ATTACK = 2; DEAD = 3;
}

message PBPlayerInfo {
    uint64 entity_id = 1;
    PBVector3 pos = 2;
    float direction = 3;
    string name = 4;
    int32 skin_id = 5;
    int32 hp = 6;
    PlayerState state = 7;
    uint32 entity_type = 8;  // 0: Player, 1: NPC, 2: Monster, 3: Boss
}
```

---

## 3. MsgID 枚举 (`ids.proto`)

```protobuf
package aegis.ids;

enum MsgID {
    UNKNOWN = 0;

    // ── 1000-1999: Lobby / Login ──
    CS_LOGIN_REQ            = 1001;  // C→S: 登录请求
    SC_LOGIN_RES            = 1002;  // S→C: 登录响应

    // ── 1100-1199: Camp (营地管理) ──
    C2S_CREATE_CAMP_REQ     = 1101;  // C→S: 创建营地
    S2C_CREATE_CAMP_RES     = 1102;  // S→C: 创建结果
    C2S_JOIN_CAMP_REQ       = 1103;  // C→S: 加入营地
    S2C_JOIN_CAMP_RES       = 1104;  // S→C: 加入结果
    S2C_CAMP_SNAPSHOT       = 1105;  // S→C: 营地快照补帧
    C2S_QUERY_CAMP_LIST_REQ = 1106;  // C→S: 查询营地列表
    S2C_QUERY_CAMP_LIST_RES = 1107;  // S→C: 营地列表响应

    // ── 2000-2999: Battle / Gameplay ──
    CS_PING                 = 2001;  // C→S: 心跳 Ping
    SC_PONG                 = 2002;  // S→C: 心跳 Pong
    CS_MOVE_REQ             = 2003;  // C→S: 移动请求
    SC_MOVE_NTF             = 2004;  // S→C: 移动通知 (单包)
    SC_ENTER_VIEW           = 2005;  // S→C: 进入视野
    SC_LEAVE_VIEW           = 2006;  // S→C: 离开视野
    CS_UPDATE_STATE_REQ     = 2007;  // C→S: 更新状态
    SC_STATE_UPDATE_BATCH   = 2008;  // S→C: 状态批量更新
    SC_MOVE_NTF_BATCH       = 2009;  // S→C: 移动批量通知
    CS_SKILL_CAST_REQ       = 2010;  // C→S: 技能释放
    SC_DAMAGE_NTF           = 2011;  // S→C: 伤害通知

    // ── 3000-3999: Server-to-Server ──
    SS_CREATE_ROOM_REQ      = 3001;  // 创建房间 RPC
    SS_TERMINATE_ROOM_REQ   = 3002;  // 销毁房间 RPC
}
```

---

## 4. 大厅协议 (`cs_lobby.proto`)

```protobuf
package aegis.cs.lobby;
import "common.proto";

// ── 登录 ──
message LoginReq    { int32 uid = 1; string token = 2; }
message LoginRes    { int32 ret_code = 1; string msg = 2; uint64 entity_id = 3; PBVector3 pos = 4; }

// ── 踢人 ──
message S2C_KickOut { string reason = 1; }

// ── 营地快照 ──
message S2C_CampSnapshotPush { uint64 scene_actor_id = 1; repeated PBPlayerInfo players = 2; }

// ── 创建营地 ──
message C2S_CreateCampReq { string camp_name = 1; uint32 map_id = 2; }
message S2C_CreateCampRes { int32 ret_code = 1; string msg = 2; uint64 scene_actor_id = 3; string camp_name = 4; }

// ── 加入营地 ──
message C2S_JoinCampReq { uint64 scene_actor_id = 1; }
message S2C_JoinCampRes { int32 ret_code = 1; string msg = 2; uint64 scene_actor_id = 3; }

// ── 查询营地列表 ──
message C2S_QueryCampListReq { int32 page_index = 1; }
message CampInfo { uint64 scene_actor_id = 1; string camp_name = 2; int32 current_players = 3; int32 max_players = 4; }
message S2C_QueryCampListRes { repeated CampInfo camps = 1; int32 total_count = 2; }
```

---

## 5. 战斗协议 (`cs_battle.proto`)

```protobuf
package aegis.cs.battle;
import "common.proto";

// ── 移动请求 ──
message CSMoveReq { PBVector3 target_pos = 1; float direction = 2; int64 timestamp = 3; }

// ── 移动批量通知 ──
message SCMoveNtfBatch {
    message MoveInfo { uint64 entity_id = 1; PBVector3 pos = 2; float direction = 3; }
    repeated MoveInfo moves = 1;
}

// ── 视野 ──
message SCEnterViewNtf { repeated PBPlayerInfo entities = 1; }
message SCLeaveViewNtf { repeated uint64 entity_ids = 1; }

// ── 状态 ──
message CSUpdateStateReq { PlayerState state = 1; }
message SCStateUpdateBatch {
    message StateUpdateInfo { uint64 entity_id = 1; PlayerState state = 2; }
    repeated StateUpdateInfo updates = 1;
}

// ── 技能 ──
message CSSkillCastReq { uint32 skill_id = 1; uint64 target_id = 2; PBVector3 target_pos = 3; float direction = 4; }

// ── 伤害事件 ──
message SCDamageNtf { uint64 attacker_id = 1; uint64 target_id = 2; uint32 skill_id = 3; int32 damage = 4; int32 current_hp = 5; bool is_crit = 6; }
```

> **注意**: `SCDamageNtf` 是**事件流**（Event）不是**状态流**（State）。伤害必须独立发包，不能合并到状态同步中，否则客户端飘字会丢失。

---

## 6. 服务端内部 RPC (`ss_bridge.proto`)

```protobuf
package aegis.ss.bridge;

message RpcResponse { int32 code = 1; string msg = 2; int64 timestamp = 3; }

message CreateRoomReq    { uint32 room_id = 1; uint32 map_id = 2; repeated uint64 player_ids = 3; uint64 player_actor_id = 4; }
message CreateRoomRes    { RpcResponse header = 1; uint32 room_id = 2; string battle_server_ip = 3; int32 battle_server_port = 4; string token = 5; }

message TerminateRoomReq { uint32 room_id = 1; uint64 player_actor_id = 4; }
message TerminateRoomRes { RpcResponse header = 1; }

message ServerStatusReq  {}
message ServerStatusRes  { RpcResponse header = 1; int32 active_rooms = 2; int32 active_players = 3; float cpu_load = 4; int64 memory_usage = 5; }
```
