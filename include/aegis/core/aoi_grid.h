#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <ranges>
#include <concepts>
#include "aegis/common/aegisLog.h" // 假设你有日志库
#include "aegis/common/spinLock.h"

namespace aegis::core
{

    // 使用别名增加代码语义可读性
    using EntityId = uint64_t;

    /**
     * @brief 表示网格中的单个单元格
     * 保持简单 POD (Plain Old Data) 风格，方便扩展锁机制
     */
    struct AOICell
    {
        mutable aegis::common::SpinLock lock;
        std::vector<EntityId> entities;
        // 1. 显式默认构造函数（必须写，因为写了下面的构造函数后，编译器不会自动生成默认的了）
        AOICell() = default;

        // 2. 自定义移动构造函数 (The SSP Fix)
        // noexcept 是必须的，否则 vector 为了异常安全可能会退化成拷贝，导致再次报错
        AOICell(AOICell &&other) noexcept
            : entities(std::move(other.entities))
        // 注意：这里不初始化 lock，它会自动调用 SpinLock 的默认构造函数。
        // 含义：新 Cell 诞生时，自带一把全新的、未上锁的锁。
        // 旧 Cell 的锁（other.lock）会被留在那儿，随 other 析构消亡。
        {
        }
    };

    /**
     * @brief 基于网格的高性能 AOI 管理器
     * 特性：
     * 1. 扁平化内存布局 (Flat Layout)
     * 2. O(1) 插入与删除 (Swap-and-Pop)
     * 3. 预计算浮点倒数优化除法性能
     */
    class AOIGrid
    {
    public:
        AOIGrid(float width, float height, float cellSize)
            : width_(width), height_(height), cellSize_(cellSize)
        {
            if (cellSize_ <= 0.0f)
            {
                // 实际项目中应抛出异常或 Panic
                return;
            }

            // 预计算倒数，将除法转换为乘法，提升 GetIndex 性能
            invCellSize_ = 1.0f / cellSize_;

            // 计算行列数 (向上取整)
            colCount_ = static_cast<uint32_t>(std::ceil(width_ / cellSize_));
            rowCount_ = static_cast<uint32_t>(std::ceil(height_ / cellSize_));

            // 初始化扁平化网格
            cells_.resize(colCount_ * rowCount_);
        }

        ~AOIGrid() = default;

        // 禁止拷贝，允许移动 (Standard Practice for Resource Managers)
        AOIGrid(const AOIGrid &) = delete;
        AOIGrid &operator=(const AOIGrid &) = delete;
        AOIGrid(AOIGrid &&) = default;
        AOIGrid &operator=(AOIGrid &&) = default;

        /**
         * @brief 向指定坐标所在的格子添加实体
         * Time Complexity: O(1) Amortized
         */
        bool Add(EntityId id, float x, float y)
        {
            if (!isValidPos(x, y)) [[unlikely]]
            {
                return false;
            }

            uint32_t index = getIndexUnsafe(x, y);
            AOICell &cell = cells_[index];

            // 临界区：极短，仅涉及 vector push_back
            {
                std::lock_guard<aegis::common::SpinLock> guard(cell.lock);
                cell.entities.push_back(id);
            }
            return true;
        }

        /**
         * @brief 从指定坐标所在的格子移除实体
         * Time Complexity: O(1) - 使用 Swap-and-Pop
         * @note 调用者必须保证 (x, y) 是实体当前所在的坐标
         */
        bool Remove(EntityId id, float x, float y)
        {
            if (!isValidPos(x, y)) [[unlikely]]
            {
                return false;
            }

            uint32_t index = getIndexUnsafe(x, y);
            AOICell &cell = cells_[index];
            // 临界区：查找 + Swap-Pop
            {
                std::lock_guard<aegis::common::SpinLock> guard(cell.lock);
                auto &entities = cell.entities;
                auto it = std::ranges::find(entities, id);
                if (it != entities.end()) [[likely]]
                {
                    *it = entities.back();
                    entities.pop_back();
                    return true;
                }
            }

            return false;
        }

        // ---------------------------------------------------------
        // Phase 3: Move & Diff (The Core AOI Logic)
        // ---------------------------------------------------------

        /**
         * @brief 处理实体移动，并计算视野变化 (Diff)
         * * @param id 移动的实体 ID
         * @param oldX, oldY 旧坐标
         * @param newX, newY 新坐标
         * @param outEnterView [Output] 结果：哪些新实体进入了我的视野
         * @param outLeaveView [Output] 结果：哪些旧实体离开了我的视野
         * @return true 如果移动成功且坐标有效
         */
        bool Move(EntityId id, float oldX, float oldY, float newX, float newY,
                  std::vector<EntityId> &outEnterView,
                  std::vector<EntityId> &outLeaveView)
        {
            outEnterView.clear();
            outLeaveView.clear();

            // 0. 基础校验
            if (!isValidPos(newX, newY)) [[unlikely]]
                return false;
            // 如果旧坐标无效，视作新加入，直接调 Add (但在 Move 语义下通常假设旧坐标合法)
            if (!isValidPos(oldX, oldY)) [[unlikely]]
                return Add(id, newX, newY);

            uint32_t oldIndex = getIndexUnsafe(oldX, oldY);
            uint32_t newIndex = getIndexUnsafe(newX, newY);

            // 1. 同一个格子：Fast Path
            if (oldIndex == newIndex)
            {
                // 格子没变，意味着 9 宫格也没变。
                // 此时没有 Enter/Leave 事件产生，无需操作 Grid 数据结构。
                // 仅仅是实体内部坐标变了，AOI 模块不感知。
                return true;
            }

            // 2. 跨格子移动：数据结构更新 (Slow Path)
            // 策略：先删后加。
            // 注意：这里有两个独立的锁操作。存在极短暂的"消失"间隙，但在游戏高并发场景通常优于死锁风险。
            Remove(id, oldX, oldY);
            Add(id, newX, newY);

            // 3. 计算 Diff：谁进入了视野？谁离开了视野？
            // 优化核心：Diff Grids instead of Entities.

            // 获取新旧 9 宫格的索引列表 (通常是 9 个，边界处可能少于 9 个)
            // 为了利用 set_difference，我们需要它们是有序的。
            // GetNeighborGridIndices 内部生成逻辑是按行列循环的，天然有序 (Row-Major Order)。
            auto oldGrids = GetNeighborGridIndices(oldX, oldY);
            auto newGrids = GetNeighborGridIndices(newX, newY);

            // 临时容器存放格子索引的差集
            // Stack memory (small buffer) optimization could be used here, but vector is safe MVP.
            std::vector<uint32_t> addedGrids;
            std::vector<uint32_t> removedGrids;
            addedGrids.reserve(9);
            removedGrids.reserve(9);

            // C++20 ranges set operations
            // set_difference: elements in A but not in B
            std::ranges::set_difference(newGrids, oldGrids, std::back_inserter(addedGrids));
            std::ranges::set_difference(oldGrids, newGrids, std::back_inserter(removedGrids));

            // 4. 填充结果实体
            // 新视野 = 只去读那些"新增的格子"里的实体
            for (uint32_t gridIdx : addedGrids)
            {
                const AOICell &cell = cells_[gridIdx];
                std::lock_guard<aegis::common::SpinLock> guard(cell.lock);
                outEnterView.insert(outEnterView.end(), cell.entities.begin(), cell.entities.end());
            }

            // 旧视野 = 只去读那些"移除的格子"里的实体
            for (uint32_t gridIdx : removedGrids)
            {
                const AOICell &cell = cells_[gridIdx];
                std::lock_guard<aegis::common::SpinLock> guard(cell.lock);
                outLeaveView.insert(outLeaveView.end(), cell.entities.begin(), cell.entities.end());
            }

            return true;
        }

        // ---------------------------------------------------------
        // View Logic (Phase 2 New Features)
        // ---------------------------------------------------------

        /**
         * @brief 获取指定坐标周围（9宫格）的所有实体 ID
         * @param out_result 输出参数，由调用者提供 vector 以复用内存
         * @note 这是 AOI 系统中最高频的调用，必须极致优化
         */
        void GetViewEntityIds(float x, float y, std::vector<EntityId> &out_result) const
        {
            out_result.clear();

            if (!isValidPos(x, y)) [[unlikely]]
            {
                return;
            }

            // 1. 计算中心格子的行列
            // 这里的 static_cast 比 floor 快，因为坐标是非负的
            uint32_t centerCol = static_cast<uint32_t>(x * invCellSize_);
            uint32_t centerRow = static_cast<uint32_t>(y * invCellSize_);

            // 2. 计算 9 宫格的边界 (Clamping)
            // 处理地图边缘情况：左下角只有 3 个邻居，边缘有 5 个，内部有 8 个
            uint32_t startCol = (centerCol > 0) ? centerCol - 1 : 0;
            uint32_t endCol = (centerCol + 1 < colCount_) ? centerCol + 1 : colCount_ - 1;

            uint32_t startRow = (centerRow > 0) ? centerRow - 1 : 0;
            uint32_t endRow = (centerRow + 1 < rowCount_) ? centerRow + 1 : rowCount_ - 1;

            // 预估一个容量，避免 push_back 导致多次 realloc
            // 假设平均每个格子有 N 个实体，我们即将访问 (endRow-startRow+1)*(endCol-startCol+1) 个格子
            // 这里做一个简单的启发式预留，假设每个格子平均 5 个实体
            // TODO: 可以做成自适应的 profiling
            // out_result.reserve(50);

            // 3. 遍历所有邻居格子
            for (uint32_t r = startRow; r <= endRow; ++r)
            {
                // 优化：计算当前行的基准索引
                uint32_t rowBaseIndex = r * colCount_;

                for (uint32_t c = startCol; c <= endCol; ++c)
                {
                    uint32_t index = rowBaseIndex + c;
                    const AOICell &cell = cells_[index];

                    // Locking Strategy:
                    // 我们逐个锁格子，而不是一次性锁 9 个。
                    // 优点：极大降低锁竞争概率，避免死锁（不需要复杂的加锁顺序）。
                    // 缺点：可能读到稍不一致的视图（Snapshot Inconsistency），但在游戏 AOI 场景中完全可接受。
                    std::lock_guard<aegis::common::SpinLock> guard(cell.lock);

                    // 批量拷贝
                    out_result.insert(out_result.end(), cell.entities.begin(), cell.entities.end());
                }
            }
        }

        /**
         * @brief 辅助函数：仅获取邻居的 Grid Index (用于调试或高级逻辑)
         */
        [[nodiscard]] std::vector<uint32_t> GetNeighborGridIndices(float x, float y) const
        {
            std::vector<uint32_t> indices;
            if (!isValidPos(x, y))
                return indices;

            uint32_t centerCol = static_cast<uint32_t>(x * invCellSize_);
            uint32_t centerRow = static_cast<uint32_t>(y * invCellSize_);

            uint32_t startCol = (centerCol > 0) ? centerCol - 1 : 0;
            uint32_t endCol = (centerCol + 1 < colCount_) ? centerCol + 1 : colCount_ - 1;
            uint32_t startRow = (centerRow > 0) ? centerRow - 1 : 0;
            uint32_t endRow = (centerRow + 1 < rowCount_) ? centerRow + 1 : rowCount_ - 1;

            indices.reserve(9);
            for (uint32_t r = startRow; r <= endRow; ++r)
            {
                for (uint32_t c = startCol; c <= endCol; ++c)
                {
                    indices.push_back(r * colCount_ + c);
                }
            }
            return indices;
        }

        /**
         * @brief 调试用：获取某个格子的实体列表
         */
        [[nodiscard]] const std::vector<EntityId> &GetEntitiesInCell(float x, float y) const
        {
            static const std::vector<EntityId> empty;
            if (!isValidPos(x, y))
            {
                return empty;
            }
            auto &cell = cells_[getIndexUnsafe(x, y)];
            {
                std::lock_guard<aegis::common::SpinLock> guard(cell.lock);
                return cell.entities;
            }
        }

    private:
        /**
         * @brief 核心映射算法：坐标 -> 数组索引
         * @note 这是热点代码路径，不做边界检查，由调用者保证
         */
        [[nodiscard]] inline uint32_t getIndexUnsafe(float x, float y) const
        {
            // 利用预计算的 invCellSize_ 进行乘法运算
            uint32_t col = static_cast<uint32_t>(x * invCellSize_);
            uint32_t row = static_cast<uint32_t>(y * invCellSize_);

            // 扁平化映射: y * width + x
            return row * colCount_ + col;
        }

        [[nodiscard]] inline bool isValidPos(float x, float y) const
        {
            return x >= 0.0f && x < width_ && y >= 0.0f && y < height_;
        }

    private:
        float width_;
        float height_;
        float cellSize_;
        float invCellSize_; // Optimization: 1 / cellSize

        uint32_t colCount_;
        uint32_t rowCount_;

        // 扁平化的一维数组，模拟二维网格
        std::vector<AOICell> cells_;
    };

} // namespace aegis::core