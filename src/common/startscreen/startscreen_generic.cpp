/*
** st_start.cpp
** Generic startup screen
**
**---------------------------------------------------------------------------
** Copyright 2022 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "startscreen.h"
#include "filesystem.h"
#include "printf.h"
#include "startupinfo.h"
#include "image.h"
#include "texturemanager.h"

#define ST_PROGRESS_X			64
#define ST_PROGRESS_Y			441

// GetHexChar is defined in startscreen.cpp — returns a pointer to the 8×16
// or 16×16 bitmap data for a Unicode codepoint from the GNU Unifont.
extern uint8_t* GetHexChar(int codepoint);

// Draw a string onto a bitmap at N× pixel scale.
// x, y are pixel coordinates of the top-left corner.
// Each font cell is 8 px wide (or 16 for wide chars), 16 px tall — scaled by N.
static void DrawScaledString(FBitmap& bmp, int x, int y, int scale,
                              const char* text, RgbQuad fg)
{
	const RgbQuad transparent = { 0, 0, 0, 0 };
	int cx = x;
	for (const uint8_t* s = (const uint8_t*)text; *s; ++s)
	{
		const uint8_t* src = GetHexChar(*s);
		if (!src) { cx += 8 * scale; continue; }
		int wide   = (*src == 32) ? 1 : 0;
		int charW  = wide ? 16 : 8;
		++src; // skip width byte

		for (int row = 0; row < 16; ++row)
		{
			for (int half = 0; half <= wide; ++half)
			{
				uint8_t bits = *src++;
				for (int col = 0; col < 8; ++col)
				{
					if (!((bits >> (7 - col)) & 1)) continue;
					int px0 = cx + (half * 8 + col) * scale;
					int py0 = y  + row * scale;
					for (int sy = 0; sy < scale; ++sy)
					for (int sx = 0; sx < scale; ++sx)
					{
						int px = px0 + sx, py = py0 + sy;
						if (px >= 0 && py >= 0 &&
						    px < bmp.GetWidth() && py < bmp.GetHeight())
							reinterpret_cast<RgbQuad*>(bmp.GetPixels())[py * bmp.GetWidth() + px] = fg;
					}
				}
			}
		}
		cx += charW * scale;
	}
}

// Returns the pixel width of a string rendered at the given scale.
static int ScaledStringWidth(const char* text, int scale)
{
	int w = 0;
	for (const uint8_t* s = (const uint8_t*)text; *s; ++s)
	{
		const uint8_t* src = GetHexChar(*s);
		if (!src) { w += 8 * scale; continue; }
		int charW = (*src == 32) ? 16 : 8;
		w += charW * scale;
	}
	return w;
}


class FGenericStartScreen : public FStartScreen
{
	int NotchPos = 0;

public:
	FGenericStartScreen(int max_progress);

	bool DoProgress(int) override;
};


//==========================================================================
//
// FGenericStartScreen Constructor
//
// Shows the Hexen startup screen. If the screen doesn't appear to be
// valid, it sets hr for a failure.
//
// The startup graphic is a planar, 4-bit 640x480 graphic preceded by a
// 16 entry (48 byte) VGA palette.
//
//==========================================================================

FGenericStartScreen::FGenericStartScreen(int max_progress)
	: FStartScreen(max_progress)
{
	const int W = 640 * 2;   // 1280
	const int H = 480 * 2;   // 960

	StartupBitmap.Create(W, H);
	ClearBlock(StartupBitmap, { 0, 0, 0, 255 }, 0, 0, W, H);

	// "DomeDoom" in large white letters, centered vertically and horizontally.
	// Font cells are 8×16 px; scale=8 gives 64×128 px per character.
	const int titleScale = 8;
	const int titleH     = 16 * titleScale;  // 128 px tall
	const RgbQuad white  = { 255, 255, 255, 255 };

	const char* title    = "DomeDoom";
	int titleW = ScaledStringWidth(title, titleScale);
	int titleX = (W - titleW) / 2;
	int titleY = (H - titleH) / 2 - titleScale * 4;  // slightly above centre

	DrawScaledString(StartupBitmap, titleX, titleY, titleScale, title, white);

	// "Fulldome Edition" in smaller text below.
	const int subScale   = 3;
	const int subH       = 16 * subScale;
	const RgbQuad grey   = { 180, 180, 180, 255 };

	const char* subtitle = "Fulldome Edition";
	int subW = ScaledStringWidth(subtitle, subScale);
	int subX = (W - subW) / 2;
	int subY = titleY + titleH + subScale * 6;

	DrawScaledString(StartupBitmap, subX, subY, subScale, subtitle, grey);
}

//==========================================================================
//
// FGenericStartScreen :: Progress
//
// Bumps the progress meter one notch.
//
//==========================================================================

bool FGenericStartScreen::DoProgress(int advance)
{
	int notch_pos;

	if (CurPos < MaxPos)
	{
		RgbQuad bcolor = { 177, 77, 16, 255 }; // [Nash June 2025] don't merge this color into VKDoom! // todo: make configurable 
		int numnotches = 200 * 2;
		notch_pos = ((CurPos + 1) * numnotches) / MaxPos;
		if (notch_pos != NotchPos)
		{ // Time to draw another notch.
			ClearBlock(StartupBitmap, bcolor, (320 - 100) * 2, 480 * 2 - 30, notch_pos, 4 * 2);
			NotchPos = notch_pos;
			if (StartupTexture)
				StartupTexture->CleanHardwareData(true);
		}
	}
	return FStartScreen::DoProgress(advance);
}


FStartScreen* CreateGenericStartScreen(int max_progress)
{
	return new FGenericStartScreen(max_progress);
}
