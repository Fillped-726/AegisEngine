---
version: alpha
name: AegisEngine Design Document
description: 轻量级 ARPG 联机游戏 — 服务端 C++20+io_uring+Actor 模型，客户端 Godot 4.6.2 Mono C#。房主建房模式，集会所社交 + 副本打怪掉宝。
---

所有内容与客户端 `/mnt/c/D/project/aegis-client/DESIGN.md` 保持一致。

关键章节索引：
- 游戏循环 → 第二章
- 战斗系统 → 第三章
- 服务端架构 → 第五章
- 协议定义 → 第六章
- 已知问题 → 第七章

**当前服务器端未实现的关键问题：**
1. SceneActor::OnHandleSkillCast 没有对 NPC 执行伤害计算（Priority: HIGH）
2. 副本 SceneActor 的 OnTick 可能未正确调度 NPC AI（Priority: HIGH）
3. 副本结算（S2C_DungeonCompleteNtf）尚未发送（Priority: MEDIUM）
