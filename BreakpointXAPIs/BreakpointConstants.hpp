#pragma once
#include <cstdint>
#include <limits>

// Event numbers use the SDK's 16-bit field!!!
namespace Linky::BreakpointX::EventNumber
{
	// Use this when we can't get the event number.
	constexpr int Unavailable = -1;
	constexpr int First = 1;
	constexpr int Last = (std::numeric_limits<std::uint16_t>::max)();

	/// @brief Check if this is a valid event number.
	/// @param value The event number to check against Fusion's supported range.
	constexpr bool IsValid(int value) { return value >= First && value <= Last; }
}

namespace Linky::BreakpointX::FusionRuntime
{
	// The old SDK doesn't have CFVERSION_25. We use the value from DarkEdif (stolen echs dee!!!)
	constexpr unsigned long Version25 = 0x02050000;
	// A negative object number means we've reached the end of the selected objects (at least from what I understood in my tests that is)...
	constexpr short NoObject = -1;
	constexpr int UnknownEventGroup = -1;
	constexpr int UnknownSelectionGeneration = -1;
}
