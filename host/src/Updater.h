#pragma once
#include <string>

// Checks GitHub for a newer Tandem release and downloads it. Blocking calls: run
// them off the UI thread.
namespace updater {

struct Release {
    std::wstring version; // e.g. "1.4.0"
    std::wstring exeUrl;  // Tandem.exe asset, empty if the release has none
    std::wstring pageUrl; // release page with the notes
};

// Latest published release. False with `error` set when it could not be read.
bool Latest(Release& out, std::wstring& error);

// True when `version` ("1.4.0" or "v1.4.0") is newer than this build.
bool IsNewer(const std::wstring& version);

// Downloads `url` to `path` and checks it is a Windows program.
bool Download(const std::wstring& url, const std::wstring& path, std::wstring& error);

} // namespace updater
