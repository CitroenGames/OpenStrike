#pragma once

#include <cstdint>
#include <string>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

namespace openstrike
{
class LoadingScreenState;
struct LoadingScreenSnapshot;
struct RuntimeConfig;

class RmlLoadingScreenController final
{
public:
    RmlLoadingScreenController();
    ~RmlLoadingScreenController();

    RmlLoadingScreenController(const RmlLoadingScreenController&) = delete;
    RmlLoadingScreenController& operator=(const RmlLoadingScreenController&) = delete;

    bool initialize(Rml::Context& rml_context, const RuntimeConfig& config);
    void shutdown();
    void update(LoadingScreenState& state);

    [[nodiscard]] bool visible() const;

private:
    void refresh(const LoadingScreenSnapshot& snapshot);
    void set_text(const char* id, const std::string& value);
    void set_inner_rml(const char* id, const std::string& value);
    void set_percent_width(const char* id, float percent);
    void set_opacity(const char* id, float opacity);

    [[nodiscard]] Rml::Element* element(const char* id) const;

    Rml::ElementDocument* document_ = nullptr;
    std::uint64_t rendered_revision_ = UINT64_MAX;
    std::uint64_t auto_close_presented_revision_ = UINT64_MAX;
    bool visible_ = false;
};
}
