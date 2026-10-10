// Engine-free tests have no Plugify host. Strong fakes in individual tests override these.
#include <plg/plugin.hpp>
namespace plg {
GetBaseDirFn GetBaseDir __attribute__((weak)) = nullptr;
GetExtensionsDirFn GetExtensionsDir __attribute__((weak)) = nullptr;
GetConfigsDirFn GetConfigsDir __attribute__((weak)) = nullptr;
GetDataDirFn GetDataDir __attribute__((weak)) = nullptr;
GetLogsDirFn GetLogsDir __attribute__((weak)) = nullptr;
GetCacheDirFn GetCacheDir __attribute__((weak)) = nullptr;
IsLoadedFn IsLoaded __attribute__((weak)) = nullptr;
LogFn Log __attribute__((weak)) = nullptr;
BeginZoneFn BeginZone __attribute__((weak)) = nullptr;
EndZoneFn EndZone __attribute__((weak)) = nullptr;
}
