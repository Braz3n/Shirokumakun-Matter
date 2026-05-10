/*
 * Matter AC Controller — application task.
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	}

	CHIP_ERROR StartApp();

private:
	CHIP_ERROR Init();
};
