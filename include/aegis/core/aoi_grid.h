#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "aegis/common/spinLock.h"

namespace aegis::core
{
    using EntityId = uint64_t;

    /**
     * @brief 表示网格中的单个单元格
     * 保持简单 POD (Plain Old Data) 风格，方便扩展锁机制
     */
    struct AOICell
    {
        std::vector<EntityId> entities;

        AOICell() = default;
        AOICell(AOICell &&other) noexcept;

        void clear();
    };

    /**
     * @brief 基于网格的高性能 AOI 管理器
     */
    class AOIGrid
    {
    public:
        AOIGrid(float width, float height, float cellSize);
        ~AOIGrid() = default;

        // 禁止拷贝，允许移动
        AOIGrid(const AOIGrid &) = delete;
        AOIGrid &operator=(const AOIGrid &) = delete;
        AOIGrid(AOIGrid &&) = default;
        AOIGrid &operator=(AOIGrid &&) = default;

        void reset(float width, float height, float cellSize);

        uint32_t Add(EntityId id, float x, float y);
        bool RemoveByGridIndex(EntityId id, uint32_t gridIndex);

        uint32_t Move(EntityId id, uint32_t oldIndex, float newX, float newY,
                      std::vector<EntityId> &outEnterView,
                      std::vector<EntityId> &outLeaveView);

        void GetViewEntityIds(float newX, float newY, std::vector<EntityId> &out_result) const;
        void GetViewEntityIds(uint32_t gridIndex, std::vector<EntityId> &out_result) const;

        [[nodiscard]] std::vector<uint32_t> GetNeighborGridIndices(float x, float y) const;
        [[nodiscard]] std::vector<uint32_t> GetNeighborGridIndices(uint32_t gridIndex) const;

    private:
        /**
         * @brief 核心映射算法：坐标 -> 数组索引
         * @note 这是热点代码路径，不做边界检查，由调用者保证
         */
        [[nodiscard]] inline uint32_t getIndexUnsafe(float x, float y) const
        {
            float clampedX = std::clamp(x, 0.0f, width_ - 0.001f);
            float clampedY = std::clamp(y, 0.0f, height_ - 0.001f);
            uint32_t col = static_cast<uint32_t>(clampedX * invCellSize_);
            uint32_t row = static_cast<uint32_t>(clampedY * invCellSize_);
            return row * colCount_ + col;
        }

        [[nodiscard]] inline bool isValidPos(float x, float y) const
        {
            constexpr float kEpsilon = 0.1f;
            return x >= -kEpsilon && x <= width_ + kEpsilon &&
                   y >= -kEpsilon && y <= height_ + kEpsilon;
        }

        void init_internal();

    public: // 将模板放在 private 下方或 public 中皆可，这里归类为核心遍历器
        /**
         * @brief 核心原语：9宫格遍历器
         * @note 模板函数必须在头文件中实现
         */
        template <typename Func>
        inline void ForEachNeighborIndex(uint32_t centerGridIndex, Func &&visitor) const
        {
            if (centerGridIndex >= cells_.size()) [[unlikely]]
                return;

            uint32_t centerCol = centerGridIndex % colCount_;
            uint32_t centerRow = centerGridIndex / colCount_;

            uint32_t startCol = (centerCol > 0) ? centerCol - 1 : 0;
            uint32_t endCol = (centerCol + 1 < colCount_) ? centerCol + 1 : colCount_ - 1;
            uint32_t startRow = (centerRow > 0) ? centerRow - 1 : 0;
            uint32_t endRow = (centerRow + 1 < rowCount_) ? centerRow + 1 : rowCount_ - 1;

            for (uint32_t r = startRow; r <= endRow; ++r)
            {
                uint32_t rowBase = r * colCount_;
                for (uint32_t c = startCol; c <= endCol; ++c)
                {
                    visitor(rowBase + c);
                }
            }
        }

        template <typename Func>
        inline void ForEachNeighborIndex(float x, float y, Func &&visitor) const
        {
            if (!isValidPos(x, y)) [[unlikely]]
                return;
            ForEachNeighborIndex(getIndexUnsafe(x, y), std::forward<Func>(visitor));
        }

    private:
        float width_;
        float height_;
        float cellSize_;
        float invCellSize_;

        uint32_t colCount_;
        uint32_t rowCount_;

        std::vector<AOICell> cells_;
    };

} // namespace aegis::core