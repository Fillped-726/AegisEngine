/**
 * @file aoi_grid.h
 * @brief Grid-based Area of Interest (AOI) management with 9-cell neighbor traversal.
 *        Supports negative coordinates via offsetX/offsetY.
 */
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
     * @brief Grid-based Area of Interest (AOI) spatial partitioning.
     * 
     * Supports arbitrary map ranges: [minX, maxX] x [minY, maxY].
     * Internally maps world coordinates to non-negative grid indices
     * via offsetX/offsetY.
     */
    class AOIGrid
    {
    public:
        /**
         * @param minX  地图最小 X 坐标（例如 -1000）
         * @param minY  地图最小 Y 坐标（例如 -1000）
         * @param maxX  地图最大 X 坐标（例如 1000）
         * @param maxY  地图最大 Y 坐标（例如 1000）
         * @param cellSize  网格大小（例如 256）
         */
        AOIGrid(float minX, float minY, float maxX, float maxY, float cellSize);
        ~AOIGrid() = default;

        AOIGrid(const AOIGrid &) = delete;
        AOIGrid &operator=(const AOIGrid &) = delete;
        AOIGrid(AOIGrid &&) = default;
        AOIGrid &operator=(AOIGrid &&) = default;

        void reset(float minX, float minY, float maxX, float maxY, float cellSize);

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
         * @brief 核心映射算法：世界坐标 -> 网格数组索引
         * 偏移 offsetX/offsetY 后确保值非负。
         * 超出地图范围的坐标统一返回 invalid (uint32_t)-1。
         */
        [[nodiscard]] inline uint32_t getIndexUnsafe(float x, float y) const
        {
            float tx = x + offsetX_;
            float ty = y + offsetY_;
            if (tx < 0.0f || ty < 0.0f || tx >= width_ || ty >= height_)
                return (uint32_t)-1;
            uint32_t col = static_cast<uint32_t>(tx * invCellSize_);
            uint32_t row = static_cast<uint32_t>(ty * invCellSize_);
            if (col >= colCount_ || row >= rowCount_)
                return (uint32_t)-1;
            return row * colCount_ + col;
        }

        [[nodiscard]] inline bool isValidPos(float x, float y) const
        {
            float tx = x + offsetX_;
            float ty = y + offsetY_;
            return tx >= 0.0f && ty >= 0.0f && tx < width_ && ty < height_;
        }

        void init_internal();

    public:
        /**
         * @brief 核心原语：9宫格遍历器
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
            uint32_t idx = getIndexUnsafe(x, y);
            if (idx == (uint32_t)-1) [[unlikely]]
                return;
            ForEachNeighborIndex(idx, std::forward<Func>(visitor));
        }

    private:
        float minX_;
        float minY_;
        float width_;       // maxX - minX
        float height_;      // maxY - minY
        float offsetX_;     // -minX
        float offsetY_;     // -minY
        float cellSize_;
        float invCellSize_;

        uint32_t colCount_;
        uint32_t rowCount_;

        std::vector<AOICell> cells_;
    };

} // namespace aegis::core
