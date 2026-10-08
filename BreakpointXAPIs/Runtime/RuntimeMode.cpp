#include "RuntimeMode.hpp"
#include <algorithm>

namespace Linky::BreakpointX
{
	namespace
	{
		constexpr DWORD MaxModulePathCharacters = 32768;
		constexpr DWORD PathBufferGrowthFactor = 2;
		// Compare the paths, ignoring upper and lower case.
		bool EqualPath(const std::wstring& left, const std::wstring& right)
		{
			return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
				right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
		}

		// Remove this ending if it matches. Otherwise, leave the path as it is.
		bool RemoveSuffix(std::wstring& path, const wchar_t* suffix)
		{
			const std::wstring tail(suffix);
			if (path.size() < tail.size() || !EqualPath(path.substr(path.size() - tail.size()), tail))
				return false;
			path.resize(path.size() - tail.size());
			return true;
		}

		// Ask Windows for the full path. Try a bigger buffer if it doesn't fit.
		std::wstring ModulePath(HMODULE module)
		{
			for (DWORD capacity = MAX_PATH; ; capacity = std::min<DWORD>(capacity * PathBufferGrowthFactor, MaxModulePathCharacters))
			{
				std::wstring path(capacity, L'\0');
				const DWORD length = GetModuleFileNameW(module, &path[0], capacity);
				if (!length) return {};
				if (length < capacity) { path.resize(length); return path; }
				// Stop at this size so we don't keep trying forever.
				if (capacity == MaxModulePathCharacters) return {};
			}
			return {};
		}

		// Make sure it's a file. A folder with the same name won't do.
		bool FileExists(const std::wstring& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		}
	}

	// Check both paths to find the Fusion install they belong to.
	std::wstring FindEditorInstallation(std::wstring processPath, std::wstring extensionPath)
	{
		// Use the same slash style before checking the folders.
		std::replace(processPath.begin(), processPath.end(), L'/', L'\\');
		std::replace(extensionPath.begin(), extensionPath.end(), L'/', L'\\');
		// This follows DarkEdif's RunApplication check. We need to check more than the EXE name.
		if (!RemoveSuffix(processPath, L"\\edrt.exe") && !RemoveSuffix(processPath, L"\\edrtex.exe"))
			return {};
		// Remove the optional Unicode and Hwa folders to get back to Data\\Runtime.
		RemoveSuffix(processPath, L"\\Unicode");
		RemoveSuffix(processPath, L"\\Hwa");
		if (!RemoveSuffix(processPath, L"Data\\Runtime")) return {};
		if (processPath.empty() || processPath.back() != L'\\') return {};
		// Remove the MFX filename, then check if its Extensions folder belongs to this install.
		const auto separator = extensionPath.find_last_of(L'\\');
		if (separator == std::wstring::npos || separator + 1 == extensionPath.size()) return {};
		extensionPath.resize(separator);
		RemoveSuffix(extensionPath, L"\\Unicode");
		if (!EqualPath(extensionPath, processPath + L"Extensions")) return {};
		return processPath;
	}

	// Check the actual app and MFX paths, then look for the editor EXE in that install.
	bool IsEditorTestRun(HMODULE extensionModule)
	{
		if (!extensionModule) return false;
		const auto root = FindEditorInstallation(ModulePath(NULL), ModulePath(extensionModule));
		return !root.empty() && (FileExists(root + L"mmf2u.exe") || FileExists(root + L"mmf2.exe"));
	}
} // namespace Linky::BreakpointX
