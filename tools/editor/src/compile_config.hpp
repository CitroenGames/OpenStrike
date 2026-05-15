#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

enum class BspMode { None = 0, Normal = 1, OnlyEntities = 2 };
enum class CompileMode { None = 0, Normal = 1, Fast = 2, Final = 3 };

struct CompileConfig
{
    // Tool paths
    std::string vbspPath;
    std::string vvisPath;
    std::string vradPath;
    std::string gameExePath;

    // Compile step modes
    BspMode     bspMode  = BspMode::Normal;
    CompileMode visMode  = CompileMode::Normal;
    CompileMode radMode  = CompileMode::Normal;

    // Options
    bool        hdrLight    = false;
    bool        dontRunGame      = false;
    bool        launchWithDebugger = false;
    std::string gameParams       = "+sv_lan 1";

    // Advanced RAD options
    float       aoRadius           = 48.0f;
    int         aoSamples          = 48;
    int         sunSamples         = 300;
    float       sampleNormalBias   = 0.25f;
    float       staticPropFudge    = 1.0f;
    bool        accurateFormFactor = false;
    bool        cpuOnlyLighting    = false;
    std::string extraRadParams;

    bool SaveToFile(const std::string& path) const
    {
        std::ofstream f(path);
        if (!f.is_open())
            return false;

        f << "[CompileConfig]\n";
        f << "vbsp_path=" << vbspPath << "\n";
        f << "vvis_path=" << vvisPath << "\n";
        f << "vrad_path=" << vradPath << "\n";
        f << "game_exe_path=" << gameExePath << "\n";
        f << "bsp_mode=" << (int)bspMode << "\n";
        f << "vis_mode=" << (int)visMode << "\n";
        f << "rad_mode=" << (int)radMode << "\n";
        f << "hdr_light=" << (hdrLight ? 1 : 0) << "\n";
        f << "dont_run_game=" << (dontRunGame ? 1 : 0) << "\n";
        f << "launch_with_debugger=" << (launchWithDebugger ? 1 : 0) << "\n";
        f << "game_params=" << gameParams << "\n";
        f << "ao_radius=" << aoRadius << "\n";
        f << "ao_samples=" << aoSamples << "\n";
        f << "sun_samples=" << sunSamples << "\n";
        f << "sample_normal_bias=" << sampleNormalBias << "\n";
        f << "static_prop_fudge=" << staticPropFudge << "\n";
        f << "accurate_form_factor=" << (accurateFormFactor ? 1 : 0) << "\n";
        f << "cpu_only_lighting=" << (cpuOnlyLighting ? 1 : 0) << "\n";
        f << "extra_rad_params=" << extraRadParams << "\n";
        return true;
    }

    bool LoadFromFile(const std::string& path)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '[')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "vbsp_path")         vbspPath = val;
            else if (key == "vvis_path")    vvisPath = val;
            else if (key == "vrad_path")    vradPath = val;
            else if (key == "game_exe_path") gameExePath = val;
            else if (key == "bsp_mode")     bspMode = (BspMode)std::stoi(val);
            else if (key == "vis_mode")     visMode = (CompileMode)std::stoi(val);
            else if (key == "rad_mode")     radMode = (CompileMode)std::stoi(val);
            else if (key == "hdr_light")    hdrLight = std::stoi(val) != 0;
            else if (key == "dont_run_game")          dontRunGame = std::stoi(val) != 0;
            else if (key == "launch_with_debugger")   launchWithDebugger = std::stoi(val) != 0;
            else if (key == "game_params")            gameParams = val;
            else if (key == "ao_radius")             aoRadius = std::stof(val);
            else if (key == "ao_samples")            aoSamples = std::stoi(val);
            else if (key == "sun_samples")           sunSamples = std::stoi(val);
            else if (key == "sample_normal_bias")    sampleNormalBias = std::stof(val);
            else if (key == "static_prop_fudge")     staticPropFudge = std::stof(val);
            else if (key == "accurate_form_factor")  accurateFormFactor = std::stoi(val) != 0;
            else if (key == "cpu_only_lighting")     cpuOnlyLighting = std::stoi(val) != 0;
            else if (key == "extra_rad_params")      extraRadParams = val;
        }
        return true;
    }
};
