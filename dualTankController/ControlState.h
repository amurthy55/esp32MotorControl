#pragma once

#include <cstdint>

constexpr uint32_t AC_FB_STABLE_MS = 2000;

struct FeedbackSnapshot {
  bool running;
  uint32_t changedAt;
  uint32_t runs;
};

struct ValveRecord {
  uint32_t version = 1;
  uint32_t lastFull = 0;
  uint32_t position = 2;
  uint32_t pending = 0;
  uint32_t cycle = 0;
  uint32_t message = 0;
  int64_t chat = 0;
  uint32_t publishAttempts = 0;
  uint32_t cleanup = 0;

  bool valid() const {
    return version == 1 && lastFull <= 2 && position >= 1 && position <= 2 &&
           pending <= 1 && cleanup <= 1 && message <= INT32_MAX &&
           publishAttempts <= 2 && (!pending || position == 1);
  }
};

struct MotorRun {
  bool active = false;
  bool gf = false;
  uint32_t completedRuns = 0;

  bool canStart(const FeedbackSnapshot &feedback, uint32_t now) const {
    return !active && !feedback.running && feedback.runs == completedRuns &&
           uint32_t(now - feedback.changedAt) >= AC_FB_STABLE_MS;
  }

  bool begin(bool groundFloor, const FeedbackSnapshot &feedback, uint32_t now) {
    if (!canStart(feedback, now)) return false;
    active = true;
    gf = groundFloor;
    return true;
  }

  void observe(const FeedbackSnapshot &feedback) {
    if (feedback.running || feedback.runs != completedRuns) active = true;
  }

  bool cancelUnobservedStart(const FeedbackSnapshot &feedback) {
    if (feedback.running || feedback.runs != completedRuns) return false;
    active = false;
    gf = false;
    return true;
  }

  bool complete(const FeedbackSnapshot &feedback, uint32_t now, bool &groundFloor) {
    if (feedback.running || feedback.runs == completedRuns ||
        uint32_t(now - feedback.changedAt) < AC_FB_STABLE_MS) return false;
    groundFloor = gf;
    completedRuns = feedback.runs;
    active = false;
    gf = false;
    return true;
  }
};
