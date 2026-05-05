// AOIGrid.cpp
#include "aegis/game/aoi_grid.h"
#include "aegis/common/aegisLog.h"
#include <ranges>

namespace aegis::core
{

    // ---------------------------------------------------------
    // AOICell 实现
    // ---------------------------------------------------------

    AOICell::AOICell(AOICell &&other) noexcept
        : entities(std::move(other.entities))
    {
    }

    void AOICell::clear()
    {
        entities.clear();
    }

    // ---------------------------------------------------------
    // AOIGrid 实现
    // ---------------------------------------------------------

    AOIGrid::AOIGrid(float minX, float minY, float maxX, float maxY, float cellSize)
        : minX_(minX), minY_(minY),
          width_(maxX - minX), height_(maxY - minY),
          offsetX_(-minX), offsetY_(-minY),
          cellSize_(cellSize)
    {
        init_internal();
    }

    void AOIGrid::reset(float minX, float minY, float maxX, float maxY, float cellSize)
    {
        minX_ = minX;
        minY_ = minY;
        width_ = maxX - minX;
        height_ = maxY - minY;
        offsetX_ = -minX;
        offsetY_ = -minY;

        bool dimChanged = (cellSize_ != cellSize);

        if (dimChanged)
        {
            cellSize_ = cellSize;
            init_internal();
        }

        for (auto &cell : cells_)
        {
            cell.clear();
        }
    }

    uint32_t AOIGrid::Add(EntityId id, float x, float y)
    {
        if (!isValidPos(x, y)) [[unlikely]]
        {
            return (uint32_t)-1;
        }

        uint32_t index = getIndexUnsafe(x, y);
        if (index == (uint32_t)-1) [[unlikely]]
            return (uint32_t)-1;
        cells_[index].entities.push_back(id);
        return index;
    }

    bool AOIGrid::RemoveByGridIndex(EntityId id, uint32_t gridIndex)
    {
        if (gridIndex >= cells_.size()) [[unlikely]]
        {
            return false;
        }

        auto &entities = cells_[gridIndex].entities;
        auto it = std::ranges::find(entities, id);

        if (it != entities.end()) [[likely]]
        {
            if (entities.size() > 1)
            {
                *it = entities.back();
            }
            entities.pop_back();
            return true;
        }

        return false;
    }

    uint32_t AOIGrid::Move(EntityId id, uint32_t oldIndex, float newX, float newY,
                           std::vector<EntityId> &outEnterView,
                           std::vector<EntityId> &outLeaveView)
    {
        outEnterView.clear();
        outLeaveView.clear();

        if (!isValidPos(newX, newY)) [[unlikely]]
            return (uint32_t)-1;

        if (oldIndex >= cells_.size() || oldIndex == (uint32_t)-1) [[unlikely]]
            return Add(id, newX, newY);

        uint32_t newIndex = getIndexUnsafe(newX, newY);
        if (newIndex == (uint32_t)-1) [[unlikely]]
            return (uint32_t)-1;

        if (oldIndex == newIndex)
        {
            return newIndex;
        }

        RemoveByGridIndex(id, oldIndex);
        Add(id, newX, newY);

        auto oldGrids = GetNeighborGridIndices(oldIndex);
        auto newGrids = GetNeighborGridIndices(newX, newY);

        std::vector<uint32_t> addedGrids;
        std::vector<uint32_t> removedGrids;
        addedGrids.reserve(9);
        removedGrids.reserve(9);

        std::ranges::set_difference(newGrids, oldGrids, std::back_inserter(addedGrids));
        std::ranges::set_difference(oldGrids, newGrids, std::back_inserter(removedGrids));

        for (uint32_t gridIdx : addedGrids)
        {
            const auto &ents = cells_[gridIdx].entities;
            outEnterView.insert(outEnterView.end(), ents.begin(), ents.end());
        }

        for (uint32_t gridIdx : removedGrids)
        {
            const auto &ents = cells_[gridIdx].entities;
            outLeaveView.insert(outLeaveView.end(), ents.begin(), ents.end());
        }

        return newIndex;
    }

    void AOIGrid::GetViewEntityIds(float newX, float newY, std::vector<EntityId> &out_result) const
    {
        out_result.clear();

        uint32_t gridIndex = getIndexUnsafe(newX, newY);
        if (gridIndex == (uint32_t)-1) [[unlikely]]
            return;

        ForEachNeighborIndex(gridIndex, [&](uint32_t neighborIdx)
                             {
            const auto &ents = cells_[neighborIdx].entities;
            out_result.insert(out_result.end(), ents.begin(), ents.end()); });
    }

    void AOIGrid::GetViewEntityIds(uint32_t gridIndex, std::vector<EntityId> &out_result) const
    {
        out_result.clear();

        ForEachNeighborIndex(gridIndex, [&](uint32_t neighborIdx)
                             {
            const auto &ents = cells_[neighborIdx].entities;
            out_result.insert(out_result.end(), ents.begin(), ents.end()); });
    }

    std::vector<uint32_t> AOIGrid::GetNeighborGridIndices(float x, float y) const
    {
        std::vector<uint32_t> indices;

        ForEachNeighborIndex(x, y, [&](uint32_t neighborIdx)
                             {
             if (indices.capacity() == 0) indices.reserve(9);
             indices.push_back(neighborIdx); });

        return indices;
    }

    std::vector<uint32_t> AOIGrid::GetNeighborGridIndices(uint32_t gridIndex) const
    {
        std::vector<uint32_t> indices;
        indices.reserve(9);

        ForEachNeighborIndex(gridIndex, [&](uint32_t neighborIdx)
                             { indices.push_back(neighborIdx); });

        return indices;
    }

    void AOIGrid::init_internal()
    {
        if (cellSize_ <= 0.0f)
            return;

        invCellSize_ = 1.0f / cellSize_;
        colCount_ = static_cast<uint32_t>(std::ceil(width_ / cellSize_));
        rowCount_ = static_cast<uint32_t>(std::ceil(height_ / cellSize_));

        size_t newSize = colCount_ * rowCount_;
        cells_.resize(newSize);
    }

} // namespace aegis::core
