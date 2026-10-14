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

// Extracts a 6-character room code (upper-cased) from user input: a plain code
// (with optional spaces or hyphens), a "Kod pokoju: ABC123" message, or a full
// invite URL containing ?join=ABC123. Returns "" if no valid code is found.
std::string ExtractRoomCode(std::string const& raw);

// The room code from the page URL (?join=), upper-cased, or "" if there is
// none or it is malformed. Removes it from the address bar, so reloading the
// page does not try to rejoin a room that is long gone.
std::string TakeJoinCodeFromUrl();

// Asks for a room code with the browser's own dialog (a phone shows its
// keyboard for it). Accepts a code, an invite link, or a shared invite message.
// "" if cancelled or malformed.
std::string AskRoomCode();

// Invite link for `code`, e.g. https://liero.pl/?join=ABC123.
std::string InviteLink(std::string const& code);

// Copies the invite link (on the web) or the room code (on native builds) to
// the system clipboard.
bool CopyInvite(std::string const& code);

// Opens the phone's share sheet (WhatsApp, Messenger, SMS, ...) with an
// invite; without one, copies the link and opens WhatsApp. Must run soon after
// a tap or key press — browsers only allow sharing in response to the user.
bool ShareInvite(std::string const& code);

// Shows or hides the page's HTML invite bar while waiting for a peer in `code`
// (pass "" to hide). No-op on native builds.
void SetWaitingRoom(std::string const& code);

}  // namespace web_page
