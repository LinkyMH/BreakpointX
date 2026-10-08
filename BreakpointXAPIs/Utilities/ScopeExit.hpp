#pragma once
#include <utility>

namespace Linky::BreakpointX
{
	/// @brief Run some cleanup when this object goes out of scope, even if an exception happens.
	/// The cleanup must not throw. Anything it uses needs to stay alive until it runs.
	template<class F>
	class ScopeExit final
	{
	public:
		/// @brief Save the cleanup function for later.
		/// @param action The cleanup function to save and run on scope exit. It must not throw.
		explicit ScopeExit(F action) : _action(std::move(action)) {}

		// Don't copy this object. We'd end up doing the same cleanup twice.
		ScopeExit(const ScopeExit&) = delete;
		ScopeExit& operator=(const ScopeExit&) = delete;

		/// @brief Run the saved cleanup once.
		~ScopeExit() noexcept { _action(); }

	private:
		F _action;
	};
}
