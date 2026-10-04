// Manifest test plugins (consenttest, symtest), built twice:
//   manifest_plugin.dll   an untrustworthy manifest: a future API, a name filling the whole array without a NUL, texts
//                         with control characters. consenttest checks it is cut, cleaned, never run; symtest's dry run
//                         reports a PROBLEM.
//   nomanifest_plugin.dll (DK2ML_TEST_NO_MANIFEST) no manifest (it's optional)
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
