#include "Nuklear.h"

namespace nuklear {

Nuklear Nuklear::instance;

Nuklear::Nuklear()
{
	retain();
}

Nuklear::~Nuklear()
{
}
};
