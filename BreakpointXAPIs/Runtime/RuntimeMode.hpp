#pragma once
#include <string>
#include <windows.h>

namespace Linky::BreakpointX
{
	/// @brief Check if the app and extension paths belong to the same Fusion editor install.
	/// @return The install folder with a slash at the end, or an empty string if they don't match.
	/// This checks the folders. IsEditorTestRun also looks for the editor EXE.
	/// @param processPath The full path to the running app's EXE. We work on a copy.
	/// @param extensionPath The full path to the loaded MFX. We work on a copy.
	std::wstring FindEditorInstallation(std::wstring processPath, std::wstring extensionPath);

	/// @brief Check if we're running through the Fusion editor.
	/// @param extensionModule The loaded MFX whose path we want to check.
	bool IsEditorTestRun(HMODULE extensionModule);
} // namespace Linky::BreakpointX
