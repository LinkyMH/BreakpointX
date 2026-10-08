#pragma once
#include "BreakpointConstants.hpp"
struct tagRDATA;

namespace Linky::BreakpointX
{
	/// @brief The functions used by Fusion's actions, conditions, and expressions.
	/// Pass the calling instance so we can find the state for its runtime.
	class BreakpointAPI final
	{
	public:
		/// @brief Record an allowed hit and open the Inspector if we need to pause.
		/// @return False if we skip the pause or something fails, otherwise true when the pause ends.
		/// @param rdPtr The BreakpointX instance that reached Break Here.
		static bool Hit(tagRDATA* rdPtr) noexcept;

		/// @brief Check if breakpoints are enabled for this instance's runtime.
		/// @param rdPtr The BreakpointX instance whose runtime we want to check.
		static bool IsEnabled(tagRDATA* rdPtr) noexcept;

		/// @brief Enable or disable breakpoints for all instances in this runtime.
		/// @param rdPtr A BreakpointX instance in the runtime we want to change.
		/// @param enabled True to allow breakpoints, false to disable them.
		static void SetEnabled(tagRDATA* rdPtr, bool enabled) noexcept;

		/// @brief Switch between enabled and disabled using the same checks.
		/// @param rdPtr A BreakpointX instance in the runtime we want to change.
		static void ToggleEnabled(tagRDATA* rdPtr) noexcept;

		/// @brief Get the last recorded event number, including hits recorded after Continue.
		/// Returns Unavailable if this runtime hasn't recorded a hit yet.
		/// @param rdPtr The BreakpointX instance whose runtime holds the event number.
		static long GetLastBreakEvent(tagRDATA* rdPtr) noexcept;

		/// @brief Add a new instance. This can throw if we can't get memory for its state.
		/// @param rdPtr The new BreakpointX instance. Call this while its runtime header is valid.
		static void AttachTrace(tagRDATA* rdPtr);

		/// @brief Remove an instance. If it was the last one, close this runtime's Inspectors too.
		/// @param rdPtr The instance being destroyed. Call this before its runtime data is gone.
		static void DetachTrace(tagRDATA* rdPtr) noexcept;

	private:
		BreakpointAPI() = delete;
	};
}
