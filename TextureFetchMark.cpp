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

    static __m128i OffsetV(int x, int y, int w, int h)
    {
        int x1 = x+1;
        int y1 = y+1;
		x = x < 0 ? 0 : (x > w-1 ? w-1 : x);
		y = y < 0 ? 0 : (y > h-1 ? h-1 : y);
        x1 = x1 < 0 ? 0 : (x1 > w-1 ? w-1 : x1);
		y1 = y1 < 0 ? 0 : (y1 > h-1 ? h-1 : y1);

        __m128i X = _mm_set_epi32(x, x1, x,  x1);
        __m128i Y = _mm_set_epi32(y,  y, y1, y1);
        __m128i W = _mm_set1_epi32(w);

        __m128i YW = _mm_mul_epi32(W, Y);

        return _mm_add_epi32(X, YW);
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

inline __m128 _mm_floor_ps2(__m128 const& x)
{
    __m128i v0 = _mm_setzero_si128();
    __m128i v1 = _mm_cmpeq_epi32(v0,v0);
    __m128i ji = _mm_srli_epi32( v1, 25);
    __m128 j = *(__m128*)&_mm_slli_epi32( ji, 23); //create vector 1.0f
    __m128i i = _mm_cvttps_epi32(x);
    __m128 fi = _mm_cvtepi32_ps(i);
    __m128 igx = _mm_cmpgt_ps(fi, x);
    j = _mm_and_ps(igx, j);
    return _mm_sub_ps(fi, j);
}

template <typename AddresserT, typename ComponentT>
class Surface
{
    int w, h, alignedW, alignedH;
    ComponentT* pixels;
public:
    typedef ComponentT ComponentType;

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

    template <typename PixelComponentT>
    inline static PixelComponentT ComputePixel(
        PixelComponentT* ret,
        PixelComponentT const* pixels,
        __m128i const& addr,
        __m128 const& weights)
    {
        *ret =
			  pixels[addr.m128i_i32[0]] * weights.m128_f32[0]
			+ pixels[addr.m128i_i32[1]] * weights.m128_f32[1]
			+ pixels[addr.m128i_i32[2]] * weights.m128_f32[2]
			+ pixels[addr.m128i_i32[3]] * weights.m128_f32[3];
    }

    inline static void ComputePixel(
        eflib::vec4* ret,
        eflib::vec4 const* pixels,
        __m128i const& addr,
        __m128 const& weights)
    {
        auto pixels_m128 = reinterpret_cast<__m128 const*>(pixels);

		__m128 sum01 = _mm_add_ps(
            _mm_mul_ps( pixels_m128[addr.m128i_i32[0]], _mm_set_ps1(weights.m128_f32[0]) ),
            _mm_mul_ps( pixels_m128[addr.m128i_i32[1]], _mm_set_ps1(weights.m128_f32[1]) )
            );

        __m128 sum23 = _mm_add_ps(
            _mm_mul_ps( pixels_m128[addr.m128i_i32[2]], _mm_set_ps1(weights.m128_f32[2]) ),
            _mm_mul_ps( pixels_m128[addr.m128i_i32[3]], _mm_set_ps1(weights.m128_f32[3]) )
            );

        *(reinterpret_cast<__m128*>(ret)) = _mm_add_ps(sum01, sum23);
    }

    inline static float ComputePixel(
        float const* pixels,
        __m128i const& addr,
        __m128 const& weights)
    {
        __m128 samples = _mm_set_ps(
            pixels[addr.m128i_i32[0]],
            pixels[addr.m128i_i32[1]],
            pixels[addr.m128i_i32[2]],
            pixels[addr.m128i_i32[3]]
        );
        __m128 mulSamples = _mm_mul_ps(samples, weights);
        __m128 ret = _mm_hadd_ps(mulSamples, mulSamples);
        return ret.m128_f32[0] + ret.m128_f32[1];
    }

    void GetPixel(ComponentT* p0, ComponentT* p1, __m128 const& coord)
	{
        __m128  coordFloor   = _mm_floor_ps(coord);
        __m128  wrappedCoord = _mm_sub_ps(coord, coordFloor);
        __m128i alignedSizeI = _mm_set_epi32(alignedW, alignedH, alignedW, alignedH);
        __m128  alignedSize  = _mm_cvtepi32_ps(alignedSizeI);
        __m128  pixelCoord   = _mm_mul_ps(wrappedCoord, alignedSize);
        pixelCoord           = _mm_sub_ps( pixelCoord, _mm_set_ps1(0.5f) );
        __m128  pixelCoordFlr= _mm_floor_ps(pixelCoord);
        __m128i pixelCoordI  = _mm_cvtps_epi32(pixelCoordFlr);
        __m128  pixelCoordFra= _mm_sub_ps(pixelCoord, pixelCoordFlr);

        {
            int xFloor = pixelCoordI.m128i_i32[0];
            int yFloor = pixelCoordI.m128i_i32[1];
            float xFrac = pixelCoordFra.m128_f32[0];
            float yFrac = pixelCoordFra.m128_f32[1];

            __m128i addrs = AddresserT::OffsetV(xFloor, yFloor, alignedW, alignedH);

            __m128 weights;
            weights.m128_f32[3] = xFrac * yFrac;
		    weights.m128_f32[0] = 1.0f - xFrac - yFrac + weights.m128_f32[3];
		    weights.m128_f32[1] = xFrac - weights.m128_f32[3];
		    weights.m128_f32[2] = yFrac - weights.m128_f32[3];

            ComputePixel(p0, pixels, addrs, weights);
        }

        {
            int xFloor = pixelCoordI.m128i_i32[2];
            int yFloor = pixelCoordI.m128i_i32[3];
            float xFrac = pixelCoordFra.m128_f32[2];
            float yFrac = pixelCoordFra.m128_f32[3];

            __m128i addrs = AddresserT::OffsetV(xFloor, yFloor, alignedW, alignedH);

            __m128 weights;
            weights.m128_f32[3] = xFrac * yFrac;
		    weights.m128_f32[0] = 1.0f - xFrac - yFrac + weights.m128_f32[3];
		    weights.m128_f32[1] = xFrac - weights.m128_f32[3];
		    weights.m128_f32[2] = yFrac - weights.m128_f32[3];

            ComputePixel(p1, pixels, addrs, weights);
        }
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

		for(int scalePow = 1; scalePow < halfScaleParts+2; ++scalePow)
		{
			float dirLength = pixelWidth * pow(scaleBase, scalePow);
			float dirX = sinA * dirLength;
			float dirY = cosA * dirLength;

			auto doGetPixel = [&](int iStart, int iEnd, int jStart, int jEnd)
			{
				for(int i = iStart; i < iEnd; ++i)
				{
					for(int j = jStart; j < jEnd; j+=2)
					{
                        __m128 coord;
                        coord.m128_f32[0] = i*dirX;
                        coord.m128_f32[1] = j*dirY;
                        coord.m128_f32[2] = i*dirX;
                        coord.m128_f32[3] = coord.m128_f32[1] + dirY;
                        __declspec(align(16)) typename SurfaceT::ComponentType p0, p1;
                        surf.GetPixel(&p0, &p1, coord);
						antiOpt += *reinterpret_cast<float*>(&p0);
                        antiOpt += *reinterpret_cast<float*>(&p1);
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
		int textureSize = 8192;
		int quadSize = 2048;
		int halfScalePowParts = 4;
		int angularParts = 1;
		
		int totalPixels = quadSize * quadSize * ( halfScalePowParts + 2) * angularParts;
		
		cout << "Initializing ... " << endl;
        Surface<LinearAddresser, eflib::vec4> surf(textureSize, textureSize);

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
