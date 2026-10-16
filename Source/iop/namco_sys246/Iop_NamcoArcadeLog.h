#pragma once

#include "../../app_shared/Log.h"

//Card reader and shutter entries go through CLog like the rest of the emulator, so
//log.showprints and LOGGING_ENABLED decide whether they are produced, and they land in
//<Play! data path>/logs/arcade_card.log. Expanding to a macro keeps the arguments (string
//formatting and the hex dumps) out of the builds where logging is compiled out, where CLog
//itself is only a stub.
#if LOGGING_ENABLED
#define ArcadeLog(...) do { ::CLog::GetInstance().Print("arcade_card", __VA_ARGS__); } while(0)
#else
#define ArcadeLog(...) \
	do { } while(0)
#endif
