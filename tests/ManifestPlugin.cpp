// Test plugins for the manifest (consenttest, symtest). Built twice:
//   manifest_plugin.dll   a manifest a loader must not trust: it asks for a future API, its name fills the whole array
//                         without a terminating NUL, and its texts carry control characters. consenttest checks how it
//                         is read (cut, cleaned, never run); symtest's dry run reports it as a PROBLEM.
//   nomanifest_plugin.dll (DK2ML_TEST_NO_MANIFEST) a plugin without one (the manifest is optional)
#include "dk2ml.h"

#ifndef DK2ML_TEST_NO_MANIFEST
extern "C" __declspec(dllexport) const DK2ML_Manifest DK2ML_PluginManifest = {
    sizeof(DK2ML_Manifest),
    99,
    {'N', 'a', 'm', 'e', '\n', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X',
     'X', 'X', 'X', 'X', 'X',  'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X',
     'X', 'X', 'X', 'X', 'X',  'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'E'},
    "  9.9\t",
    "Someone\x01"
    "Else",
    "",
    0,
};
#endif

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API*, const DK2ML_PluginInfo*)
{
    return 0;
}
