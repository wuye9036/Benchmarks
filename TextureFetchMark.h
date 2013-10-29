#pragma once

#include <cstdint>
#include <utility>

struct LinearAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y)
    {
        return std::make_pair(x, y);
    }

    static int Offset(int x, int y, int w, int h)
    {
        return y*h+x;
    }
};

struct MortonAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y);
    static int Offset(int x, int y, int w, int h);
};

template <int TileSize>
struct TiledAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y)
    {
        return std::make_pair(
            (x + TileSize - 1) / TileSize * TileSize,
            (x + TileSize - 1) / TileSize * TileSize,
    }
    static int Offset(int x, int y, int w, int h);
};

template <typename AddresserT, typename ComponentT>
class Surface
{
    int w, h, alignedW, alignedH;
    ComponentT* pixels;
};