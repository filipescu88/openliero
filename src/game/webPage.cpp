#include "webPage.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <string>

#if OPENLIERO_EMSCRIPTEN
#include <emscripten/emscripten.h>
#endif

namespace web_page {

bool IsRoomCode(std::string const& code) {
  return code.size() == 6 && std::ranges::all_of(code, [](char c) {
           return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
         });
}

namespace {

std::string CleanCandidate(std::string s) {
  std::erase_if(
      s, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '-'; });
  std::ranges::transform(s, s.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return IsRoomCode(s) ? s : std::string();
}

#if OPENLIERO_EMSCRIPTEN
std::string RunScript(std::string const& script) {
  char const* const kResult = emscripten_run_script_string(script.c_str());
  return kResult ? std::string(kResult) : std::string();
}

// JavaScript that builds the invite URL from the current page, keeping a
// ?relay= override (used for testing against another relay). `code` must
// already be validated: it is pasted into the script.
std::string InviteUrlJs(std::string const& code) {
  return "(function () {"
         "  var u = new URL(location.href);"
         "  var keep = u.searchParams.get('relay');"
         "  u.search = ''; u.hash = '';"
         "  if (keep) { u.searchParams.set('relay', keep); }"
         "  u.searchParams.set('join', '" +
         code +
         "');"
         "  return u.toString();"
         "})()";
}
#endif

}  // namespace

std::string ExtractRoomCode(std::string const& raw) {
  std::string upper = raw;
  std::ranges::transform(upper, upper.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });

  // 1) Invite URL (?join=CODE or &join=CODE).
  for (std::string::size_type pos = 0; (pos = upper.find("JOIN=", pos)) != std::string::npos;
       pos += 5) {
    if (pos == 0 || upper[pos - 1] == '?' || upper[pos - 1] == '&') {
      std::string::size_type const kStart = pos + 5;
      std::string::size_type end = kStart;
      while (end < raw.size() && raw[end] != '&' && raw[end] != '#' && raw[end] != '/' &&
             raw[end] != '"' && raw[end] != '\'' &&
             std::isspace(static_cast<unsigned char>(raw[end])) == 0) {
        ++end;
      }
      return CleanCandidate(raw.substr(kStart, end - kStart));
    }
  }
  if (upper.find("://") != std::string::npos) {
    return {};
  }

  // 2) Shared message such as "Kod pokoju: ABC123" or "ROOM CODE: ABC123".
  if (std::string::size_type const kColon = raw.rfind(':'); kColon != std::string::npos) {
    if (std::string code = CleanCandidate(raw.substr(kColon + 1)); !code.empty()) {
      return code;
    }
  }

  // 3) Plain code (with optional spaces or hyphens).
  return CleanCandidate(raw);
}

#if OPENLIERO_EMSCRIPTEN

std::string TakeJoinCodeFromUrl() {
  return ExtractRoomCode(
      RunScript("(function () {"
                "  var u = new URL(location.href);"
                "  var code = u.searchParams.get('join');"
                "  if (code === null) { return ''; }"
                "  u.searchParams.delete('join');"
                "  history.replaceState(null, '', u.toString());"
                "  return code;"
                "})()"));
}

std::string AskRoomCode() {
  return ExtractRoomCode(
      RunScript("(function () {"
                "  var c = window.prompt('Kod pokoju (6 znaków) lub wklej link zaproszenia:', '');"
                "  return c === null ? '' : c;"
                "})()"));
}

std::string InviteLink(std::string const& code) {
  if (!IsRoomCode(code)) {
    return {};
  }
  return RunScript(InviteUrlJs(code));
}

bool CopyInvite(std::string const& code) {
  if (!IsRoomCode(code)) {
    return false;
  }
  std::string const kScript =
      "(function () {"
      "  if (window.copyLieroInvite) { return window.copyLieroInvite('" +
      code +
      "') ? 'ok' : ''; }"
      "  var url = " +
      InviteUrlJs(code) +
      ";"
      "  if (navigator.clipboard && navigator.clipboard.writeText) {"
      "    navigator.clipboard.writeText(url).catch(function () {});"
      "    return 'ok';"
      "  }"
      "  return '';"
      "})()";
  return !RunScript(kScript).empty();
}

bool ShareInvite(std::string const& code) {
  if (!IsRoomCode(code)) {
    return false;
  }
  std::string const kScript =
      "(function () {"
      "  if (window.shareLieroInvite) { return window.shareLieroInvite('" +
      code +
      "'); }"
      "  var url = " +
      InviteUrlJs(code) +
      ";"
      "  var text = 'Zagraj ze mną w Liero! Kod pokoju: " +
      code +
      "';"
      "  if (navigator.clipboard && navigator.clipboard.writeText) {"
      "    navigator.clipboard.writeText(url).catch(function () {});"
      "  }"
      "  if (navigator.share) {"
      "    navigator.share({ title: 'Liero', text: text, url: url }).catch(function () {});"
      "    return 'share';"
      "  }"
      "  window.open('https://wa.me/?text=' + encodeURIComponent(text + '\\n' + url), '_blank');"
      "  return 'whatsapp';"
      "})()";
  return !RunScript(kScript).empty();
}

void SetWaitingRoom(std::string const& code) {
  std::string const kValid = IsRoomCode(code) ? code : std::string();
  std::string const kScript =
      "(function () {"
      "  if (window.setLieroInviteRoom) { window.setLieroInviteRoom('" +
      kValid +
      "'); }"
      "  return '';"
      "})()";
  (void)RunScript(kScript);
}

#else

std::string TakeJoinCodeFromUrl() { return {}; }
std::string AskRoomCode() { return {}; }
std::string InviteLink(std::string const& /*code*/) { return {}; }
bool CopyInvite(std::string const& code) {
  return IsRoomCode(code) && SDL_SetClipboardText(code.c_str());
}
bool ShareInvite(std::string const& /*code*/) { return false; }
void SetWaitingRoom(std::string const& /*code*/) {}

#endif

}  // namespace web_page
