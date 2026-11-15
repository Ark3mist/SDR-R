#pragma once

namespace osx {

// SDR++-style big 10-digit tuning display ("0.100.000.000").
//  - click upper / lower half of a digit: +/- one unit of that digit
//  - scroll wheel over a digit: +/- one unit per notch
//  - right-click a digit: zero every digit below it
// Returns true when *freqHz was changed. Range 0 .. 9 999 999 999 Hz.
bool FrequencySelect(const char* id, double* freqHz, float digitSize = 36.0f);

}  // namespace osx
