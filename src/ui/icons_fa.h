// Adonai — FontAwesome 6 Free (Solid) icon codepoints, merged into the UI font.
// Subset of juliettef/IconFontCppHeaders. The .ttf is data/fonts/fa-solid-900.ttf.
#pragma once

// Range covering the FA6 solid glyphs (used to build the merge glyph range).
#define ICON_MIN_FA 0xe005
#define ICON_MAX_FA 0xf8ff

// --- window chrome ---
#define ICON_FA_LOCK "\xef\x80\xa3"           // U+f023
#define ICON_FA_LOCK_OPEN "\xef\x8f\x81"      // U+f3c1
#define ICON_FA_THUMBTACK "\xef\x82\x8d"      // U+f08d  (always-on-top)
#define ICON_FA_WINDOW_MINIMIZE "\xef\x8b\x91" // U+f2d1
#define ICON_FA_XMARK "\xef\x80\x8d"          // U+f00d  (close)
#define ICON_FA_CHEVRON_DOWN "\xef\x81\xb8"   // U+f078  (title dropdown)
#define ICON_FA_UP_DOWN_LEFT_RIGHT "\xef\x82\xb2" // U+f0b2 (move handle)
#define ICON_FA_EXPAND "\xef\x81\xa5"         // U+f065  (maximize glyph)
#define ICON_FA_POWER_OFF "\xef\x80\x91"      // U+f011  (close/power)

// --- top tab bar (Lucifer-style) ---
#define ICON_FA_USERS "\xef\x83\x80"          // U+f0c0  (fleet)
#define ICON_FA_USER_PLUS "\xef\x88\xb4"      // U+f234  (add bot)
#define ICON_FA_LAYER_GROUP "\xef\x97\xbd"    // U+f5fd  (accounts / bulk import)
#define ICON_FA_NETWORK_WIRED "\xef\x9b\xbf"  // U+f6ff  (proxy)
#define ICON_FA_ROBOT "\xef\x95\x84"          // U+f544  (bots)
#define ICON_FA_TERMINAL "\xef\x84\xa0"       // U+f120  (console)
#define ICON_FA_GLOBE "\xef\x82\xac"          // U+f0ac  (world / proxy)
#define ICON_FA_LIST "\xef\x80\xba"           // U+f03a  (list)
#define ICON_FA_CODE "\xef\x84\xa1"           // U+f121  (executor)
#define ICON_FA_DATABASE "\xef\x87\x80"       // U+f1c0  (database)
#define ICON_FA_RIGHT_LEFT "\xef\x8d\xa2"     // U+f362  (switch)
#define ICON_FA_MAGNIFYING_GLASS "\xef\x80\x82" // U+f002 (search)
#define ICON_FA_FLOPPY_DISK "\xef\x83\x87"    // U+f0c7  (save)
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc"    // U+f07c  (load)

// --- actions / status ---
#define ICON_FA_GEAR "\xef\x80\x93"           // U+f013
#define ICON_FA_PLAY "\xef\x81\x8b"           // U+f04b
#define ICON_FA_CIRCLE_STOP "\xef\x8a\x8d"    // U+f28d
#define ICON_FA_TRASH "\xef\x87\xb8"          // U+f1f8
#define ICON_FA_CIRCLE_CHECK "\xef\x81\x98"   // U+f058
#define ICON_FA_CIRCLE_XMARK "\xef\x81\x97"   // U+f057
#define ICON_FA_ARROWS_ROTATE "\xef\x8b\xb1"  // U+f2f1  (reconnect)
#define ICON_FA_ARROW_LEFT "\xef\x81\xa0"     // U+f060  (back / move left)
#define ICON_FA_ARROW_UP "\xef\x81\xa2"       // U+f062  (move up)
#define ICON_FA_ARROW_RIGHT "\xef\x81\xa1"    // U+f061  (move right)
#define ICON_FA_ARROW_DOWN "\xef\x81\xa3"     // U+f063  (move down)
#define ICON_FA_RIGHT_TO_BRACKET "\xef\x8b\xb6" // U+f2f6 (enter door)
#define ICON_FA_CLOCK "\xef\x80\x97"          // U+f017  (intervals)
#define ICON_FA_SHIRT "\xef\x95\x93"          // U+f553  (wear)
#define ICON_FA_DOWN_LONG "\xef\x8c\x89"      // U+f309  (drop)
#define ICON_FA_PLUS "\x2b"                    // simple '+'
