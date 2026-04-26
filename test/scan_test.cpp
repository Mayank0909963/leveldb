// test/scan_test.cpp — Comprehensive edge-case + stress tests
// Build: g++ test/scan_test.cpp -Iinclude -Lbuild -lleveldb -lpthread -std=c++17 -o scan_test

#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

// ── config ────────────────────────────────────────────────────────────────────

static const int NUM_ROUNDS  = 300;
static const int KEY_SPACE   = 100;
static const int FLUSH_EVERY = 30;

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

static leveldb::DB* OpenFreshDB(const std::string& path) {
  system(("rm -rf " + path).c_str());
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

  void Put(const std::string& k, const std::string& v) {
    assert(db_->Put(leveldb::WriteOptions(), k, v).ok());
    model_[k] = v;
  }

  void Delete(const std::string& k) {
    assert(db_->Delete(leveldb::WriteOptions(), k).ok());
    model_.erase(k);
  }

  void DeleteRange(const std::string& start, const std::string& end) {
    assert(db_->DeleteRange(leveldb::WriteOptions(), start, end).ok());
    auto it = model_.lower_bound(start);
    while (it != model_.end() && it->first < end) {
      it = model_.erase(it);
    }
  }

  void CheckScan(const std::string& start, const std::string& end, const std::string& label) {
    std::vector<std::pair<std::string, std::string>> expected;
    for (auto it = model_.lower_bound(start); it != model_.end() && it->first < end; ++it) {
      expected.push_back(*it);
    }

    std::vector<std::pair<std::string, std::string>> actual;
    assert(db_->Scan(leveldb::ReadOptions(), start, end, &actual).ok());

    CHECK(label + "/count",
          actual.size() == expected.size(),
          "expected " + std::to_string(expected.size()) +
          " got " + std::to_string(actual.size()) +
          " in [" + start + ", " + end + ")");

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

    for (size_t i = 1; i < actual.size(); i++) {
      CHECK(label + "/order",
            actual[i].first > actual[i-1].first,
            "keys not sorted: " + actual[i-1].first + " then " + actual[i].first);
    }
  }

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

  void CheckAllKeys(const std::string& label) {
    for (auto& kv : model_) {
      CheckGet(kv.first, label);
    }
  }

  void CheckFullScan(const std::string& label) {
    // Scan the entire key space
    std::vector<std::pair<std::string, std::string>> actual;
    assert(db_->Scan(leveldb::ReadOptions(), "", "~", &actual).ok());  // '~' > all printable
    CHECK(label + "/full_count",
          actual.size() == model_.size(),
          "expected " + std::to_string(model_.size()) +
          " got " + std::to_string(actual.size()));
    size_t i = 0;
    for (auto it = model_.begin(); it != model_.end() && i < actual.size(); ++it, ++i) {
      CHECK(label + "/full_key[" + std::to_string(i) + "]",
            actual[i].first == it->first, "");
      CHECK(label + "/full_val[" + std::to_string(i) + "]",
            actual[i].second == it->second, "");
    }
  }

  leveldb::DB* db() { return db_; }
  const std::map<std::string, std::string>& model() const { return model_; }

 private:
  leveldb::DB* db_;
  std::map<std::string, std::string> model_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// SCAN EDGE CASES
// ═══════════════════════════════════════════════════════════════════════════════

void TestScanEdgeCases(leveldb::DB* db) {
  std::cout << "\n=== Scan Edge Cases ===\n";
  ModelTester t(db);

  // 1. Empty DB scan
  t.CheckScan("a", "z", "scan_empty_db");

  // 2. Single key — scan includes it
  t.Put("m", "middle");
  t.CheckScan("a", "z", "scan_single_key_in_range");

  // 3. Single key — scan excludes it (key < start)
  t.CheckScan("n", "z", "scan_single_key_before");

  // 4. Single key — scan excludes it (key == end, half-open)
  t.CheckScan("a", "m", "scan_single_key_at_end");

  // 5. start == end → empty result
  t.CheckScan("m", "m", "scan_start_eq_end");

  // 6. start > end → empty result (degenerate)
  {
    std::vector<std::pair<std::string, std::string>> res;
    db->Scan(leveldb::ReadOptions(), "z", "a", &res);
    CHECK("scan_start_gt_end", res.empty(), "got " + std::to_string(res.size()));
  }

  // 7. Exact boundary: start == key
  t.CheckScan("m", "n", "scan_start_eq_key");

  // 8. Many keys, scan entire range
  for (int i = 0; i < 50; i++) t.Put(MakeKey(i), MakeVal(i, 0));
  t.CheckScan(MakeKey(0), MakeKey(49), "scan_50_keys");

  // 9. Scan subset
  t.CheckScan(MakeKey(10), MakeKey(20), "scan_subset");

  // 10. Scan single key in populated DB
  t.CheckScan(MakeKey(25), MakeKey(26), "scan_single_in_populated");

  // 11. Overwrite key many times, scan returns latest
  for (int i = 0; i < 100; i++) t.Put("overwrite_key", "v" + std::to_string(i));
  t.CheckScan("overwrite_key", "overwrite_kez", "scan_after_overwrites");
  t.CheckGet("overwrite_key", "get_after_overwrites");

  // 12. Scan after compaction
  db->CompactRange(nullptr, nullptr);
  t.CheckScan(MakeKey(0), MakeKey(49), "scan_after_compact");

  // 13. Full DB scan
  t.CheckFullScan("scan_full_db");

  // 14. Keys with common prefixes
  t.Put("prefix_a", "1");
  t.Put("prefix_aa", "2");
  t.Put("prefix_ab", "3");
  t.Put("prefix_b", "4");
  t.CheckScan("prefix_a", "prefix_b", "scan_common_prefix");

  // 15. Keys with special characters
  t.Put("key-with-dash", "v1");
  t.Put("key.with.dot", "v2");
  t.Put("key_with_underscore", "v3");
  t.CheckScan("key", "kez", "scan_special_chars");

  // 16. Empty string key
  t.Put("", "empty_key_val");
  t.CheckScan("", "\x01", "scan_empty_string_key");

  // 17. Long key and value
  std::string long_key(200, 'x');
  std::string long_val(10000, 'y');
  t.Put(long_key, long_val);
  std::string long_key_end = long_key + "\xff";
  t.CheckScan(long_key, long_key_end, "scan_long_kv");
  t.CheckGet(long_key, "get_long_kv");

  std::cout << "  Scan edge cases done.\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// DELETERANGE EDGE CASES
// ═══════════════════════════════════════════════════════════════════════════════

void TestDeleteRangeEdgeCases(leveldb::DB* db) {
  std::cout << "\n=== DeleteRange Edge Cases ===\n";
  ModelTester t(db);

  // 1. DeleteRange on empty DB (no-op, should not crash)
  t.DeleteRange("a", "z");
  t.CheckScan("a", "z", "dr_empty_db");

  // 2. Insert keys, delete all
  for (int i = 0; i < 20; i++) t.Put(MakeKey(i), MakeVal(i, 0));
  t.DeleteRange(MakeKey(0), MakeKey(20));
  t.CheckScan(MakeKey(0), MakeKey(20), "dr_delete_all");
  for (int i = 0; i < 20; i++) t.CheckGet(MakeKey(i), "dr_delete_all_get");

  // 3. DeleteRange with start == end (no-op)
  t.Put("keep_me", "safe");
  t.DeleteRange("keep_me", "keep_me");
  t.CheckGet("keep_me", "dr_start_eq_end");

  // 4. DeleteRange with start > end (no-op)
  t.DeleteRange("z", "a");
  t.CheckGet("keep_me", "dr_start_gt_end");

  // 5. Delete then re-put
  t.Put("phoenix", "alive");
  t.Delete("phoenix");
  t.Put("phoenix", "reborn");
  t.CheckGet("phoenix", "dr_delete_reput");

  // 6. DeleteRange then re-put inside range
  for (int i = 0; i < 10; i++) t.Put("r" + std::to_string(i), "val");
  t.DeleteRange("r3", "r7");
  t.Put("r5", "restored");
  t.CheckScan("r0", "r9", "dr_reput_inside_range");
  t.CheckGet("r5", "dr_reput_r5");
  t.CheckGet("r3", "dr_deleted_r3");  // should be missing
  t.CheckGet("r6", "dr_deleted_r6");  // should be missing

  // 7. Adjacent DeleteRanges
  for (int i = 0; i < 30; i++) t.Put(MakeKey(i), MakeVal(i, 1));
  t.DeleteRange(MakeKey(0), MakeKey(10));
  t.DeleteRange(MakeKey(10), MakeKey(20));
  t.DeleteRange(MakeKey(20), MakeKey(30));
  t.CheckScan(MakeKey(0), MakeKey(30), "dr_adjacent_ranges");

  // 8. Overlapping DeleteRanges
  for (int i = 0; i < 20; i++) t.Put(MakeKey(i), MakeVal(i, 2));
  t.DeleteRange(MakeKey(0), MakeKey(15));
  t.DeleteRange(MakeKey(10), MakeKey(20));  // overlaps [10,15)
  t.CheckScan(MakeKey(0), MakeKey(20), "dr_overlapping_ranges");

  // 9. DeleteRange boundary: key at start included, key at end excluded
  t.Put("boundary_a", "v1");
  t.Put("boundary_b", "v2");
  t.Put("boundary_c", "v3");
  t.DeleteRange("boundary_a", "boundary_c");
  t.CheckGet("boundary_a", "dr_boundary_start");  // should be deleted
  t.CheckGet("boundary_b", "dr_boundary_mid");    // should be deleted
  t.CheckGet("boundary_c", "dr_boundary_end");    // should survive

  // 10. DeleteRange of non-existent keys (should not error)
  t.DeleteRange("zzz_no_exist_start", "zzz_no_exist_end");
  CHECK("dr_nonexistent", true, "");

  // 11. DeleteRange after compaction
  for (int i = 0; i < 10; i++) t.Put(MakeKey(i + 50), MakeVal(i, 3));
  db->CompactRange(nullptr, nullptr);
  t.DeleteRange(MakeKey(50), MakeKey(55));
  t.CheckScan(MakeKey(50), MakeKey(60), "dr_after_compact");

  // 12. DeleteRange then compact then scan
  db->CompactRange(nullptr, nullptr);
  t.CheckScan(MakeKey(50), MakeKey(60), "dr_compact_then_scan");

  // 13. Multiple deletes of same key
  t.Put("multi_del", "exists");
  t.Delete("multi_del");
  t.Delete("multi_del");  // double delete
  t.CheckGet("multi_del", "dr_double_delete");

  std::cout << "  DeleteRange edge cases done.\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// FORCEFULLCOMPACTION EDGE CASES
// ═══════════════════════════════════════════════════════════════════════════════

void TestForceFullCompaction(leveldb::DB* db) {
  std::cout << "\n=== ForceFullCompaction Edge Cases ===\n";
  ModelTester t(db);

  // 1. Compact empty DB
  leveldb::Status s = db->ForceFullCompaction();
  CHECK("ffc_empty_db", s.ok(), s.ToString());

  // 2. Insert data, compact, verify
  for (int i = 0; i < 50; i++) t.Put(MakeKey(i), MakeVal(i, 0));
  s = db->ForceFullCompaction();
  CHECK("ffc_after_inserts", s.ok(), s.ToString());
  t.CheckFullScan("ffc_scan_after");
  for (int i = 0; i < 50; i++) t.CheckGet(MakeKey(i), "ffc_get_after");

  // 3. Delete some keys, compact, verify tombstones are cleaned
  for (int i = 0; i < 25; i++) t.Delete(MakeKey(i));
  s = db->ForceFullCompaction();
  CHECK("ffc_after_deletes", s.ok(), s.ToString());
  for (int i = 0; i < 50; i++) t.CheckGet(MakeKey(i), "ffc_verify_deletes");
  t.CheckFullScan("ffc_scan_after_deletes");

  // 4. DeleteRange then compact
  for (int i = 0; i < 30; i++) t.Put(MakeKey(i + 60), MakeVal(i, 1));
  t.DeleteRange(MakeKey(60), MakeKey(75));
  s = db->ForceFullCompaction();
  CHECK("ffc_after_deleterange", s.ok(), s.ToString());
  t.CheckScan(MakeKey(60), MakeKey(90), "ffc_scan_dr");
  for (int i = 60; i < 90; i++) t.CheckGet(MakeKey(i), "ffc_get_dr");

  // 5. Double compaction (idempotent)
  s = db->ForceFullCompaction();
  CHECK("ffc_double_compact", s.ok(), s.ToString());
  t.CheckFullScan("ffc_double_scan");

  // 6. Large data: many writes to create multi-level structure
  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < KEY_SPACE; i++) {
      t.Put(MakeKey(i), MakeVal(i, 100 + round));
    }
  }
  s = db->ForceFullCompaction();
  CHECK("ffc_large_data", s.ok(), s.ToString());
  t.CheckFullScan("ffc_large_scan");

  // 7. Writes after compaction still work
  t.Put("post_compact_key", "post_compact_val");
  t.CheckGet("post_compact_key", "ffc_post_write");

  std::cout << "  ForceFullCompaction edge cases done.\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INTERLEAVED OPERATIONS TEST
// ═══════════════════════════════════════════════════════════════════════════════

void TestInterleavedOperations(leveldb::DB* db) {
  std::cout << "\n=== Interleaved Operations ===\n";
  ModelTester t(db);

  // Mix of Put, Delete, DeleteRange, Scan, Compact in rapid succession
  for (int i = 0; i < 50; i++) t.Put(MakeKey(i), MakeVal(i, 0));

  // Scan, then delete part, then scan again
  t.CheckScan(MakeKey(0), MakeKey(50), "interleave_1");
  t.DeleteRange(MakeKey(10), MakeKey(20));
  t.CheckScan(MakeKey(0), MakeKey(50), "interleave_2");

  // Compact mid-stream
  db->CompactRange(nullptr, nullptr);
  t.CheckScan(MakeKey(0), MakeKey(50), "interleave_3_post_compact");

  // Re-insert into deleted range
  for (int i = 10; i < 20; i++) t.Put(MakeKey(i), MakeVal(i, 99));
  t.CheckScan(MakeKey(0), MakeKey(50), "interleave_4_reinsert");

  // ForceFullCompaction then verify everything
  db->ForceFullCompaction();
  t.CheckFullScan("interleave_5_ffc");

  // Delete everything via range delete
  t.DeleteRange(MakeKey(0), MakeKey(99));
  t.CheckScan(MakeKey(0), MakeKey(99), "interleave_6_delete_all");

  // Verify empty after full delete
  for (int i = 0; i < 50; i++) t.CheckGet(MakeKey(i), "interleave_7_empty");

  // Compact the empty state
  db->ForceFullCompaction();
  t.CheckFullScan("interleave_8_empty_ffc");

  std::cout << "  Interleaved operations done.\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// RANDOM STRESS TEST
// ═══════════════════════════════════════════════════════════════════════════════

void RunStressTest(leveldb::DB* db) {
  std::cout << "\n=== Stress Test: " << NUM_ROUNDS << " random rounds ===\n";
  ModelTester tester(db);
  srand(42);

  for (int round = 0; round < NUM_ROUNDS; round++) {
    int op = rand() % 5;

    if (op == 0) {
      int k = rand() % KEY_SPACE;
      tester.Put(MakeKey(k), MakeVal(k, round));
    } else if (op == 1) {
      int k = rand() % KEY_SPACE;
      tester.Delete(MakeKey(k));
    } else if (op == 2) {
      int a = rand() % (KEY_SPACE - 1);
      int b = a + 1 + rand() % 10;
      if (b >= KEY_SPACE) b = KEY_SPACE - 1;
      if (a < b) tester.DeleteRange(MakeKey(a), MakeKey(b));
    } else if (op == 3) {
      int a = rand() % (KEY_SPACE - 1);
      int b = a + 1 + rand() % 20;
      if (b >= KEY_SPACE) b = KEY_SPACE - 1;
      if (a < b) {
        tester.CheckScan(MakeKey(a), MakeKey(b),
                         "stress_r" + std::to_string(round));
      }
    } else {
      // Occasional ForceFullCompaction during stress
      if (round % 100 == 50) {
        db->ForceFullCompaction();
        tester.CheckFullScan("stress_ffc_r" + std::to_string(round));
      }
    }

    if (round % FLUSH_EVERY == 0) {
      db->CompactRange(nullptr, nullptr);
    }

    if (round % 75 == 0) {
      for (int k = 0; k < KEY_SPACE; k++) {
        tester.CheckGet(MakeKey(k), "stress_get_r" + std::to_string(round));
      }
    }
  }

  tester.CheckFullScan("stress_final_scan");
  for (int k = 0; k < KEY_SPACE; k++) {
    tester.CheckGet(MakeKey(k), "stress_final_get");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
  // Each test gets a fresh DB
  {
    auto* db = OpenFreshDB("/tmp/test_scan_edge");
    TestScanEdgeCases(db);
    delete db;
  }
  {
    auto* db = OpenFreshDB("/tmp/test_dr_edge");
    TestDeleteRangeEdgeCases(db);
    delete db;
  }
  {
    auto* db = OpenFreshDB("/tmp/test_ffc_edge");
    TestForceFullCompaction(db);
    delete db;
  }
  {
    auto* db = OpenFreshDB("/tmp/test_interleaved");
    TestInterleavedOperations(db);
    delete db;
  }
  {
    auto* db = OpenFreshDB("/tmp/test_stress");
    RunStressTest(db);
    db->ForceFullCompaction();
    delete db;
  }

  // Cleanup
  system("rm -rf /tmp/test_scan_edge /tmp/test_dr_edge /tmp/test_ffc_edge /tmp/test_interleaved /tmp/test_stress");

  std::cout << "\n══════════════════════════════\n";
  std::cout << "Total checks : " << total_tests  << "\n";
  std::cout << "Passed       : " << total_passed << "\n";
  std::cout << "Failed       : " << total_failed << "\n";
  if (total_failed == 0)
    std::cout << "ALL PASSED ✓\n";
  else
    std::cout << "FAILURES: " << total_failed << " — see above\n";

  return total_failed == 0 ? 0 : 1;
}