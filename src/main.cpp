#include "openstrike/app/application.hpp"
#include "openstrike/core/command_line.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/runtime_config.hpp"

#include <exception>

int main(int argc, char** argv)
{
    try
    {
        openstrike::CommandLine command_line(argc, argv);
        openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(command_line);
        return openstrike::run_application(config);
    }
    catch (const std::exception& error)
    {
        openstrike::log_error("fatal: {}", error.what());
        return 1;
    }
}

