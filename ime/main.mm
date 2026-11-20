// RDMachine Input: the server's half of typing with the client's input method
// (docs/ime.md). It composes nothing itself. The agent (rdmachine-server in
// this user's session) sends it the client's composition and committed text
// over a Unix socket; it puts them into the focused text field with
// setMarkedText / insertText, and reports back where the composition is, so
// the client can put its candidate window there. Keys always pass through.
//
// The agent's messages (4-byte length + JSON, as AgentChannel):
//   {"type":"mark","seq":1,"text":"ni'hao","sel":[6,0]}
//   {"type":"commit","seq":2,"text":"你好"}   -> {"type":"ack","seq":2,"ok":true}
//   {"type":"cancel","seq":3}
//   {"type":"report","delay":60}   -> a state after that many ms (a click or
//                                     a key may have moved the caret)
// Ours: {"type":"hello","version":1}, {"type":"client","active":true,"app":"..."},
//   {"type":"state","seq":1,"marked":[0,6],"sel":[6,0],"rect":[x,y,w,h]} (points,
//   top left of the primary display), {"type":"ended","seq":1} (the system
//   ended the composition: a click elsewhere, another app).
// Nothing typed is ever logged.
#import <Cocoa/Cocoa.h>
#import <InputMethodKit/InputMethodKit.h>
#include <os/log.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rdmachine/agent_link.h"
#include "rdmachine/ime_wire.h"

namespace {

os_log_t Log() {
  static os_log_t l = os_log_create("org.rdmachine.inputmethod", "ime");
  return l;
}

std::mutex gChannelMu;
std::shared_ptr<rdm::AgentChannel> gChannel;  // the agent, when connected

void SendToAgent(const nlohmann::json& m) {
  std::shared_ptr<rdm::AgentChannel> ch;
  {
    std::lock_guard lock(gChannelMu);
    ch = gChannel;
  }
  if (ch) ch->Send(m);
}

NSRange ToRange(const nlohmann::json& j) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number_unsigned() || !j[1].is_number_unsigned()) {
    return NSMakeRange(0, 0);
  }
  return NSMakeRange(j[0].get<NSUInteger>(), j[1].get<NSUInteger>());
}

nlohmann::json FromRange(NSRange r) {
  if (r.location == NSNotFound) return nullptr;
  return nlohmann::json::array({r.location, r.length});
}

}  // namespace

@interface RemoteController : IMKInputController
- (void)agentMessage:(const nlohmann::json&)m;
@end

// The controller of the text field that has the focus (one per field).
static __weak RemoteController* gCurrent;

@implementation RemoteController {
  NSUInteger _markedLength;  // UTF-16 length of what we marked, 0 = no composition
  uint64_t _seq;             // the last agent message applied here
  uint64_t _reportGen;       // later reports supersede earlier ones
  uint64_t _requestGen;      // the same for the agent's report requests
}

- (void)activateServer:(id)sender {
  gCurrent = self;
  SendToAgent({{"type", "client"}, {"active", true}, {"app", [[sender bundleIdentifier] ?: @"" UTF8String]}});
  // Where the caret is before anything is typed: the client's candidate
  // window then starts in the right place on the first character.
  [self reportAfter:30];
}

// A report `ms` from now; a later request replaces one still waiting.
- (void)reportAfter:(int)ms {
  const uint64_t gen = ++_requestGen;
  __weak RemoteController* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    RemoteController* me = weakSelf;
    if (me && me->_requestGen == gen && me->_markedLength == 0) [me reportSoon];
  });
  // Once more a little later: after a click some apps (Chromium) still give
  // the old place at first. The agent sends the client only what changed.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (ms + 250) * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    RemoteController* me = weakSelf;
    if (me && me->_requestGen == gen && me->_markedLength == 0) SendToAgent([me report]);
  });
}

- (void)deactivateServer:(id)sender {
  if (gCurrent == self) {
    gCurrent = nil;
    SendToAgent({{"type", "client"}, {"active", false}, {"app", [[sender bundleIdentifier] ?: @"" UTF8String]}});
  }
}

- (NSUInteger)recognizedEvents:(id)sender {
  return NSEventMaskKeyDown;
}

// Keys are the client's own business: what its input method did not take
// arrives here as a plain key and goes on to the app.
- (BOOL)handleEvent:(NSEvent*)event client:(id)sender {
  return NO;
}

// The system ends the composition (focus moves, the app changes): it goes in
// as it stands, and the client hears that it is over.
- (void)commitComposition:(id)sender {
  if (_markedLength == 0) return;
  NSRange mk = [sender markedRange];
  if (mk.location != NSNotFound && mk.length > 0) {
    NSAttributedString* s = [sender attributedSubstringFromRange:mk];
    if (s.length) [sender insertText:s.string replacementRange:NSMakeRange(NSNotFound, NSNotFound)];
  }
  _markedLength = 0;
  SendToAgent({{"type", "ended"}, {"seq", _seq}});
}

- (void)agentMessage:(const nlohmann::json&)m {
  id client = self.client;
  const std::string type = m.value("type", "");
  if (m.contains("seq")) _seq = m.value("seq", uint64_t{0});
  const NSRange none = NSMakeRange(NSNotFound, NSNotFound);
  if (type == "mark") {
    NSString* text = [NSString stringWithUTF8String:m.value("text", "").c_str()] ?: @"";
    NSRange sel = m.contains("sel") ? ToRange(m["sel"]) : NSMakeRange(text.length, 0);
    if (sel.location > text.length) sel = NSMakeRange(text.length, 0);
    NSDictionary* attrs = [self markForStyle:kTSMHiliteSelectedRawText atRange:NSMakeRange(0, text.length)];
    [client setMarkedText:[[NSAttributedString alloc] initWithString:text attributes:attrs]
           selectionRange:sel
         replacementRange:none];
    _markedLength = text.length;
    [self reportSoon];
  } else if (type == "commit") {
    NSString* text = [NSString stringWithUTF8String:m.value("text", "").c_str()] ?: @"";
    [client insertText:text replacementRange:none];
    _markedLength = 0;
    SendToAgent({{"type", "ack"}, {"seq", _seq}, {"ok", true}});
    [self reportSoon];  // where the next composition will start
  } else if (type == "report") {
    [self reportAfter:std::clamp(m.value("delay", 0), 0, 1000)];
  } else if (type == "cancel") {
    [client setMarkedText:@"" selectionRange:NSMakeRange(0, 0) replacementRange:none];
    _markedLength = 0;
  }
}

// Asks now, and again 16 and 60 ms later when the answer was empty or may
// still change: web content answers from a cache its renderer fills in
// shortly after the composition changes (docs/ime.md, 2).
- (void)reportSoon {
  const uint64_t gen = ++_reportGen;
  auto last = std::make_shared<nlohmann::json>([self report]);
  SendToAgent(*last);
  __weak RemoteController* weakSelf = self;
  for (int ms : {16, 60}) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      RemoteController* me = weakSelf;
      if (!me || me->_reportGen != gen) return;
      nlohmann::json again = [me report];
      if (again != *last) {
        SendToAgent(again);
        *last = std::move(again);
      }
    });
  }
}

// Where the composition starts: the first non-empty answer of its first
// character, the right edge of its last one, the right edge of the character
// before the caret, the caret itself.
- (nlohmann::json)report {
  id client = self.client;
  nlohmann::json r{{"type", "state"}, {"seq", _seq}};
  @try {
    const NSRange mk = [client markedRange];
    const NSRange sel = [client selectedRange];
    r["marked"] = FromRange(mk);
    r["sel"] = FromRange(sel);
    NSRect rect = NSZeroRect;
    auto ask = [&](NSUInteger index, bool right_edge) {
      NSRect line = NSZeroRect;
      [client attributesForCharacterIndex:index lineHeightRectangle:&line];
      if (line.size.height <= 0) return false;
      rect = line;
      if (right_edge) rect.origin.x += rect.size.width;
      rect.size.width = 1;
      return true;
    };
    bool found = false;
    if (_markedLength > 0) {
      // Composing: only where the composition starts, the client's candidate
      // window's anchor. Nothing found: no position, and the client keeps the
      // one it has. Never the caret's (it moves right with every letter, and
      // the window would follow it). Chromium's markedRange can be made up
      // (docs/ime.md, 2): the start is also the caret minus the composition
      // when the caret is at its end, as it is while typing.
      if (mk.location != NSNotFound) found = ask(mk.location, false);
      if (!found && sel.location != NSNotFound && sel.location >= _markedLength) {
        found = ask(sel.location - _markedLength, false);
      }
    } else if (sel.location != NSNotFound) {
      // The caret itself first: Chromium answers a query that starts at the
      // caret from its caret cache, anything else from a selection cache that
      // may still be where the caret was before a click. At the end of the
      // text (no character there) the right edge of the one before it.
      found = ask(sel.location, false) || (sel.location > 0 && ask(sel.location - 1, true));
    }
    if (found) {
      // Cocoa's screen coordinates (origin bottom left of the primary
      // display) to the top-left origin everything else uses.
      const CGFloat primary_height = NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0;
      r["rect"] = {rect.origin.x, primary_height - rect.origin.y - rect.size.height, rect.size.width,
                   rect.size.height};
    }
  } @catch (NSException* e) {
    os_log_error(Log(), "position query failed: %{public}@", e.reason);
  }
  return r;
}

@end

namespace {

// Keeps a connection to the agent, reconnecting every second while there is
// none (the agent starts and stops with sessions of RDMachine Server).
void AgentLoop() {
  const std::string path = rdm::ImeSocketPath();
  for (;;) {
    std::string err;
    const int fd = rdm::UnixConnect(path, &err);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    auto ch = std::make_shared<rdm::AgentChannel>(fd);
    {
      std::lock_guard lock(gChannelMu);
      gChannel = ch;
    }
    ch->Send({{"type", "hello"}, {"version", 1}});
    dispatch_async(dispatch_get_main_queue(), ^{
      if (RemoteController* c = gCurrent) {
        SendToAgent({{"type", "client"},
                     {"active", true},
                     {"app", [[c.client bundleIdentifier] ?: @"" UTF8String]}});
      }
    });
    os_log(Log(), "connected to the agent");
    for (;;) {
      rdm::AgentChannel::Received in = ch->Receive(-1);
      if (in.status != rdm::AgentChannel::Received::kMessage) break;
      auto m = std::make_shared<nlohmann::json>(std::move(in.message));
      dispatch_async(dispatch_get_main_queue(), ^{
        if (RemoteController* c = gCurrent) {
          [c agentMessage:*m];
        } else if (m->value("type", "") == "commit") {
          // No text field takes input methods now: the agent types it otherwise.
          SendToAgent({{"type", "ack"}, {"seq", m->value("seq", uint64_t{0})}, {"ok", false}});
        }
      });
    }
    {
      std::lock_guard lock(gChannelMu);
      gChannel.reset();
    }
    os_log(Log(), "agent gone");
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace

int main() {
  @autoreleasepool {
    NSBundle* b = [NSBundle mainBundle];
    IMKServer* server = [[IMKServer alloc] initWithName:[b objectForInfoDictionaryKey:@"InputMethodConnectionName"]
                                       bundleIdentifier:b.bundleIdentifier];
    if (!server) os_log_error(Log(), "no input method server");
    std::thread(AgentLoop).detach();
    [[NSApplication sharedApplication] run];
  }
  return 0;
}
