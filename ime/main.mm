// RDMachine Input: the server's half of typing with the client's input method
// (docs/ime.md). It composes nothing itself. The agent (rdmachine-server in
// this user's session) sends it the client's composition and committed text
// over a Unix socket; it puts them into the focused text field with
// setMarkedText / insertText, and reports back where the composition is, so
// the client can put its candidate window there. Keys always pass through.
//
// The agent's messages (4-byte length + JSON, as AgentChannel); "seq" is the
// client's number, "id" the agent's own (rising while the agent runs):
//   {"type":"mark","seq":1,"id":7,"text":"ni'hao","sel":[6,0]}
//   {"type":"commit","seq":2,"id":8,"text":"你好"} -> {"type":"ack","seq":2,"id":8,"ok":true}
//   {"type":"cancel","seq":3,"id":9}
//   {"type":"report","delay":60}   -> a state after that many ms (a click or
//                                     a key may have moved the caret)
// Ours: {"type":"hello","version":2}, {"type":"client","active":true,"app":"...","ctl":3},
//   {"type":"state","seq":1,"after":7,"ctl":3,"active":true,"app":"...","marked":[0,6],"sel":[6,0],
//   "rect":[x,y,w,h]} (points, top left of the primary display; "after": the
//   last id put in when the report was made, by any controller),
//   {"type":"ended","seq":1,"id":7} (the system ended the composition of mark
//   7: a click elsewhere, another app). "ctl" tells controllers apart: one per
//   text field, and macOS activates and deactivates them in no fixed order.
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
- (uint64_t)ctl;
@end

// The controller of the text field that has the focus (one per field).
static __weak RemoteController* gCurrent;
// The same, not zeroed when it goes away (a weak reference is zeroed before
// dealloc runs): who has to say that no field has the focus now.
static const void* gCurrentId;
// The last agent id put in (by whichever controller, or dropped for want of
// one): what a report made now reflects. Main thread; 0 when a new agent
// connects (its ids start again).
static uint64_t gAppliedId;
static uint64_t gNextCtl = 1;

@implementation RemoteController {
  NSUInteger _markedLength;  // UTF-16 length of what we marked, 0 = no composition
  NSString* _markedText;     // what we marked
  uint64_t _markId;          // the agent's id of that mark
  // Its field does not hold the compositions put into it (Claude's other
  // view, say): the agent hears that no field takes input methods, so the
  // client shows the composition itself and the text goes in as typed.
  BOOL _refusing;
  uint64_t _markGen;         // later marks supersede the check of an earlier one
  uint64_t _seq;             // the last agent message applied here (the client's number)
  uint64_t _ctl;             // this controller, for the agent
  uint64_t _reportGen;       // later reports supersede earlier ones
  uint64_t _requestGen;      // the same for the agent's report requests
}

- (instancetype)initWithServer:(IMKServer*)server delegate:(id)delegate client:(id)client {
  self = [super initWithServer:server delegate:delegate client:client];
  if (self) _ctl = gNextCtl++;
  os_log(Log(), "new controller %llu app=%{public}s", _ctl, [[client bundleIdentifier] ?: @"" UTF8String]);
  return self;
}

- (void)dealloc {
  os_log(Log(), "controller gone %llu", _ctl);
  if (gCurrentId == (__bridge const void*)self) {
    // Gone while it had the focus: no deactivate came, but no field is there.
    gCurrentId = nullptr;
    SendToAgent({{"type", "client"}, {"active", false}, {"app", ""}, {"ctl", _ctl}});
  }
}

- (uint64_t)ctl {
  return _ctl;
}

// Whether the field holds a composition now (what we marked, or one it made
// up: Chromium's marked range can be).
- (BOOL)holdsComposition:(id)client {
  NSRange mk = NSMakeRange(NSNotFound, 0);
  @try {
    mk = [client markedRange];
  } @catch (NSException*) {
  }
  return mk.location != NSNotFound && mk.length > 0;
}

// The field turned out not to hold compositions, or to hold them after all.
- (void)setRefusing:(BOOL)refusing client:(id)client {
  if (refusing == _refusing) return;
  _refusing = refusing;
  os_log(Log(), "%llu %{public}s", _ctl,
         refusing ? "does not hold the composition: no field that takes input methods" : "holds compositions again");
  SendToAgent({{"type", "client"}, {"active", !refusing}, {"app", [[client bundleIdentifier] ?: @"" UTF8String]},
               {"ctl", _ctl}});
}

// This controller's field has the focus now.
- (void)becomeCurrent:(id)sender {
  gCurrent = self;
  gCurrentId = (__bridge const void*)self;
  _refusing = NO;  // a field that just took the focus is given the benefit of the doubt
  // A composition it remembers that the field no longer has (it was ended
  // or replaced while another controller had the focus) is over.
  if (_markedLength > 0) {
    NSRange mk = NSMakeRange(NSNotFound, 0);
    @try {
      mk = [sender markedRange];
    } @catch (NSException*) {
    }
    if (mk.location == NSNotFound || mk.length == 0) {
      _markedLength = 0;
      _markedText = nil;
      _markId = 0;
    }
  }
  SendToAgent({{"type", "client"}, {"active", true}, {"app", [[sender bundleIdentifier] ?: @"" UTF8String]},
               {"ctl", _ctl}});
  // Where the caret is before anything is typed: the client's candidate
  // window then starts in the right place on the first character.
  [self reportAfter:30];
}

- (void)activateServer:(id)sender {
  // Which controller, for which app, and which one was current: the order
  // macOS calls these in is what decides "a field has the focus" (no text).
  RemoteController* cur = gCurrent;
  os_log(Log(), "activate %llu app=%{public}s current=%llu", _ctl, [[sender bundleIdentifier] ?: @"" UTF8String],
         cur ? [cur ctl] : 0);
  [self becomeCurrent:sender];
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
    if (me && me->_requestGen == gen && me->_markedLength == 0 && gCurrent == me) SendToAgent([me report]);
  });
}

- (void)deactivateServer:(id)sender {
  RemoteController* cur = gCurrent;
  os_log(Log(), "deactivate %llu app=%{public}s current=%llu", _ctl, [[sender bundleIdentifier] ?: @"" UTF8String],
         cur ? [cur ctl] : 0);
  if (cur == self) {
    gCurrent = nil;
    gCurrentId = nullptr;
    SendToAgent(
        {{"type", "client"}, {"active", false}, {"app", [[sender bundleIdentifier] ?: @"" UTF8String]}, {"ctl", _ctl}});
  }
}

- (NSUInteger)recognizedEvents:(id)sender {
  return NSEventMaskKeyDown;
}

// Keys are the client's own business: what its input method did not take
// arrives here as a plain key and goes on to the app.
//
// Only the controller of the field the keys go to gets them. macOS sometimes
// ends a burst of activations with a deactivate of the field that still has
// the focus (Claude, after a reconnect or an app switch: docs/ime.md), and no
// activate follows until the focus moves; a key coming here says this
// controller has the focus after all (a typed commit, an arrow, Return).
- (BOOL)handleEvent:(NSEvent*)event client:(id)sender {
  if (gCurrent != self) {
    os_log(Log(), "a key came to %llu while it was not current: it has the focus", _ctl);
    [self becomeCurrent:sender];
  }
  return NO;
}

// The system ends the composition (focus moves, the app changes): it goes in
// as it stands, and the client hears that it is over.
- (void)commitComposition:(id)sender {
  os_log(Log(), "commitComposition %llu marked=%lu", _ctl, static_cast<unsigned long>(_markedLength));
  if (_markedLength == 0) return;
  // Only while the field still holds a composition: Chromium may already have
  // kept it as plain text (losing the focus), and putting it in again would
  // have it twice. Then what we marked ourselves, not what the app says its
  // marked range holds: Chromium's marked range can be made up (docs/ime.md).
  NSRange mk = NSMakeRange(NSNotFound, 0);
  @try {
    mk = [sender markedRange];
  } @catch (NSException*) {
  }
  if (mk.location != NSNotFound && mk.length > 0 && _markedText.length) {
    [sender insertText:_markedText replacementRange:NSMakeRange(NSNotFound, NSNotFound)];
  }
  const uint64_t mark = _markId;
  _markedLength = 0;
  _markedText = nil;
  _markId = 0;
  SendToAgent({{"type", "ended"}, {"seq", _seq}, {"id", mark}});
}

- (void)agentMessage:(const nlohmann::json&)m {
  id client = self.client;
  const std::string type = m.value("type", "");
  if (m.contains("seq")) _seq = m.value("seq", uint64_t{0});
  const uint64_t agent_id = m.value("id", uint64_t{0});
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
    _markedText = text;
    _markId = agent_id;
    if (_refusing && [self holdsComposition:client]) [self setRefusing:NO client:client];
    [self reportSoon];
    // Still nothing marked a little later (web content answers from a cache
    // its renderer fills in shortly after): this field does not take it.
    const uint64_t gen = ++_markGen;
    __weak RemoteController* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      RemoteController* me = weakSelf;
      if (!me || me->_markGen != gen || me->_markedLength == 0 || gCurrent != me) return;
      id c = me.client;
      if (![me holdsComposition:c]) [me setRefusing:YES client:c];
    });
  } else if (type == "commit") {
    NSString* text = [NSString stringWithUTF8String:m.value("text", "").c_str()] ?: @"";
    if (_markedLength > 0 && ![self holdsComposition:client]) {
      // Its composition never showed up in the field: it does not take the
      // text this way either. The agent types it, as a keyboard would. (Had
      // it been there after all, only slow to say so, it goes first: no
      // pinyin left next to the typed text.)
      [client setMarkedText:@"" selectionRange:NSMakeRange(0, 0) replacementRange:none];
      _markedLength = 0;
      _markedText = nil;
      _markId = 0;
      ++_markGen;
      [self setRefusing:YES client:client];
      SendToAgent({{"type", "ack"}, {"seq", _seq}, {"id", agent_id}, {"ok", false}});
      return;
    }
    [client insertText:text replacementRange:none];
    _markedLength = 0;
    _markedText = nil;
    _markId = 0;
    SendToAgent({{"type", "ack"}, {"seq", _seq}, {"id", agent_id}, {"ok", true}});
    [self reportSoon];  // where the next composition will start
  } else if (type == "report") {
    [self reportAfter:std::clamp(m.value("delay", 0), 0, 1000)];
  } else if (type == "cancel") {
    [client setMarkedText:@"" selectionRange:NSMakeRange(0, 0) replacementRange:none];
    _markedLength = 0;
    _markedText = nil;
    _markId = 0;
  }
}

// Asks now, and again 16 and 60 ms later when the answer was empty or may
// still change: web content answers from a cache its renderer fills in
// shortly after the composition changes (docs/ime.md, 2).
- (void)reportSoon {
  // Only the field with the focus says where the caret is: a controller that
  // lost it still has timers running (6.3.9).
  if (gCurrent != self) return;
  const uint64_t gen = ++_reportGen;
  auto last = std::make_shared<nlohmann::json>([self report]);
  SendToAgent(*last);
  __weak RemoteController* weakSelf = self;
  for (int ms : {16, 60}) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      RemoteController* me = weakSelf;
      if (!me || me->_reportGen != gen || gCurrent != me) return;
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
  nlohmann::json r{{"type", "state"}, {"seq", _seq}, {"after", gAppliedId}, {"ctl", _ctl}};
  // Whether this is the field that has the focus, each time: the agent's
  // sessions come and go, and the "client" message is sent only on a change.
  r["active"] = gCurrent == self && !_refusing;
  r["app"] = [[client bundleIdentifier] ?: @"" UTF8String];
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
    ch->Send({{"type", "hello"}, {"version", 2}});
    dispatch_async(dispatch_get_main_queue(), ^{
      gAppliedId = 0;  // this agent's ids start again
      if (RemoteController* c = gCurrent) {
        SendToAgent({{"type", "client"},
                     {"active", true},
                     {"app", [[c.client bundleIdentifier] ?: @"" UTF8String]},
                     {"ctl", [c ctl]}});
      }
    });
    os_log(Log(), "connected to the agent");
    for (;;) {
      rdm::AgentChannel::Received in = ch->Receive(-1);
      if (in.status != rdm::AgentChannel::Received::kMessage) break;
      auto m = std::make_shared<nlohmann::json>(std::move(in.message));
      dispatch_async(dispatch_get_main_queue(), ^{
        // Before it is applied: a report made while applying it reflects it.
        if (auto it = m->find("id"); it != m->end() && it->is_number_unsigned()) gAppliedId = it->get<uint64_t>();
        if (RemoteController* c = gCurrent) {
          [c agentMessage:*m];
        } else if (m->value("type", "") == "commit") {
          // No text field takes input methods now: the agent types it otherwise.
          SendToAgent({{"type", "ack"},
                       {"seq", m->value("seq", uint64_t{0})},
                       {"id", m->value("id", uint64_t{0})},
                       {"ok", false}});
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
