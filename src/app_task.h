/*
 * Matter AC Controller — application task.
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

struct Identify;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	}

	CHIP_ERROR StartApp();

	static void IdentifyStartHandler(Identify *ident);
	static void IdentifyStopHandler(Identify *ident);

private:
	CHIP_ERROR Init();
};
