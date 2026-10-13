#pragma once

// The page the web build runs in: invite links and sharing. Every function is
// a no-op returning empty/false on native builds.
//
// An invite link is the game's own URL with ?join=<room code>; opening it joins
// that room directly (no typing on a phone).

#include <string>

namespace web_page {

// A room code as the relay hands them out: 6 characters, A-Z / 0-9.
bool IsRoomCode(std::string const& code);

// The room code from the page URL (?join=), upper-cased, or "" if there is
// none or it is malformed. Removes it from the address bar, so reloading the
// page does not try to rejoin a room that is long gone.
std::string TakeJoinCodeFromUrl();

// Asks for a room code with the browser's own dialog (a phone shows its
// keyboard for it). "" if cancelled or malformed.
std::string AskRoomCode();

// Invite link for `code`, e.g. https://liero.pl/?join=ABC123.
std::string InviteLink(std::string const& code);

// Opens the phone's share sheet (WhatsApp, Messenger, SMS, ...) with an
// invite; without one, WhatsApp directly. Must run soon after a tap or key
// press — browsers only allow sharing in response to the user.
bool ShareInvite(std::string const& code);

}  // namespace web_page
