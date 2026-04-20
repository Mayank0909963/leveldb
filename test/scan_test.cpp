// test/stress_test.cc
// Auto stress tester with model-based correctness checking
// Build: g++ test/stress_test.cc -Iinclude -Lbuild -lleveldb -lpthread -o stress_test
// Run:   ./stress_test

#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/options.h"

// ── config ────────────────────────────────────────────────────────────────────

static const int NUM_ROUNDS        = 200;   // total random operation rounds
static const int KEY_SPACE         = 100;   // keys are key000..key099
static const int FLUSH_EVERY       = 30;    // compact to SST every N rounds

// ── globals ───────────────────────────────────────────────────────────────────

static int total_tests  = 0;
static int total_passed = 0;
static int total_failed = 0;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string MakeKey(int n) {
  char buf[16];
  snprintf(buf, sizeof(buf), "key%03d", n);
  return buf;
}

static std::string MakeVal(int n, int round) {
  char buf[32];
  snprintf(buf, sizeof(buf), "val%03d_r%d", n, round);
  return buf;
}

static leveldb::DB* OpenDB(const std::string& path) {
  leveldb::Options opts;
  opts.create_if_missing = true;
  leveldb::DB* db;
  assert(leveldb::DB::Open(opts, path, &db).ok());
  return db;
}

static void CHECK(const std::string& label, bool condition, const std::string& detail = "") {
  total_tests++;
  if (condition) {
    total_passed++;
  } else {
    total_failed++;
    std::cout << "  FAIL [" << label << "]" << (detail.empty() ? "" : ": " + detail) << "\n";
  }
}

// ── model-based tester ────────────────────────────────────────────────────────

class ModelTester {
 public:
  ModelTester(leveldb::DB* db) : db_(db) {}

  // Put into both DB and model
  void Put(const std::string& k, const std::string& v) {
    assert(db_->Put(leveldb::WriteOptions(), k, v).ok());
    model_[k] = v;
  }

  // Delete from both
  void Delete(const std::string& k) {
    assert(db_->Delete(leveldb::WriteOptions(), k).ok());
    model_.erase(k);
  }

  // DeleteRange on both
  void DeleteRange(const std::string& start, const std::string& end) {
    assert(db_->DeleteRange(leveldb::WriteOptions(), start, end).ok());
    // erase from model
    auto it = model_.lower_bound(start);
    while (it != model_.end() && it->first < end) {
      it = model_.erase(it);
    }
  }

  // Verify Scan matches model for range [start, end)
  void CheckScan(const std::string& start, const std::string& end, const std::string& label) {
    // get expected from model
    std::vector<std::pair<std::string, std::string>> expected;
    for (auto it = model_.lower_bound(start); it != model_.end() && it->first < end; ++it) {
      expected.push_back(*it);
    }

    // get actual from DB
    std::vector<std::pair<std::string, std::string>> actual;
    assert(db_->Scan(leveldb::ReadOptions(), start, end, &actual).ok());

    // compare count
    CHECK(label + "/count",
          actual.size() == expected.size(),
          "expected " + std::to_string(expected.size()) +
          " got " + std::to_string(actual.size()) +
          " in [" + start + ", " + end + ")");

    // compare each entry
    size_t n = std::min(actual.size(), expected.size());
    for (size_t i = 0; i < n; i++) {
      CHECK(label + "/key[" + std::to_string(i) + "]",
            actual[i].first == expected[i].first,
            "expected key " + expected[i].first + " got " + actual[i].first);
      CHECK(label + "/val[" + std::to_string(i) + "]",
            actual[i].second == expected[i].second,
            "key " + actual[i].first +
            ": expected " + expected[i].second + " got " + actual[i].second);
    }

    // check sorted order
    for (size_t i = 1; i < actual.size(); i++) {
      CHECK(label + "/order",
            actual[i].first > actual[i-1].first,
            "keys not sorted: " + actual[i-1].first + " then " + actual[i].first);
    }
  }

  // Verify individual Gets match model
  void CheckGet(const std::string& k, const std::string& label) {
    std::string val;
    leveldb::Status s = db_->Get(leveldb::ReadOptions(), k, &val);

    bool in_model = model_.count(k) > 0;
    bool in_db    = s.ok();

    CHECK(label + "/exists[" + k + "]",
          in_model == in_db,
          in_model ? "should exist but missing in DB"
                   : "should be deleted but found in DB with val=" + val);

    if (in_model && in_db) {
      CHECK(label + "/value[" + k + "]",
            val == model_[k],
            "expected " + model_[k] + " got " + val);
    }
  }

  const std::map<std::string, std::string>& model() const { return model_; }

 private:
  leveldb::DB* db_;
  std::map<std::string, std::string> model_;
};

// ── random stress test ────────────────────────────────────────────────────────

void RunStressTest(leveldb::DB* db) {
  std::cout << "\n=== Stress Test: " << NUM_ROUNDS << " random rounds ===\n";
  ModelTester tester(db);
  srand(42);  // fixed seed for reproducibility

  for (int round = 0; round < NUM_ROUNDS; round++) {
    int op = rand() % 4;

    if (op == 0) {
      // PUT: random key, new value
      int k = rand() % KEY_SPACE;
      tester.Put(MakeKey(k), MakeVal(k, round));

    } else if (op == 1) {
      // DELETE: random single key
      int k = rand() % KEY_SPACE;
      tester.Delete(MakeKey(k));

    } else if (op == 2) {
      // DELETERANGE: random [a, b) where a < b
      int a = rand() % (KEY_SPACE - 1);
      int b = a + 1 + rand() % 10;  // b is 1..10 ahead of a
      if (b >= KEY_SPACE) b = KEY_SPACE - 1;
      if (a < b) tester.DeleteRange(MakeKey(a), MakeKey(b));

    } else {
      // SCAN CHECK: random range and verify
      int a = rand() % (KEY_SPACE - 1);
      int b = a + 1 + rand() % 20;
      if (b >= KEY_SPACE) b = KEY_SPACE - 1;
      if (a < b) {
        tester.CheckScan(MakeKey(a), MakeKey(b),
                         "round" + std::to_string(round));
      }
    }

    // periodically flush to SST so we test cross-layer behavior
    if (round % FLUSH_EVERY == 0) {
      db->CompactRange(nullptr, nullptr);
    }

    // periodically do a full Get check on all keys
    if (round % 50 == 0) {
      for (int k = 0; k < KEY_SPACE; k++) {
        tester.CheckGet(MakeKey(k), "round" + std::to_string(round));
      }
    }
  }

  // final full scan check
  tester.CheckScan(MakeKey(0), MakeKey(KEY_SPACE - 1), "final_scan");

  // final full get check
  for (int k = 0; k < KEY_SPACE; k++) {
    tester.CheckGet(MakeKey(k), "final_get");
  }
}

// ── edge case battery ─────────────────────────────────────────────────────────

void RunEdgeCases(leveldb::DB* db) {
  std::cout << "\n=== Edge Case Battery ===\n";
  ModelTester tester(db);

  // 1. overwrite same key many times
  for (int i = 0; i < 50; i++) tester.Put("dup", "v" + std::to_string(i));
  tester.CheckScan("dup", "dupe", "overwrite");
  tester.CheckGet("dup", "overwrite");

  // 2. delete then re-put
  tester.Put("ghost", "alive");
  tester.Delete("ghost");
  tester.Put("ghost", "reborn");
  tester.CheckGet("ghost", "delete_reput");

  // 3. deleterange then re-put inside range
  for (int i = 0; i < 10; i++) tester.Put("r" + std::to_string(i), "v");
  tester.DeleteRange("r3", "r7");
  tester.Put("r5", "restored");
  tester.CheckScan("r0", "r9", "reput_after_deleterange");

  // 4. scan entire empty db range
  tester.CheckScan("zzz0", "zzz9", "empty_range");

  // 5. single key range
  tester.Put("solo", "only");
  tester.CheckScan("solo", "solp", "single_key_range");

  // 6. flush then check
  db->CompactRange(nullptr, nullptr);
  tester.CheckScan("r0", "r9", "after_flush");
  tester.CheckGet("r5", "after_flush");

  std::cout << "  Edge cases done.\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
  const std::string kPath = "/tmp/stress_test_db";
  system(("rm -rf " + kPath).c_str());
  leveldb::DB* db = OpenDB(kPath);

  RunEdgeCases(db);
  RunStressTest(db);

  delete db;
  system(("rm -rf " + kPath).c_str());

  std::cout << "\n──────────────────────────────\n";
  std::cout << "Total checks : " << total_tests  << "\n";
  std::cout << "Passed       : " << total_passed << "\n";
  std::cout << "Failed       : " << total_failed << "\n";
  if (total_failed == 0)
    std::cout << "ALL PASSED ✓\n";
  else
    std::cout << "FAILURES: " << total_failed << " — see above\n";

  return total_failed == 0 ? 0 : 1;
}