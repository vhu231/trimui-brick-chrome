"""Voice input for the Brick, via Chrome's Web Speech API.

Recognition has to happen inside Chrome -- it is the thing with the speech
engine -- but the pad daemon lives outside it, so this drives Chrome over the
DevTools protocol.

It is a session rather than a one-shot call because the useful mode is
push-to-talk: SpeechRecognition defaults to continuous=false and stops at the
first pause, which only ever captured the opening phrase.  Here recognition runs
continuous and the caller ends it by releasing the button.

Nothing here touches X: it runs on a worker thread and python-xlib connections
are not thread-safe.
"""

import json
import urllib.request

import websocket

DEBUG_PORT = 9222

_START = """
(function () {
  var Recognition = window.SpeechRecognition || window.webkitSpeechRecognition;
  if (!Recognition) return 'no-speech-api';
  try { if (window.__brickVoice) window.__brickVoice.recognition.abort(); } catch (e) {}
  var state = {final: '', interim: '', error: null, ended: false,
               started: false, audio: false};
  var recognition = new Recognition();
  recognition.lang = %s;
  recognition.continuous = true;
  recognition.interimResults = true;
  recognition.maxAlternatives = 1;
  // Rebuild the whole transcript every time rather than appending from
  // event.resultIndex.  results is cumulative and Chrome may re-report a result
  // that is already final, so appending duplicated those segments, and an index
  // that skipped ahead dropped others.  Rebuilding is idempotent.
  recognition.onresult = function (event) {
    var settled = '', pending = '';
    for (var i = 0; i < event.results.length; i++) {
      var text = event.results[i][0].transcript;
      if (event.results[i].isFinal) settled += text; else pending += text;
    }
    state.final = settled;
    state.interim = pending;
  };
  // Readiness, so the caller only invites the user to speak once capture is
  // actually running: onstart is the service listening, onaudiostart is the
  // microphone open.
  recognition.onstart = function () { state.started = true; };
  recognition.onaudiostart = function () { state.audio = true; };
  recognition.onerror = function (event) { state.error = String(event.error); };
  recognition.onend = function () { state.ended = true; };
  window.__brickVoice = {recognition: recognition, state: state};
  try { recognition.start(); } catch (e) { return String(e); }
  return 'started';
})()
"""

_POLL = """
(function () {
  var voice = window.__brickVoice;
  if (!voice) return null;
  return {text: voice.state.final + voice.state.interim,
          error: voice.state.error,
          ready: !!(voice.state.audio || voice.state.started)};
})()
"""

# stop() only asks the engine to finalise; the last isFinal result arrives just
# afterwards, so wait for onend before collecting.
_STOP = """
new Promise(function (resolve) {
  var voice = window.__brickVoice;
  if (!voice) { resolve({ok: false, error: 'not-started'}); return; }
  try { voice.recognition.stop(); } catch (e) {}
  var waited = 0;
  var timer = setInterval(function () {
    waited += 100;
    if (voice.state.ended || waited >= 4000) {
      clearInterval(timer);
      window.__brickVoice = null;
      var text = voice.state.final || voice.state.interim;
      if (!text && voice.state.error) resolve({ok: false, error: voice.state.error});
      else resolve({ok: true, text: text});
    }
  }, 100);
})
"""


class SpeechError(Exception):
    pass


MESSAGES = {
    "no-speech-api": "这个 Chrome 没有语音接口",
    "not-allowed": "网页未授权麦克风",
    "service-not-allowed": "网页未授权麦克风",
    "no-speech": "没有听到说话",
    "audio-capture": "麦克风打不开",
    "network": "语音服务连不上",
    "aborted": "识别被中断",
    "not-started": "识别没能启动",
}


def describe(error):
    return MESSAGES.get(error, "语音失败: %s" % error)


def _page_target(port):
    """The foreground tab, which /json lists most-recently-used first."""
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/json" % port, timeout=5) as response:
            targets = json.load(response)
    except Exception as exc:
        raise SpeechError("Chrome 调试端口没开 (%s)" % exc.__class__.__name__)
    for target in targets:
        if target.get("type") == "page" and target.get("webSocketDebuggerUrl"):
            return target
    raise SpeechError("没有可用的网页标签")


class Session:
    def __init__(self, port=DEBUG_PORT):
        self.port = port
        self.connection = None
        self._next_id = 0

    def _evaluate(self, expression, await_promise=False):
        self._next_id += 1
        message_id = self._next_id
        self.connection.send(json.dumps({
            "id": message_id,
            "method": "Runtime.evaluate",
            "params": {
                "expression": expression,
                "awaitPromise": await_promise,
                "returnByValue": True,
                # SpeechRecognition refuses to start without one.
                "userGesture": True,
            },
        }))
        while True:
            message = json.loads(self.connection.recv())
            if message.get("id") == message_id:
                break
        if "error" in message:
            raise SpeechError(str(message["error"].get("message", "unknown")))
        result = message.get("result", {})
        if "exceptionDetails" in result:
            raise SpeechError("页面脚本出错")
        return result.get("result", {}).get("value")

    def start(self, language="zh-CN"):
        target = _page_target(self.port)
        try:
            self.connection = websocket.create_connection(
                target["webSocketDebuggerUrl"], timeout=40,
                # DevTools answers 403 to any handshake carrying an Origin
                # header, which websocket-client adds by default.
                suppress_origin=True)
        except Exception as exc:
            raise SpeechError("连不上 Chrome (%s)" % exc.__class__.__name__)
        outcome = self._evaluate(_START % json.dumps(language))
        if outcome != "started":
            self.close()
            raise SpeechError(describe(outcome or "unknown"))

    def state(self):
        """Best-effort {text, error, ready}; never raises."""
        try:
            value = self._evaluate(_POLL)
        except Exception:
            return None
        return value if isinstance(value, dict) else None

    def stop(self):
        try:
            value = self._evaluate(_STOP, await_promise=True)
        finally:
            self.close()
        if not isinstance(value, dict):
            raise SpeechError("没有拿到识别结果")
        if not value.get("ok"):
            raise SpeechError(describe(value.get("error", "unknown")))
        return value.get("text", "")

    def close(self):
        if self.connection is not None:
            try:
                self.connection.close()
            except Exception:
                pass
            self.connection = None
