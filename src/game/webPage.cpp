#include "webPage.hpp"

#include <algorithm>
#include <cctype>

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

std::string Normalize(std::string code) {
  std::erase_if(code, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
  std::ranges::transform(code, code.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return IsRoomCode(code) ? code : std::string();
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

#if OPENLIERO_EMSCRIPTEN

std::string TakeJoinCodeFromUrl() {
  return Normalize(
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
  return Normalize(
      RunScript("(function () {"
                "  var c = window.prompt('Kod pokoju (6 znaków):', '');"
                "  return c === null ? '' : c;"
                "})()"));
}

std::string InviteLink(std::string const& code) {
  if (!IsRoomCode(code)) {
    return {};
  }
  return RunScript(InviteUrlJs(code));
}

bool ShareInvite(std::string const& code) {
  if (!IsRoomCode(code)) {
    return false;
  }
  std::string const kScript =
      "(function () {"
      "  var url = " +
      InviteUrlJs(code) +
      ";"
      "  var text = 'Zagraj ze mną w Liero! Kod pokoju: " +
      code +
      "';"
      "  if (navigator.share) {"
      "    navigator.share({ title: 'Liero', text: text, url: url }).catch(function () {});"
      "    return 'share';"
      "  }"
      "  window.open('https://wa.me/?text=' + encodeURIComponent(text + '\\n' + url), '_blank');"
      "  return 'whatsapp';"
      "})()";
  return !RunScript(kScript).empty();
}

#else

std::string TakeJoinCodeFromUrl() { return {}; }
std::string AskRoomCode() { return {}; }
std::string InviteLink(std::string const& /*code*/) { return {}; }
bool ShareInvite(std::string const& /*code*/) { return false; }

#endif

}  // namespace web_page
