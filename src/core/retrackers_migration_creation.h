#pragma once

namespace core {
class Download;

// One main-thread factory invocation owns the migration apply command.
class RetrackersMigrationCreation {
public:
  explicit RetrackersMigrationCreation(Download* download)
    : m_previous(s_candidate), m_previous_attempted(s_attempted), m_previous_applied(s_applied) {
    s_candidate = download;
    s_attempted = false;
    s_applied = false;
  }
  ~RetrackersMigrationCreation() {
    s_candidate = m_previous;
    s_attempted = m_previous_attempted;
    s_applied = m_previous_applied;
  }
  static bool in_scope(Download* download) { return s_candidate == download; }
  static bool mark_apply(Download* download) {
    if (s_candidate != download) return false;
    s_attempted = true;
    return true;
  }
  bool attempted() const { return s_attempted; }
  static bool mark_applied(Download* download) {
    if (s_candidate != download || !s_attempted) return false;
    s_applied = true;
    return true;
  }
  bool applied() const { return s_applied; }
  // Internal factory rollback may remove only its own uncommitted candidate.
  class ScopedAbortErase {
  public:
    explicit ScopedAbortErase(Download* download) : m_previous(s_abort_erase) {
      s_abort_erase = download;
    }
    ~ScopedAbortErase() { s_abort_erase = m_previous; }
  private:
    Download* m_previous;
  };
  static bool abort_erase_permitted(Download* download) {
    return s_candidate == download && s_attempted && s_abort_erase == download;
  }
private:
  Download* m_previous;
  bool m_previous_attempted;
  bool m_previous_applied;
  inline static thread_local Download* s_candidate = nullptr;
  inline static thread_local bool s_attempted = false;
  inline static thread_local bool s_applied = false;
  inline static thread_local Download* s_abort_erase = nullptr;
};
}
