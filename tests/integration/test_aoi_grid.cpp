#include <iostream>
#include <vector>
#include <cassert>
#include <source_location>
#include <string_view>

// Include your implementation
#include "aegis/game/aoi_grid.h"

using namespace aegis::core;

// ==========================================
// Minimal Test Harness (模拟简单的测试框架)
// ==========================================
void LogTestPass(std::string_view name)
{
    std::cout << "[PASS] " << name << std::endl;
}

void LogTestFail(std::string_view name, std::string_view reason, std::source_location loc = std::source_location::current())
{
    std::cerr << "[FAIL] " << name << ": " << reason
              << " (" << loc.file_name() << ":" << loc.line() << ")" << std::endl;
    std::exit(1);
}

#define EXPECT_TRUE(cond) \
    if (!(cond))          \
        LogTestFail("Check True", #cond);
#define EXPECT_FALSE(cond) \
    if (cond)              \
        LogTestFail("Check False", #cond);
#define EXPECT_EQ(a, b)                      \
    if ((a) != (b))                          \
        LogTestFail("Check Equal", #a " == " \
                                      "b");

// ==========================================
// Test Cases
// ==========================================

void Test_BasicAddRemove()
{
    // 1. 初始化 100x100 的地图，格子大小 10
    // 这意味着地图被分为 10x10 = 100 个格子
    AOIGrid grid(100.0f, 100.0f, 10.0f);

    // 2. 添加实体 100 到 (50, 50)
    // 索引计算预期: col=5, row=5. index = 5*10 + 5 = 55
    bool addResult = grid.Add(100, 50.0f, 50.0f);
    EXPECT_TRUE(addResult);

    // 3. 验证存在
    const auto &entities = grid.GetEntitiesInCell(50.0f, 50.0f);
    EXPECT_EQ(entities.size(), 1);
    EXPECT_EQ(entities[0], 100);

    // 4. 验证其他格子为空 (防止索引计算错误)
    const auto &emptyCell = grid.GetEntitiesInCell(0.0f, 0.0f);
    EXPECT_EQ(emptyCell.size(), 0);

    // 5. 删除实体
    bool removeResult = grid.Remove(100, 50.0f, 50.0f);
    EXPECT_TRUE(removeResult);

    // 6. 验证删除后为空
    const auto &entitiesAfterRemove = grid.GetEntitiesInCell(50.0f, 50.0f);
    EXPECT_EQ(entitiesAfterRemove.size(), 0);

    LogTestPass("Test_BasicAddRemove");
}

void Test_SwapAndPopLogic()
{
    AOIGrid grid(100.0f, 100.0f, 10.0f);

    // 添加 A(10), B(20), C(30) 到同一个格子
    grid.Add(10, 5.0f, 5.0f);
    grid.Add(20, 5.0f, 5.0f);
    grid.Add(30, 5.0f, 5.0f);

    auto &vec = grid.GetEntitiesInCell(5.0f, 5.0f);
    EXPECT_EQ(vec.size(), 3);
    // 此时顺序应该是 10, 20, 30

    // 删除中间的 B(20)
    // Swap-and-Pop 逻辑：应该把尾部的 C(30) 搬到 B(20) 的位置，然后 pop
    grid.Remove(20, 5.0f, 5.0f);

    EXPECT_EQ(vec.size(), 2);

    // 验证剩余元素是否为 10 和 30 (顺序不敏感，但通常 vector 变成了 {10, 30})
    bool has10 = false;
    bool has30 = false;
    for (auto id : vec)
    {
        if (id == 10)
            has10 = true;
        if (id == 30)
            has30 = true;
    }
    EXPECT_TRUE(has10);
    EXPECT_TRUE(has30);

    LogTestPass("Test_SwapAndPopLogic");
}

void Test_BoundaryCheck()
{
    AOIGrid grid(100.0f, 100.0f, 10.0f);

    // 负坐标
    EXPECT_FALSE(grid.Add(1, -1.0f, 50.0f));

    // 超出边界
    EXPECT_FALSE(grid.Add(1, 150.0f, 50.0f));

    // 边界值 (99.99 应该在最后一个格子, 100.0 视作越界或边界处理，这里代码是 < width_)
    EXPECT_TRUE(grid.Add(1, 99.9f, 99.9f));
    EXPECT_EQ(grid.GetEntitiesInCell(99.9f, 99.9f).size(), 1);

    LogTestPass("Test_BoundaryCheck");
}

void Test_9Grid_Neighbors()
{
    // 1. 初始化地图
    // 100x100 大小，格子大小 10 => 10x10 个格子
    // Grid(col, row) 索引范围: col [0-9], row [0-9]
    AOIGrid grid(100.0f, 100.0f, 10.0f);

    // 2. 场景布置
    // -------------------------------------------------
    // | (0,2) ID:99 | ...
    // | [OUT OF VIEW]|
    // -------------------------------------------------
    // | (0,1) ID:20 | (1,1)       | ...
    // | [Neighbor]  | [Neighbor]  |
    // -------------------------------------------------
    // | (0,0) ID:10 | (1,0) ID:30 | (2,0) ...
    // | [CENTER]    | [Neighbor]  |
    // -------------------------------------------------

    // Center: (5, 5) -> Grid[0,0]
    grid.Add(10, 5.0f, 5.0f);

    // Top: (5, 15) -> Grid[0,1] (y 在 10-20 之间)
    grid.Add(20, 5.0f, 15.0f);

    // Right: (15, 5) -> Grid[1,0] (x 在 10-20 之间)
    grid.Add(30, 15.0f, 5.0f);

    // Far Away: (5, 25) -> Grid[0,2] (Row 2)
    // Row 0 的邻居是 Range[0, 1]，Row 2 应该不可见
    grid.Add(99, 5.0f, 25.0f);

    // 3. 执行查询
    // 查 (5,5) 所在的格子及其周围
    std::vector<uint64_t> results;
    // 预留空间是个好习惯，虽然这里逻辑上不需要显式测试它
    results.reserve(10);

    grid.GetViewEntityIds(5.0f, 5.0f, results);

    // 4. 验证结果
    // 期望结果：包含 10, 20, 30。不包含 99。
    EXPECT_EQ(results.size(), 3);

    // 由于多线程/SwapPop导致顺序不确定，先排序再比较
    std::ranges::sort(results);

    // 检查具体 ID
    bool found10 = std::ranges::binary_search(results, 10);
    bool found20 = std::ranges::binary_search(results, 20);
    bool found30 = std::ranges::binary_search(results, 30);
    bool found99 = std::ranges::binary_search(results, 99);

    EXPECT_TRUE(found10);
    EXPECT_TRUE(found20);
    EXPECT_TRUE(found30);
    EXPECT_FALSE(found99); // 关键：确保没看到太远的

    LogTestPass("Test_9Grid_Neighbors");
}

void Test_Move_Diff()
{
    AOIGrid grid(100.0f, 100.0f, 10.0f);

    // 场景 setup:
    // Cell A (0,0): (5,5) -> ID: 10 (Mover)
    // Cell B (0,1): (5,15) -> ID: 20 (Static)
    // Cell C (2,0): (25,5) -> ID: 30 (Far Right)

    // 初始状态：10 在 (0,0).
    // 10 能看到 20 (在 (0,1)).
    // 10 看不到 30 (在 (2,0), 因为 (0,0) 的邻居最远到 (1,x)).

    grid.Add(10, 5.0f, 5.0f);
    grid.Add(20, 5.0f, 15.0f);
    grid.Add(30, 25.0f, 5.0f);

    // 动作：10 从 (5,5) 瞬移到 (15,5) -> 即从 Grid[0,0] 走到 Grid[1,0]
    // New Grid[1,0] 的邻居包括：Col 0, 1, 2.
    // 所以 ID:30 (在 Grid[2,0]) 应该进入视野 (Enter).
    // ID:20 (在 Grid[0,1]) 依然在视野内 (Keep).
    // 没有人离开视野 (因为 Col 0 依然是 Col 1 的邻居).

    std::vector<uint64_t> enter;
    std::vector<uint64_t> leave;

    grid.Move(10, 5.0f, 5.0f, 15.0f, 5.0f, enter, leave);

    // 验证 Enter
    bool has30 = std::ranges::find(enter, 30) != enter.end();
    EXPECT_TRUE(has30);
    EXPECT_EQ(enter.size(), 1); // 应该只新看到了 30

    // 验证 Leave
    // 从 Grid[0,0] (Col 0,1) -> Grid[1,0] (Col 0,1,2).
    // 原视野: Cols 0,1. 新视野: Cols 0,1,2.
    // 差集 Old - New: 无. (假设左边没有边界外的实体)
    EXPECT_EQ(leave.size(), 0);

    // 再走一步：从 (15,5) [Grid 1,0] 走到 (25,5) [Grid 2,0]
    // Old View (Center 1,0): Cols 0, 1, 2
    // New View (Center 2,0): Cols 1, 2, 3
    // Leave: Col 0 的实体 (这里没有实体在 Col 0 除了旧位置? 不对，20 在 (0,1) 即 Grid[0,1])
    // Wait, 20 is at (5,15) -> Grid[0,1].
    // Move 10 from Grid[1,0] to Grid[2,0].
    // Grid[2,0]'s neighbors: Cols 1, 2, 3.
    // Grid[0,1] is at Col 0. Col 0 is NOT a neighbor of Col 2.
    // So 20 should LEAVE view.

    grid.Move(10, 15.0f, 5.0f, 25.0f, 5.0f, enter, leave);

    bool has20_leave = std::ranges::find(leave, 20) != leave.end();
    EXPECT_TRUE(has20_leave);

    LogTestPass("Test_Move_Diff");
}

int main()
{
    std::cout << "Running AOIGrid Tests..." << std::endl;
    std::cout << "-----------------------" << std::endl;

    Test_BasicAddRemove();
    Test_SwapAndPopLogic();
    Test_BoundaryCheck();
    Test_9Grid_Neighbors();
    Test_Move_Diff();

    std::cout << "-----------------------" << std::endl;
    std::cout << "All Tests Passed." << std::endl;
    return 0;
}