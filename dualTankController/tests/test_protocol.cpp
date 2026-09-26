#include <cassert>
#include <iostream>
#include <string>
#include "../ControlState.h"
#include "../TelegramProtocol.h"

HttpResponse http(const std::string &wire) {
  HttpResponse response;
  for (char c : wire) response.feed(c);
  return response;
}

int main() {
  MotorRun motor;
  FeedbackSnapshot feedback{false, 0, 0};
  bool gf = true;
  assert(!motor.canStart(feedback, 1999));
  assert(motor.canStart(feedback, 2000));
  assert(!motor.complete(feedback, 10000, gf));
  assert(motor.begin(true, feedback, 2000));
  assert(!motor.begin(false, feedback, 2500));
  assert(motor.cancelUnobservedStart(feedback));
  assert(!motor.active && !motor.gf && motor.canStart(feedback, 5000));

  assert(motor.begin(true, feedback, 5000));
  feedback = {true, 5100, 1};
  assert(!motor.cancelUnobservedStart(feedback));
  feedback = {false, 5200, 1};
  assert(!motor.complete(feedback, 7199, gf));
  assert(motor.complete(feedback, 7200, gf) && gf);
  assert(!motor.complete(feedback, 7200, gf));

  feedback = {true, 7300, 2};
  motor.observe(feedback);
  assert(motor.active && !motor.gf);
  feedback = {false, 7400, 2};
  assert(!motor.begin(true, feedback, 7500));
  feedback = {false, 8500, 3};
  assert(!motor.complete(feedback, 10499, gf));
  assert(motor.complete(feedback, 10500, gf) && !gf);
  motor.completedRuns = UINT32_MAX;
  feedback = {false, UINT32_MAX - 999, 0};
  assert(!motor.complete(feedback, 999, gf));
  assert(motor.complete(feedback, 1000, gf));
  std::cout << "PASS motor: idle, failure, short run, manual FF, pending stop, hidden edge, rollover\n";

  auto batch = parseJson(R"({"ok":true,"result":[
    {"update_id":1,"callback_query":{"id":"q1","data":"MOTOR_ON_GF",
      "message":{"message_id":77,"chat":{"id":-123},"text":"Controller"}}},
    {"update_id":2,"message":{"message_id":78,"chat":{"id":-456},"text":"/commands"}}
  ]})");
  assert(batch && telegramOk(batch.get()));
  const cJSON *updates = jsonField(batch.get(), "result");
  TelegramUpdate first = decodeUpdate(cJSON_GetArrayItem(updates, 0));
  TelegramUpdate second = decodeUpdate(cJSON_GetArrayItem(updates, 1));
  assert(first.id == 1 && first.chat == -123 && first.message == 77 &&
         first.callback == "MOTOR_ON_GF" && first.text.empty() && first.query == "q1");
  assert(second.id == 2 && second.chat == -456 && second.text == "/commands" &&
         second.callback.empty() && second.query.empty());
  assert(!parseJson(R"({"ok":true,"result":{"message_id":42)"));
  assert(!parseJson(R"({"ok":true}garbage)"));
  assert(!telegramOk(parseJson(R"({"ok":"true"})").get()));
  assert(jsonInteger(parseJson(R"({"id":1.5})").get(), "id") == 0);
  assert(normalizeCommand("/update_valve_position@PumpBot GF", "PumpBot") ==
         "/update_valve_position GF");
  assert(normalizeCommand("/commands@OtherBot", "PumpBot").empty());
  assert(normalizeCommand("/commands@PumpBot", "PumpBot") == "/commands");
  assert(normalizeCommand("/commands@pumpbot", "PumpBot") == "/commands");
  assert(normalizeCommand("/update_valve_position FF", "PumpBot") ==
         "/update_valve_position FF");
  const std::string text = "name \"quoted\" & percent% \n next";
  auto quoted = parseJson(quoteJson(text.c_str()).c_str());
  assert(quoted && std::string(quoted->valuestring) == text);
  std::cout << "PASS JSON: separate updates/chats, truncation, types, addressed commands, escaping\n";

  auto response = http("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}");
  assert(response.state == HttpResponse::State::Complete && response.status == 200);
  assert(telegramOk(parseJson(response.body.c_str()).get()));
  auto chunked = http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "5;ext=x\r\n{\"ok\"\r\n6\r\n:true}\r\n0\r\nX-Test: yes\r\n\r\n");
  assert(chunked.state == HttpResponse::State::Complete && chunked.body == response.body);
  auto truncated = http("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{\"ok\":true}");
  truncated.close();
  assert(truncated.state == HttpResponse::State::Invalid);
  auto badChunk = http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n");
  assert(badChunk.state == HttpResponse::State::Invalid);
  auto unfinishedChunk = http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nB\r\n{\"ok\":true}\r\n");
  unfinishedChunk.close();
  assert(unfinishedChunk.state == HttpResponse::State::Invalid);
  auto conflicting = http("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nTransfer-Encoding: chunked\r\n\r\n");
  assert(conflicting.state == HttpResponse::State::Invalid);
  auto oversized = http("HTTP/1.1 200 OK\r\nContent-Length: 40000\r\n\r\n");
  assert(oversized.state == HttpResponse::State::Invalid);
  auto closed = http("HTTP/1.0 200 OK\r\n\r\n{\"ok\":true}");
  closed.close();
  assert(closed.state == HttpResponse::State::Complete);
  auto invalidStatus = http("HTTP/1.1 200oops\r\nContent-Length: 11\r\n\r\n{\"ok\":true}");
  assert(invalidStatus.state == HttpResponse::State::Invalid);
  auto headerLimit = http("HTTP/1.1 200 OK\r\nX-Header: " + std::string(4096, 'x'));
  assert(headerLimit.state == HttpResponse::State::Invalid);
  std::cout << "PASS HTTP: lengths, chunks/trailers, incomplete bodies, invalid framing, size cap\n";

  ValveRecord state;
  assert(state.valid());
  state.pending = 1;
  assert(!state.valid());
  state.position = 1;
  assert(state.valid());
  state.version = 99;
  assert(!state.valid());
  std::cout << "PASS persistence schema validation\n";
}
