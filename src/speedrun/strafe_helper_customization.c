#include "strafe_helper/strafe_helper_customization.h"

#include "shared/shared.h"
#include "refresh/refresh.h"

#include <assert.h>
#include <math.h>


static uint32_t getColorForElement(const enum shc_ElementId element_id)
{
	switch (element_id) {
		case shc_ElementId_AcceleratingAngles:
			return MakeColor(0, 128, 32, 96);
		case shc_ElementId_OptimalAngle:
			return MakeColor(0, 255, 64, 192);
		case shc_ElementId_CenterMarker:
			return MakeColor(255, 255, 255, 192);
		case shc_ElementId_SpeedText:
			return MakeColor(255, 255, 255, 224);
	}
	assert(0);
	return MakeColor(255, 255, 255, 255);
}

void shc_drawFilledRectangle(const float x, const float y,
                             const float w, const float h,
                             const enum shc_ElementId element_id)
{
	const uint32_t color = getColorForElement(element_id);
	R_DrawFill32(roundf(x), roundf(y), roundf(w), roundf(h), color);
}
