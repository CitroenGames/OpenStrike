#pragma once

#include "openstrike/app/application.hpp"

namespace openstrike
{
[[nodiscard]] ApplicationDefinition make_openstrike_application_definition();
int run_openstrike_application(const RuntimeConfig& config);
}
