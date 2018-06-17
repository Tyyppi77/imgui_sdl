#include "imgui_sdl.h"

#include "SDL.h"

#include "SDL_image.h"
#include "SDL2_gfxPrimitives.h"
#include "imgui.h"

#include <map>
#include <vector>
#include <iostream>
#include <algorithm>
#include <functional>

namespace ImGuiSDL
{
	// TODO: Separate declarations for cleaner documentation.
	// TODO: Improved README with usage, include stuff etc.
	// TODO: Reference at least IMGUI

	struct Line
	{
		const double XCoefficient;
		const double YCoefficient;
		const double Constant;
		const bool Tie;

		Line(double x0, double y0, double x1, double y1)
			: XCoefficient(y0 - y1),
			YCoefficient(x1 - x0),
			Constant(-0.5 * (XCoefficient * (x0 + x1) + YCoefficient * (y0 + y1))),
			Tie(XCoefficient != 0 ? XCoefficient > 0 : YCoefficient > 0)
		{
		}

		double Evaluate(double x, double y) const
		{
			return XCoefficient * x + YCoefficient * y + Constant;
		}

		bool IsInside(double x, double y) const
		{
			return IsInside(Evaluate(x, y));
		}

		bool IsInside(double v) const
		{
			return (v > 0 || (v == 0 && Tie));
		}
	};

	struct InterpolatedFactorEquation
	{
		const double Value0;
		const double Value1;
		const double Value2;

		const ImVec2& V0;
		const ImVec2& V1;
		const ImVec2& V2;

		const double Divisor;

		InterpolatedFactorEquation(double value0, double value1, double value2, const ImVec2& v0, const ImVec2& v1, const ImVec2& v2)
			: Value0(value0), Value1(value1), Value2(value2), V0(v0), V1(v1), V2(v2),
			Divisor((V1.y - V2.y) * (V0.x - V2.x) + (V2.x - V1.x) * (V0.y - V2.y))
		{
		}

		double Evaluate(double x, double y) const
		{
			const double w1 = ((V1.y - V2.y) * (x - V2.x) + (V2.x - V1.x) * (y - V2.y)) / Divisor;
			const double w2 = ((V2.y - V0.y) * (x - V2.x) + (V0.x - V2.x) * (y - V2.y)) / Divisor;
			const double w3 = 1.0 - w1 - w2;

			return w1 * Value0 + w2 * Value1 + w3 * Value2;
		}
	};

	struct Rect
	{
		double MinX, MinY, MaxX, MaxY;
		double MinU, MinV, MaxU, MaxV;

		bool IsOnExtreme(const ImVec2& point) const
		{
			return (point.x == MinX || point.x == MaxX) && (point.y == MinY || point.y == MaxY);
		}

		bool UsesOnlyColor(const Texture* texture) const
		{
			// TODO: Consider caching the fixed point representation of these.
			const double whiteU = (0.5 / texture->Surface->w);
			const double whiteV = (0.5 / texture->Surface->h);

			return MinU == MaxU && MinU == whiteU && MinV == MaxV && MaxV == whiteV;
		}
	};

	Target::Target(int width, int height, SDL_Renderer* renderer) : Renderer(renderer), Width(width), Height(height)
	{
	}

	void Target::Resize(int width, int height)
	{
		Width = width;
		Height = height;

		for (auto& pair : UniformColorTriangleCache)
		{
			SDL_DestroyTexture(pair.second.Texture);
		}
		UniformColorTriangleCache.clear();
	}

	void Target::SetClipRect(const ClipRect& rect)
	{
		Clip = rect;
		const SDL_Rect clip = {
			rect.X,
			rect.Y,
			rect.Width,
			rect.Height
		};
		SDL_RenderSetClipRect(Renderer, &clip);
	}

	void Target::EnableClip()
	{
		SetClipRect(Clip);
	}
	void Target::DisableClip()
	{
		SDL_RenderSetClipRect(Renderer, nullptr);
	}

	void Target::SetAt(int x, int y, const Color& color)
	{
		SDL_SetRenderDrawColor(Renderer, color.R * 255, color.G * 255, color.B * 255, color.A * 255);
		SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_BLEND);
		SDL_RenderDrawPoint(Renderer, x, y);
	}

	SDL_Texture* Target::MakeTexture(int width, int height)
	{
		SDL_Texture* texture = SDL_CreateTexture(Renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		return texture;
	}

	void Target::UseAsRenderTarget(SDL_Texture* texture)
	{
		SDL_SetRenderTarget(Renderer, texture);
		if (texture)
		{
			SDL_SetRenderDrawColor(Renderer, 0, 0, 0, 0);
			SDL_RenderClear(Renderer);
		}
	}

	Rect CalculateBoundingBox(const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2)
	{
		// TODO: This doesn't account for flipped UVs.
		return Rect{
			Min3(v0.pos.x, v1.pos.x, v2.pos.x),
			Min3(v0.pos.y, v1.pos.y, v2.pos.y),
			Max3(v0.pos.x, v1.pos.x, v2.pos.x),
			Max3(v0.pos.y, v1.pos.y, v2.pos.y),
			Min3(v0.uv.x, v1.uv.x, v2.uv.x),
			Min3(v0.uv.y, v1.uv.y, v2.uv.y),
			Max3(v0.uv.x, v1.uv.x, v2.uv.x),
			Max3(v0.uv.y, v1.uv.y, v2.uv.y)
		};
	}

	struct FixedPointTriangleRenderInfo
	{
		int X1, X2, X3, Y1, Y2, Y3;
		int MinX, MaxX, MinY, MaxY;
	};

	FixedPointTriangleRenderInfo CalculateFixedPointTriangleInfo(const ImVec2& v1, const ImVec2& v2, const ImVec2& v3)
	{
		static constexpr float scale = 16.0f;

		const int x1 = static_cast<int>(round(v1.x * scale));
		const int x2 = static_cast<int>(round(v2.x * scale));
		const int x3 = static_cast<int>(round(v3.x * scale));

		const int y1 = static_cast<int>(round(v1.y * scale));
		const int y2 = static_cast<int>(round(v2.y * scale));
		const int y3 = static_cast<int>(round(v3.y * scale));

		int minx = (Min3(x1, x2, x3) + 0xF) >> 4;
		int maxx = (Max3(x1, x2, x3) + 0xF) >> 4;
		int miny = (Min3(y1, y2, y3) + 0xF) >> 4;
		int maxy = (Max3(y1, y2, y3) + 0xF) >> 4;

		return FixedPointTriangleRenderInfo{ x1, x2, x3, y1, y2, y3, minx, maxx, miny, maxy };
	}

	Target::TriangleCacheItem DrawTriangleWithColorFunction(Target& target, const ImDrawVert& v1, const ImDrawVert& v2, const ImDrawVert& v3, const FixedPointTriangleRenderInfo& renderInfo, const std::function<Color(double x, double y)>& colorFunction)
	{
		// RIPPED OFF FROM https://web.archive.org/web/20171128164608/http://forum.devmaster.net/t/advanced-rasterization/6145.
		// This is a fixed point imlementation that rounds to top-left.

		const int deltaX12 = renderInfo.X1 - renderInfo.X2;
		const int deltaX23 = renderInfo.X2 - renderInfo.X3;
		const int deltaX31 = renderInfo.X3 - renderInfo.X1;

		const int deltaY12 = renderInfo.Y1 - renderInfo.Y2;
		const int deltaY23 = renderInfo.Y2 - renderInfo.Y3;
		const int deltaY31 = renderInfo.Y3 - renderInfo.Y1;

		const int fixedDeltaX12 = deltaX12 << 4;
		const int fixedDeltaX23 = deltaX23 << 4;
		const int fixedDeltaX31 = deltaX31 << 4;

		const int fixedDeltaY12 = deltaY12 << 4;
		const int fixedDeltaY23 = deltaY23 << 4;
		const int fixedDeltaY31 = deltaY31 << 4;

		if (renderInfo.MaxX - renderInfo.MinX == 0 || renderInfo.MaxY - renderInfo.MinY == 0) return Target::TriangleCacheItem{ nullptr };

		int c1 = deltaY12 * renderInfo.X1 - deltaX12 * renderInfo.Y1;
		int c2 = deltaY23 * renderInfo.X2 - deltaX23 * renderInfo.Y2;
		int c3 = deltaY31 * renderInfo.X3 - deltaX31 * renderInfo.Y3;

		if (deltaY12 < 0 || (deltaY12 == 0 && deltaX12 > 0)) c1++;
		if (deltaY23 < 0 || (deltaY23 == 0 && deltaX23 > 0)) c2++;
		if (deltaY31 < 0 || (deltaY31 == 0 && deltaX31 > 0)) c3++;

		int cY1 = c1 + deltaX12 * (renderInfo.MinY << 4) - deltaY12 * (renderInfo.MinX << 4);
		int cY2 = c2 + deltaX23 * (renderInfo.MinY << 4) - deltaY23 * (renderInfo.MinX << 4);
		int cY3 = c3 + deltaX31 * (renderInfo.MinY << 4) - deltaY31 * (renderInfo.MinX << 4);

		SDL_Texture* cache = target.MakeTexture(renderInfo.MaxX - renderInfo.MinX, renderInfo.MaxY - renderInfo.MinY);
		target.DisableClip();
		target.UseAsRenderTarget(cache);

		for (int y = renderInfo.MinY; y < renderInfo.MaxY; y++)
		{
			int cX1 = cY1;
			int cX2 = cY2;
			int cX3 = cY3;

			for (int x = renderInfo.MinX; x < renderInfo.MaxX; x++)
			{
				if (cX1 > 0 && cX2 > 0 && cX3 > 0)
				{
					target.SetAt(x - renderInfo.MinX, y - renderInfo.MinY, colorFunction(x + 0.5, y + 0.5));
				}

				cX1 -= fixedDeltaY12;
				cX2 -= fixedDeltaY23;
				cX3 -= fixedDeltaY31;
			}

			cY1 += fixedDeltaX12;
			cY2 += fixedDeltaX23;
			cY3 += fixedDeltaX31;
		}

		target.UseAsRenderTarget(nullptr);
		target.EnableClip();

		return Target::TriangleCacheItem{ cache, renderInfo.MinX, renderInfo.MinY, renderInfo.MaxX - renderInfo.MinX, renderInfo.MaxY - renderInfo.MinY };
	}

	void DrawTriangle(Target& target, const ImDrawVert& v1, const ImDrawVert& v2, const ImDrawVert& v3, const Texture* texture, const Rect& bounding)
	{
		const Color color0 = Color(v1.col);
		const Color color1 = Color(v2.col);
		const Color color2 = Color(v3.col);

		const InterpolatedFactorEquation textureU(v1.uv.x, v2.uv.x, v3.uv.x, v1.pos, v2.pos, v3.pos);
		const InterpolatedFactorEquation textureV(v1.uv.y, v2.uv.y, v3.uv.y, v1.pos, v2.pos, v3.pos);

		const InterpolatedFactorEquation shadeR(color0.R, color1.R, color2.R, v1.pos, v2.pos, v3.pos);
		const InterpolatedFactorEquation shadeG(color0.G, color1.G, color2.G, v1.pos, v2.pos, v3.pos);
		const InterpolatedFactorEquation shadeB(color0.B, color1.B, color2.B, v1.pos, v2.pos, v3.pos);
		const InterpolatedFactorEquation shadeA(color0.A, color1.A, color2.A, v1.pos, v2.pos, v3.pos);

		const auto& renderInfo = CalculateFixedPointTriangleInfo(v3.pos, v2.pos, v1.pos);

		// The naming inconsistency in the parameters is intentional. The fixed point algorithm wants the vertices in a counter clockwise order.
		const auto& cached = DrawTriangleWithColorFunction(target, v3, v2, v1, renderInfo, [&](double x, double y) {
			const double u = textureU.Evaluate(x, y);
			const double v = textureV.Evaluate(x, y);
			const Color sampled = texture->Sample(u, v);
			const Color shade = Color(shadeR.Evaluate(x, y), shadeG.Evaluate(x, y), shadeB.Evaluate(x, y), shadeA.Evaluate(x, y));

			return sampled * shade;
		});

		if (!cached.Texture) return;

		const SDL_Rect destination = { cached.X, cached.Y, cached.Width, cached.Height };
		SDL_RenderCopy(target.Renderer, cached.Texture, nullptr, &destination);
	}

	void DrawUniformColorTriangle(Target& target, const ImDrawVert& v1, const ImDrawVert& v2, const ImDrawVert& v3)
	{
		const Color color(v1.col);

		const auto& renderInfo = CalculateFixedPointTriangleInfo(v3.pos, v2.pos, v1.pos);

		const auto key = std::make_tuple(v1.col, 
			static_cast<int>(round(v1.pos.x)) - renderInfo.MinX, static_cast<int>(round(v1.pos.y)) - renderInfo.MinY,
			static_cast<int>(round(v2.pos.x)) - renderInfo.MinX, static_cast<int>(round(v2.pos.y)) - renderInfo.MinY,
			static_cast<int>(round(v3.pos.x)) - renderInfo.MinX, static_cast<int>(round(v3.pos.y)) - renderInfo.MinY);
		if (target.UniformColorTriangleCache.count(key) > 0)
		{
			const auto& cacheItem = target.UniformColorTriangleCache.at(key);

			const SDL_Rect destination = { renderInfo.MinX, renderInfo.MinY, cacheItem.Width, cacheItem.Height };
			SDL_RenderCopy(target.Renderer, cacheItem.Texture, nullptr, &destination);

			return;
		}

		// The naming inconsistency in the parameters is intentional. The fixed point algorithm wants the vertices in a counter clockwise order.
		const auto& cached = DrawTriangleWithColorFunction(target, v3, v2, v1, renderInfo, [&color](double, double) { return color; });

		if (!cached.Texture) return;

		const SDL_Rect destination = { cached.X, cached.Y, cached.Width, cached.Height };
		SDL_RenderCopy(target.Renderer, cached.Texture, nullptr, &destination);

		target.UniformColorTriangleCache[key] = cached;
	}

	void DrawRectangle(Target& target, const Rect& bounding, const Texture* texture, const Color& color)
	{
		// We are safe to assume uniform color here, because the caller checks it and and uses the triangle renderer to render those.

		const SDL_Rect destination = {
			bounding.MinX,
			bounding.MinY,
			(bounding.MaxX - bounding.MinX),
			(bounding.MaxY - bounding.MinY)
		};

		// If the area isn't textured, we can just draw a rectangle with the correct color.
		if (bounding.UsesOnlyColor(texture))
		{
			SDL_SetRenderDrawColor(target.Renderer, color.R * 255, color.G * 255, color.B * 255, color.A * 255);
			SDL_RenderFillRect(target.Renderer, &destination);
		}
		else
		{
			// We can now just calculate the correct source rectangle and draw it.

			const SDL_Rect source = {
				bounding.MinU * texture->Surface->w,
				bounding.MinV * texture->Surface->h,
				(bounding.MaxU - bounding.MinU) * texture->Surface->w,
				(bounding.MaxV - bounding.MinV) * texture->Surface->h
			};

			SDL_SetTextureColorMod(texture->Source, color.R * 255, color.G * 255, color.B * 255);
			SDL_RenderCopy(target.Renderer, texture->Source, &source, &destination);
		}
	}

	void DoImGuiRender(Target& target, ImDrawData* drawData)
	{
		for (int n = 0; n < drawData->CmdListsCount; n++)
		{
			auto commandList = drawData->CmdLists[n];
			auto vertexBuffer = commandList->VtxBuffer;
			auto indexBuffer = commandList->IdxBuffer.Data;

			for (int cmd_i = 0; cmd_i < commandList->CmdBuffer.Size; cmd_i++)
			{
				const ImDrawCmd* drawCommand = &commandList->CmdBuffer[cmd_i];

				const ClipRect clipRect = {
					drawCommand->ClipRect.x,
					drawCommand->ClipRect.y,
					drawCommand->ClipRect.z - drawCommand->ClipRect.x,
					drawCommand->ClipRect.w - drawCommand->ClipRect.y
				};
				target.SetClipRect(clipRect);

				if (drawCommand->UserCallback)
				{
					drawCommand->UserCallback(commandList, drawCommand);
				}
				else
				{
					const Texture* texture = static_cast<const Texture*>(drawCommand->TextureId);

					// Loops over triangles.
					for (int i = 0; i + 3 <= drawCommand->ElemCount; i += 3)
					{
						const ImDrawVert& v0 = vertexBuffer[indexBuffer[i + 0]];
						const ImDrawVert& v1 = vertexBuffer[indexBuffer[i + 1]];
						const ImDrawVert& v2 = vertexBuffer[indexBuffer[i + 2]];

						const Rect& bounding = CalculateBoundingBox(v0, v1, v2);

						const bool isTriangleUniformColor = v0.col == v1.col && v1.col == v2.col;
						const bool doesTriangleUseOnlyColor = bounding.UsesOnlyColor(texture);

						// Actually, since we render a whole bunch of rectangles, we try to first detect those, and render them more efficiently.
						// How are rectangles detected? It's actually pretty simple: If all 6 vertices lie on the extremes of the bounding box, 
						// it's a rectangle.
						if (i + 6 <= drawCommand->ElemCount)
						{
							const ImDrawVert& v3 = vertexBuffer[indexBuffer[i + 3]];
							const ImDrawVert& v4 = vertexBuffer[indexBuffer[i + 4]];
							const ImDrawVert& v5 = vertexBuffer[indexBuffer[i + 5]];

							const bool isUniformColor = isTriangleUniformColor && v2.col == v3.col && v3.col == v4.col && v4.col == v5.col;

							if (isUniformColor
							&& bounding.IsOnExtreme(v0.pos)
							&& bounding.IsOnExtreme(v1.pos)
							&& bounding.IsOnExtreme(v2.pos)
							&& bounding.IsOnExtreme(v3.pos)
							&& bounding.IsOnExtreme(v4.pos)
							&& bounding.IsOnExtreme(v5.pos))
							{
								DrawRectangle(target, bounding, texture, Color(v0.col));

								i += 3;  // Additional increment.
								continue;
							}
						}

						if (isTriangleUniformColor && doesTriangleUseOnlyColor)
						{
							DrawUniformColorTriangle(target, v0, v1, v2);
						}
						else
						{
							DrawTriangle(target, v0, v1, v2, texture, bounding);
						}
					}
				}

				indexBuffer += drawCommand->ElemCount;
			}
		}

		target.DisableClip();
	}
};