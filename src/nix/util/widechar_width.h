// widechar_width.h
//
// Minimal stub for widechar character width detection.
// The full implementation is from https://github.com/ridiculousfish/widecharwidth
//
// For now we provide a simple fallback that assumes:
// - Control chars have width 0
// - ASCII printable chars have width 1
// - Everything else (CJK, emoji, etc.) has width 2 or 1

#pragma once

#include <cstdint>

// Return values for widechar_wcwidth
constexpr int widechar_nonprint = -1;
constexpr int widechar_combining = -2;
constexpr int widechar_ambiguous = -3;
constexpr int widechar_private_use = -4;
constexpr int widechar_unassigned = -5;
constexpr int widechar_widened_in_9 = -6;

// Simplified width calculation
// Returns the display width of a Unicode codepoint, or a negative constant
inline int widechar_wcwidth(uint32_t cp) {
  // C0 and C1 control characters
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
    return widechar_nonprint;
  }

  // ASCII printable
  if (cp < 0x7F) {
    return 1;
  }

  // Common combining marks (incomplete list, but covers most)
  if ((cp >= 0x0300 && cp <= 0x036F) || // Combining Diacritical Marks
      (cp >= 0x1AB0 && cp <= 0x1AFF) || // Combining Diacritical Marks Extended
      (cp >= 0x1DC0 && cp <= 0x1DFF) || // Combining Diacritical Marks Supplement
      (cp >= 0x20D0 && cp <= 0x20FF) || // Combining Diacritical Marks for Symbols
      (cp >= 0xFE20 && cp <= 0xFE2F)) { // Combining Half Marks
    return widechar_combining;
  }

  // CJK characters - typically double-width
  if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
      (cp >= 0x2E80 && cp <= 0x9FFF) ||   // CJK Radicals through CJK Unified Ideographs
      (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
      (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
      (cp >= 0xFE10 && cp <= 0xFE1F) ||   // Vertical Forms
      (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK Compatibility Forms
      (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth Forms
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Forms
      (cp >= 0x20000 && cp <= 0x2FFFF) || // CJK Extension B through F
      (cp >= 0x30000 && cp <= 0x3FFFF)) { // CJK Extension G+
    return 2;
  }

  // Emoji that are typically rendered wide
  if ((cp >= 0x1F300 &&
       cp <= 0x1F9FF) || // Misc Symbols and Pictographs through Supplemental Symbols
      (cp >= 0x1FA00 && cp <= 0x1FAFF)) { // Symbols and Pictographs Extended-A
    return 2;
  }

  // Private use areas
  if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFF) ||
      (cp >= 0x100000 && cp <= 0x10FFFF)) {
    return widechar_private_use;
  }

  // Default: single width
  return 1;
}
