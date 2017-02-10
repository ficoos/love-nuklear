#ifndef NUKLEAR_H
#define NUKLEAR_H

// LOVE
#include "common/Module.h"

using namespace love;

namespace nuklear {
class Nuklear: public Module
{
private:
	Nuklear();

public:
	virtual ~Nuklear();

	// Implements Module.
	virtual ModuleType getModuleType() const
	{
		return M_PLUGIN;
	}

	virtual const char *getName() const
	{
		return "nuklear";
	}

	static Nuklear instance;
};
};

#endif /* NUKLEAR_H */
