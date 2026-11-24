/* Keep RGB colour codes legible while retaining their hue. */
#ifndef TOUCH_TEXTCOLOR_H
#define TOUCH_TEXTCOLOR_H
static void Touch_TextColorLift(float color[4])
{
	float y = 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
	if (y < 0.30f)
	{
		float amount = (0.30f - y) / (1.0f - y);
		int i;
		for (i = 0; i < 3; ++i)
			color[i] += (1.0f - color[i]) * amount;
	}
}
#endif
