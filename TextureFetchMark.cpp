#include "TextureFetchMark.h"

#include "KBest.h"

#include <eflib/include/math/vector.h>
#include <eflib/include/math/math.h>

#include <cstdint>
#include <utility>
#include <algorithm>
#include <iostream>
#include <thread>

using namespace std;

struct LinearAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y)
    {
        return std::make_pair(x, y);
    }

    static int Offset(int x, int y, int w, int h)
    {
		x = x < 0 ? 0 : (x > w-1 ? w-1 : x);
		y = y < 0 ? 0 : (y > h-1 ? h-1 : y);
        return y*h+x;
    }
};

struct MortonAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y)
	{
		int upperBound = std::max(x, y);
		upperBound--;
		upperBound |= upperBound >> 1;
		upperBound |= upperBound >> 2;
		upperBound |= upperBound >> 4;
		upperBound |= upperBound >> 8;
		upperBound |= upperBound >> 16;
		upperBound++;
		return std::make_pair(upperBound, upperBound);
	}

    static int Offset(int x, int y, int w, int h)
	{
		x = x < 0 ? 0 : (x > w-1 ? w-1 : x);
		y = y < 0 ? 0 : (y > h-1 ? h-1 : y);

		static const unsigned int B[] = {0x55555555, 0x33333333, 0x0F0F0F0F, 0x00FF00FF};
		static const unsigned int S[] = {1, 2, 4, 8};

		x = (x | (x << S[3])) & B[3];
		x = (x | (x << S[2])) & B[2];
		x = (x | (x << S[1])) & B[1];
		x = (x | (x << S[0])) & B[0];

		y = (y | (y << S[3])) & B[3];
		y = (y | (y << S[2])) & B[2];
		y = (y | (y << S[1])) & B[1];
		y = (y | (y << S[0])) & B[0];

		return x | (y << 1);
	}
};

template <int TileSize>
struct TiledAddresser
{
    static std::pair<int, int> AlignedSize(int x, int y)
    {
        return std::make_pair(
            (x + TileSize - 1) / TileSize * TileSize,
            (x + TileSize - 1) / TileSize * TileSize
			);
    }
    static int Offset(int x, int y, int w, int h)
	{
		x = x < 0 ? 0 : (x > w-1 ? w-1 : x);
		y = y < 0 ? 0 : (y > h-1 ? h-1 : y);

		int xOffset = (x % TileSize);
		int yOffset = (y % TileSize);

		int xTile = x / TileSize;
		int yTile = y / TileSize;

		int tileWidth = w / TileSize;
		int const tilePixels = TileSize * TileSize;

		return (yTile * tileWidth + xTile) * tilePixels + (xOffset + yOffset * TileSize);
	}
};

template <typename AddresserT, typename ComponentT>
class Surface
{
    int w, h, alignedW, alignedH;
    ComponentT* pixels;
public:
	Surface(int w, int h)
		:w(w), h(h), pixels(nullptr)
	{
		std::pair<int, int> alignedSize = AddresserT::AlignedSize(w, h);
		alignedW = alignedSize.first;
		alignedH = alignedSize.second;

		pixels = (ComponentT*)_aligned_malloc(sizeof(ComponentT) * alignedW * alignedH, 64);
		for(size_t i = 0; i < alignedH * alignedW * sizeof(ComponentT) / sizeof(float); ++i)
		{
			(reinterpret_cast<float*>(pixels))[i] = static_cast<float>(i);
		}
	}
	
	~Surface()
	{
		_aligned_free(pixels);
	}

	ComponentT GetPixel(float x, float y)
	{
		auto xPixels = ( x - floor(x) ) * alignedW - 0.5f;
		auto yPixels = ( y - floor(y) ) * alignedH - 0.5f;

		int xFloor = static_cast<int>(floor(xPixels));
		int yFloor = static_cast<int>(floor(yPixels));
		float xFrac = xPixels - xFloor;
		float yFrac = yPixels - yFloor;

		int address00 = AddresserT::Offset(xFloor,   yFloor,   alignedW, alignedH);
		_mm_prefetch( reinterpret_cast<char*>(address00+pixels), 0 );

		int address01 = AddresserT::Offset(xFloor+1, yFloor,   alignedW, alignedH);
		_mm_prefetch( reinterpret_cast<char*>(address01+pixels), 0 );

		int address10 = AddresserT::Offset(xFloor,   yFloor+1, alignedW, alignedH);
		_mm_prefetch( reinterpret_cast<char*>(address10+pixels), 0 );

		int address11 = AddresserT::Offset(xFloor+1, yFloor+1, alignedW, alignedH);
		_mm_prefetch( reinterpret_cast<char*>(address11+pixels), 0 );

		auto weight11 = xFrac * yFrac;
		auto weight00 = 1.0f - xFrac - yFrac + weight11;
		auto weight01 = xFrac - weight11;
		auto weight10 = yFrac - weight11;

		auto pixel
			= pixels[address00] * weight00
			+ pixels[address01] * weight01
			+ pixels[address10] * weight10
			+ pixels[address11] * weight11;

		// printf("%4d,%4d,%4d,%4d\n", address00, address01, address10, address11);
		return pixel;
	}
};

template <typename SurfaceT>
int TestFunction(SurfaceT& surf, int textureSize, int quadSize, int angularParts, float startAngular, int halfScaleParts)
{
	float pixelWidth = 1.0f / textureSize;
	float scaleBase = 1.5f;

	float antiOpt = 0.0f;
	float angularStep = 360.f / angularParts;
	for(int iAngular = 0; iAngular < angularParts; ++iAngular)
	{
		float angular = angularStep * iAngular + startAngular;
		float sinA = sin(angular / 360 * 2 * 3.1415926f);
		float cosA = cos(angular / 360 * 2 * 3.1415926f);

		for(int scalePow = -halfScaleParts; scalePow <= halfScaleParts; ++scalePow)
		{
			float dirLength = pixelWidth * pow(scaleBase, scalePow);
			float dirX = sinA * dirLength;
			float dirY = cosA * dirLength;

			auto doGetPixel = [&](int iStart, int iEnd, int jStart, int jEnd)
			{
				for(int i = iStart; i < iEnd; ++i)
				{
					for(int j = jStart; j < jEnd; ++j)
					{
						auto pixel = surf.GetPixel(i*dirX, j*dirY);
						antiOpt += *reinterpret_cast<float*>(&pixel);
					}
				}
			};

			std::thread tr[4];

			for(int iThreadY = 0; iThreadY < 2; ++iThreadY)
			{
				for(int iThreadX = 0; iThreadX < 2; ++iThreadX)
				{
					tr[iThreadY*2+iThreadX] = thread([&]()
					{
						doGetPixel(
							iThreadX*quadSize/2,
							iThreadY*quadSize/2,
							(iThreadX+1)*quadSize/2,
							(iThreadY+1)*quadSize/2
							);
					});
				}
			}

			for(int iThread = 0; iThread < 4; ++iThread)
			{
				tr[iThread].join();
			}
		}
	}

	cout << antiOpt << endl;
	return antiOpt > 0.0f ? 0 : 1;
}

int TextureFetchMark()
{
	{
		int textureSize = 2048;
		int quadSize = 512;
		int halfScalePowParts = 2;
		int angularParts = 1;
		
		int totalPixels = quadSize * quadSize * ( halfScalePowParts * 2 + 1) * angularParts;
		
		cout << "Initializing ... " << endl;
		Surface<MortonAddresser, float> surf(textureSize, textureSize);

		cout << "Running ... " << endl;
		auto Surf1x1_Linear_Float = [=, &surf]() -> void
		{
			TestFunction(surf, textureSize, quadSize, angularParts, -60.0f, halfScalePowParts);
		};

		k_best measure(16, 0.05f, 200);
		auto testResult = measure.test(Surf1x1_Linear_Float);
		
		cout << "Surf1x1_Linear_Float: " << testResult.second << "us" << endl;
		printf("Pixel Sampling Rate: %5.3fMTexel/s\n", totalPixels / float(testResult.second));
	}
	return 0;
}
