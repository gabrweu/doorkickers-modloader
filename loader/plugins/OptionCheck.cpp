// The rules for DK2ML_API::AddOption, shared by the loader and symtest's dry run, so the dry run refuses what the real
// loader would.
#include "Loader.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

namespace {

// sizeof(DK2ML_Option): structSize must be at least this, and bytes past it are ignored.
constexpr uint32_t kOptionMinSize = offsetof(DK2ML_Option, user) + sizeof(void*);

constexpr int kMaxChoices = 64;          // the refusal text below names it
constexpr size_t kMaxFormatLength = 63; // a longer format falls back to the default

bool IsDigitAt(const std::string& s, size_t i)
{
    return i < s.size() && isdigit(static_cast<unsigned char>(s[i]));
}

} // namespace

const char* Options_Check(const DK2ML_Option* plugin, DK2ML_Option* copy)
{
    *copy = {};
    if (!plugin || plugin->structSize < kOptionMinSize) {
        return "unknown option layout (set structSize = sizeof(DK2ML_Option))";
    }

    memcpy(copy, plugin, std::min<size_t>(plugin->structSize, sizeof(*copy)));
    const DK2ML_Option* option = copy;
    if (option->type < DK2ML_OPTION_HEADER || option->type > DK2ML_OPTION_BUTTON) {
        return "unknown option type";
    }

    bool needsValue = option->type != DK2ML_OPTION_HEADER && option->type != DK2ML_OPTION_BUTTON;
    if (needsValue && !option->value) {
        return "no value pointer";
    }

    bool isRange = option->type == DK2ML_OPTION_FLOAT || option->type == DK2ML_OPTION_INT;
    if (isRange && !(option->max > option->min)) {
        return "max must be greater than min";
    }

    bool choicesValid = option->choices && option->choiceCount > 0 && option->choiceCount <= kMaxChoices;
    if (option->type == DK2ML_OPTION_CHOICE && !choicesValid) {
        return "a choice needs 1 to 64 choices";
    }
    return nullptr;
}

std::string Options_SafeFormat(const std::string& format, bool integer)
{
    // A mod-supplied format must be one conversion of the right kind plus plain text, because anything else could read
    // past the arguments.
    const char* fallback = integer ? "%d" : "%.2f";
    const char* allowedTypes = integer ? "di" : "fgeFGE";
    int conversions = 0;

    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == '%') {
            ++i;
            continue;
        }

        // flags, width, precision, then the conversion type
        size_t j = i + 1;
        while (j < format.size() && strchr("-+ #0", format[j])) {
            ++j;
        }
        while (IsDigitAt(format, j)) {
            ++j;
        }
        if (j < format.size() && format[j] == '.') {
            ++j;
            while (IsDigitAt(format, j)) {
                ++j;
            }
        }
        if (j >= format.size() || !strchr(allowedTypes, format[j])) {
            return fallback;
        }

        ++conversions;
        i = j;
    }
    return conversions == 1 && format.size() <= kMaxFormatLength ? format : fallback;
}
