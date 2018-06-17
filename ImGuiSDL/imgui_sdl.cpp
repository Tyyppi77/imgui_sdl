#include "imgui_sdl.h"

#include "SDL.h"

#include "SDL_image.h"
#include "SDL2_gfxPrimitives.h"
#include "imgui.h"

#include <map>
#include <vector>
#include <iostream>
#include <algorithm>

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

		for (auto& pair : CacheTextures)
		{
			SDL_DestroyTexture(pair.second);
		}
		CacheTextures.clear();
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

	static constexpr int CacheTexturePadding = 2;

	void DrawTriangle(Target& target, const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2, const Texture* texture, const Rect& bounding)
	{
		// TODO: Figure out how to actually position this stuff, looks offset.
		const SDL_Rect destination = {
			static_cast<int>(bounding.MinX) - CacheTexturePadding,
			static_cast<int>(bounding.MinY) - CacheTexturePadding,
			static_cast<int>((bounding.MaxX - bounding.MinX)) + 2 * CacheTexturePadding,
			static_cast<int>((bounding.MaxY - bounding.MinY)) + 2 * CacheTexturePadding
		};

		const Line line0(v0.pos.x, v0.pos.y, v1.pos.x, v1.pos.y);
		const Line line1(v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y);
		const Line line2(v2.pos.x, v2.pos.y, v0.pos.x, v0.pos.y);

		const Color color0 = Color(v0.col);
		const Color color1 = Color(v1.col);
		const Color color2 = Color(v2.col);

		const InterpolatedFactorEquation textureU(v0.uv.x, v1.uv.x, v2.uv.x, v0.pos, v1.pos, v2.pos);
		const InterpolatedFactorEquation textureV(v0.uv.y, v1.uv.y, v2.uv.y, v0.pos, v1.pos, v2.pos);

		const InterpolatedFactorEquation shadeR(color0.R, color1.R, color2.R, v0.pos, v1.pos, v2.pos);
		const InterpolatedFactorEquation shadeG(color0.G, color1.G, color2.G, v0.pos, v1.pos, v2.pos);
		const InterpolatedFactorEquation shadeB(color0.B, color1.B, color2.B, v0.pos, v1.pos, v2.pos);
		const InterpolatedFactorEquation shadeA(color0.A, color1.A, color2.A, v0.pos, v1.pos, v2.pos);

		for (int drawY = 0; drawY <= destination.h; drawY += 1)
		{
			for (int drawX = 0; drawX <= destination.w; drawX += 1)
			{
				const double sampleX = drawX + 0.5f + bounding.MinX;
				const double sampleY = drawY + 0.5f + bounding.MinY;

				const double checkX = sampleX;
				const double checkY = sampleY;

				if (line0.IsInside(checkX, checkY) && line1.IsInside(checkX, checkY) && line2.IsInside(checkX, checkY))
				{
					const double u = textureU.Evaluate(sampleX, sampleY);
					const double v = textureV.Evaluate(sampleX, sampleY);

					// Sample the color from the surface.
					const Color& sampled = texture->Sample(u, v);

					const Color& shade = Color(shadeR.Evaluate(sampleX, sampleY), shadeG.Evaluate(sampleX, sampleY), shadeB.Evaluate(sampleX, sampleY), shadeA.Evaluate(sampleX, sampleY));

					target.SetAt(drawX + CacheTexturePadding, drawY + CacheTexturePadding, sampled * shade);
				}
			}
		}
	}

	void DrawTriangleNoCache(Target& target, const ImDrawVert& v3, const ImDrawVert& v2, const ImDrawVert& v1, const Texture* texture)
	{
		// RIPPED OFF FROM https://web.archive.org/web/20171128164608/http://forum.devmaster.net/t/advanced-rasterization/6145.

		static constexpr float scale = 16.0f;
		const int y1 = static_cast<int>(round(v1.pos.y * scale));
		const int y2 = static_cast<int>(round(v2.pos.y * scale));
		const int y3 = static_cast<int>(round(v3.pos.y * scale));

		const int x1 = static_cast<int>(round(v1.pos.x * scale));
		const int x2 = static_cast<int>(round(v2.pos.x * scale));
		const int x3 = static_cast<int>(round(v3.pos.x * scale));

		const int deltaX12 = x1 - x2;
		const int deltaX23 = x2 - x3;
		const int deltaX31 = x3 - x1;

		const int deltaY12 = y1 - y2;
		const int deltaY23 = y2 - y3;
		const int deltaY31 = y3 - y1;

		const int fixedDeltaX12 = deltaX12 << 4;
		const int fixedDeltaX23 = deltaX23 << 4;
		const int fixedDeltaX31 = deltaX31 << 4;

		const int fixedDeltaY12 = deltaY12 << 4;
		const int fixedDeltaY23 = deltaY23 << 4;
		const int fixedDeltaY31 = deltaY31 << 4;

		int minx = (Min3(x1, x2, x3) + 0xF) >> 4;
		int maxx = (Max3(x1, x2, x3) + 0xF) >> 4;
		int miny = (Min3(y1, y2, y3) + 0xF) >> 4;
		int maxy = (Max3(y1, y2, y3) + 0xF) >> 4;

		int c1 = deltaY12 * x1 - deltaX12 * y1;
		int c2 = deltaY23 * x2 - deltaX23 * y2;
		int c3 = deltaY31 * x3 - deltaX31 * y3;

		if (deltaY12 < 0 || (deltaY12 == 0 && deltaX12 > 0)) c1++;
		if (deltaY23 < 0 || (deltaY23 == 0 && deltaX23 > 0)) c2++;
		if (deltaY31 < 0 || (deltaY31 == 0 && deltaX31 > 0)) c3++;

		int cY1 = c1 + deltaX12 * (miny << 4) - deltaY12 * (minx << 4);
		int cY2 = c2 + deltaX23 * (miny << 4) - deltaY23 * (minx << 4);
		int cY3 = c3 + deltaX31 * (miny << 4) - deltaY31 * (minx << 4);

		for (int y = miny; y < maxy; y++)
		{
			int cX1 = cY1;
			int cX2 = cY2;
			int cX3 = cY3;

			for (int x = minx; x < maxx; x++)
			{
				if (cX1 > 0 && cX2 > 0 && cX3 > 0)
				{
					target.SetAt(x, y, Color(v1.col));
				}

				cX1 -= fixedDeltaY12;
				cX2 -= fixedDeltaY23;
				cX3 -= fixedDeltaY31;
			}

			cY1 += fixedDeltaX12;
			cY2 += fixedDeltaX23;
			cY3 += fixedDeltaX31;
		}
	}

	void DrawBottomFlatTriangle(Target& target, double x0, double y0, double x1, double y1, double x2, double y2, const Color& color)
	{
		const double invSlope0 = (x1 - x0) / (y1 - y0);
		const double invSlope1 = (x2 - x0) / (y2 - y0);

		double currentX0(x0);
		double currentX1(x0);

		for (int scanLineY = y0; scanLineY <= y1; scanLineY++)
		{
			// Draws a horizontal line slice.
			const double xStart = std::min(currentX0, currentX1);
			const double xEnd = std::max(currentX0, currentX1);
			for (int x = static_cast<int>(xStart); x <= static_cast<int>(xEnd); x++)
			{
				target.SetAt(x + CacheTexturePadding, scanLineY + CacheTexturePadding, color);
			}

			currentX0 = currentX0 + invSlope1;
			currentX1 = currentX1 + invSlope0;
		}
	}

	void DrawTopFlatTriangle(Target& target, double x0, double y0, double x1, double y1, double x2, double y2, const Color& color)
	{
		const double invSlope0 = (x2 - x0) / (y2 - y0);
		const double invSlope1 = (x2 - x1) / (y2 - y1);

		double currentX0(x2);
		double currentX1(x2);

		for (int scanLineY = y2; scanLineY > y0; scanLineY--)
		{
			// Draws a horizontal line slice.
			const double xStart = std::min(currentX0, currentX1);
			const double xEnd = std::max(currentX0, currentX1);
			for (int x = static_cast<int>(xStart); x <= static_cast<int>(xEnd); x++)
			{
				target.SetAt(x + CacheTexturePadding, scanLineY + CacheTexturePadding, color);
			}

			currentX0 = currentX0 - invSlope0;
			currentX1 = currentX1 - invSlope1;
		}
	}

	void DrawLine(Target& target, ImVec2 start, ImVec2 end, const Color& color)
	{
		const bool isSteep = (std::abs((end.y - start.y)) > std::abs((end.x - start.x)));
		if (isSteep)
		{
			std::swap(start.x, start.y);
			std::swap(end.x, end.y);
		}

		if (start.x > end.x)
		{
			std::swap(start, end);
		}

		const double dx = end.x - start.x;
		const double dy = std::abs((end.y - start.y));

		double error = dx / 2.0f;
		const int ystep = (start.y < end.y) ? 1 : -1;
		int y = start.y;

		for (int x = start.x; x <= end.x; x++)
		{
			if (isSteep) target.SetAt(y + CacheTexturePadding, x + CacheTexturePadding, color);
			else target.SetAt(x + CacheTexturePadding, y + CacheTexturePadding, color);

			error -= dy;
			if (error < 0.0f)
			{
				y += ystep;
				error += dx;
			}
		}
	}

	void DrawUniformColorTriangle(Target& target, const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2, const Rect& bounding)
	{
		// TODO: Code duplication from the generic triangle function.

		// TODO: Figure out how to actually position this stuff, looks offset.
		const SDL_Rect destination = {
			static_cast<int>(bounding.MinX) - CacheTexturePadding,
			static_cast<int>(bounding.MinY) - CacheTexturePadding,
			static_cast<int>((bounding.MaxX - bounding.MinX)) + 2 * CacheTexturePadding,
			static_cast<int>((bounding.MaxY - bounding.MinY)) + 2 * CacheTexturePadding
		};

		const ImVec2 offset0 = ImVec2(v0.pos.x - bounding.MinX, v0.pos.y - bounding.MinY);
		const ImVec2 offset1 = ImVec2(v1.pos.x - bounding.MinX, v1.pos.y - bounding.MinY);
		const ImVec2 offset2 = ImVec2(v2.pos.x - bounding.MinX, v2.pos.y - bounding.MinY);

		// TODO: We actually generate a lot of redundant cache textures that have the same content.

		// Constructs a cache key that we can use to see if there's a cached version of what we're about to render,
		// or if there doesn't exist anything, we create a cache item.
		const Target::TextureCacheKey key = std::make_tuple(
			offset0.x, offset0.y, v0.uv.x, v0.uv.y, v0.col,
			offset1.x, offset1.y, v1.uv.x, v1.uv.y, v1.col,
			offset2.x, offset2.y, v2.uv.x, v2.uv.y, v2.col,
			destination.w, destination.h);
		if (target.CacheTextures.count(key) > 0)
		{
			// TODO: Do we use this cache more than once per frame, per texture?

			SDL_RenderCopy(target.Renderer, target.CacheTextures.at(key), nullptr, &destination);

			return;
		}

		SDL_Texture* cacheTexture = target.MakeTexture(destination.w, destination.h);
		target.UseAsRenderTarget(cacheTexture);
		target.DisableClip();

		// Draw the triangle here.

		const Color color = Color(v0.col);

		std::vector<ImVec2> vertices = { offset0, offset1, offset2 };
		std::sort(vertices.begin(), vertices.end(), [](const ImVec2& a, const ImVec2& b) { return a.y < b.y; });

		const ImVec2& vertex0 = vertices.at(0);
		const ImVec2& vertex1 = vertices.at(1);
		const ImVec2& vertex2 = vertices.at(2);

		if (vertex1.y == vertex2.y)
		{
			DrawBottomFlatTriangle(target, vertex0.x, vertex0.y, vertex1.x, vertex1.y, vertex2.x, vertex2.y, color);
		}
		else if (vertex0.y == vertex1.y)
		{
			DrawTopFlatTriangle(target, vertex0.x, vertex0.y, vertex1.x, vertex1.y, vertex2.x, vertex2.y, color);
		}
		else
		{
			const ImVec2 vertex3 = ImVec2(
				vertex0.x + ((vertex1.y - vertex0.y) / (vertex2.y - vertex0.y)) * (vertex2.x - vertex0.x), vertex1.y);

			DrawBottomFlatTriangle(target, vertex0.x, vertex0.y, vertex1.x, vertex1.y, vertex3.x, vertex3.y, color);
			DrawTopFlatTriangle(target, vertex1.x, vertex1.y, vertex3.x, vertex3.y, vertex2.x, vertex2.y, color);
		}

		DrawLine(target, vertex0, vertex1, color);
		DrawLine(target, vertex1, vertex2, color);
		DrawLine(target, vertex2, vertex0, color);

		target.EnableClip();
		target.UseAsRenderTarget(nullptr);

		SDL_RenderCopy(target.Renderer, cacheTexture, nullptr, &destination);

		target.CacheTextures[key] = cacheTexture;
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

			return;
		}

		const SDL_Rect source = {
			bounding.MinU * texture->Surface->w,
			bounding.MinV * texture->Surface->h,
			(bounding.MaxU - bounding.MinU) * texture->Surface->w,
			(bounding.MaxV - bounding.MinV) * texture->Surface->h
		};

		SDL_SetTextureColorMod(texture->Source, color.R * 255, color.G * 255, color.B * 255);
		SDL_RenderCopy(target.Renderer, texture->Source, &source, &destination);
	}

	void DoImGuiRender(Target& target, ImDrawData* drawData)
	{
		for (int n = 0; n < drawData->CmdListsCount; n++)
		{
			auto cmdList = drawData->CmdLists[n];
			auto vertexBuffer = cmdList->VtxBuffer;  // vertex buffer generated by ImGui
			auto indexBuffer = cmdList->IdxBuffer.Data;   // index buffer generated by ImGui

			for (int cmd_i = 0; cmd_i < cmdList->CmdBuffer.Size; cmd_i++)
			{
				const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmd_i];

				const ClipRect clipRect = {
					pcmd->ClipRect.x,
					pcmd->ClipRect.y,
					pcmd->ClipRect.z - pcmd->ClipRect.x,
					pcmd->ClipRect.w - pcmd->ClipRect.y
				};
				target.SetClipRect(clipRect);

				if (pcmd->UserCallback)
				{
					pcmd->UserCallback(cmdList, pcmd);
				}
				else
				{
					const Texture* texture = static_cast<const Texture*>(pcmd->TextureId);

					// Loops over triangles.
					for (int i = 0; i + 3 <= pcmd->ElemCount; i += 3)
					{
						const ImDrawVert& v0 = vertexBuffer[indexBuffer[i + 0]];
						const ImDrawVert& v1 = vertexBuffer[indexBuffer[i + 1]];
						const ImDrawVert& v2 = vertexBuffer[indexBuffer[i + 2]];

						const Rect& bounding = CalculateBoundingBox(v0, v1, v2);

						// TODO: Optimize single color triangles.
						const bool isTriangleUniformColor = v0.col == v1.col && v1.col == v2.col;
						const bool doesTriangleUseOnlyColor = bounding.UsesOnlyColor(texture);

						// Actually, since we render a whole bunch of rectangles, we try to first detect those, and render them more efficiently.
						// How are rectangles detected? It's actually pretty simple: If all 6 vertices lie on the extremes of the bounding box, 
						// it's a rectangle.
						if (i + 6 <= pcmd->ElemCount)
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

						//DrawTriangleNoCache(target, vertexBuffer[indexBuffer[i + 0]], vertexBuffer[indexBuffer[i + 1]], vertexBuffer[indexBuffer[i + 2]], texture);

						// TODO: I just really need to figure out subpixel accuracy with fixed point or something.
						// http://www.dbfinteractive.com/forum/index.php?topic=6233.0

						if (isTriangleUniformColor && doesTriangleUseOnlyColor)
						{
							DrawTriangleNoCache(target, v0, v1, v2, texture);
						}
						else
						{
							DrawTriangle(target, v0, v1, v2, texture, bounding);
						}
					}
				}

				indexBuffer += pcmd->ElemCount;
			}
		}

		target.DisableClip();
	}
};