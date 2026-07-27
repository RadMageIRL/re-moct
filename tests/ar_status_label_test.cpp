// ar_status_label_test - CD-S4: every AccurateRip status has its own words.
//
// Seven readers consume ARStatus, and the ones that were wrong were wrong for
// one shared reason: a fall-through absorbed a value nobody listed, so "we never
// asked" came out as "no match" - and, in CUETools mode, as a permanent
// ACCURATERIP=AR: network error tag in the user's files.
//
// So the property worth asserting is not any particular wording. It is that
// EVERY status maps to its own non-empty label, and that NONE of them lands on
// the not-a-real-label sentinel. Delete a case from either switch and this test
// fails, which is what stops an eighth reader repeating the defect.
//
// Pure: links nothing.
#include "ArStatus.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do{ ++g_checks; if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("  [%s]\n", #c);} }while(0)

// Every enumerator, listed by hand ON PURPOSE: if someone adds a status and does
// not add it here, the count check below fails and points at this line.
static const ARStatus kAll[] = {
    ARStatus::NotQueried, ARStatus::Matched_v2, ARStatus::Matched_v1,
    ARStatus::NotFound,   ARStatus::NetworkError, ARStatus::ReadError,
};
static const int kAllCount = (int)(sizeof kAll / sizeof kAll[0]);

static void test_every_status_has_its_own_words() {
    for (int form = 0; form < 2; ++form) {
        const char* which = form == 0 ? "arStatusLabel" : "arStatusPassLabel";
        std::vector<std::string> seen;
        for (int i = 0; i < kAllCount; ++i) {
            const char* s = form == 0 ? arStatusLabel(kAll[i])
                                      : arStatusPassLabel(kAll[i]);
            CHECK(s && *s, "%s(%d) returned an empty label", which, i);
            // The sentinel means the switch did not handle this value.
            CHECK(std::strcmp(s, "AR ???") != 0 && std::strcmp(s, "???") != 0,
                  "%s(%d) fell through to the sentinel - the switch is not exhaustive",
                  which, i);
            for (const std::string& prev : seen)
                CHECK(prev != s, "%s: two statuses share the label \"%s\"", which, s);
            seen.push_back(s);
        }
        CHECK((int)seen.size() == kAllCount, "%s: expected %d labels, got %zu",
              which, kAllCount, seen.size());
    }
}

// The specific one this slice is about. NotQueried is not a failure and must not
// be worded as one: a rip that never asked AccurateRip must not say "no match",
// "not found" or "network error".
static void test_not_queried_is_not_a_failure() {
    const std::string shortf = arStatusLabel(ARStatus::NotQueried);
    const std::string longf  = arStatusPassLabel(ARStatus::NotQueried);

    CHECK(shortf.find("queried") != std::string::npos,
          "short label for NotQueried should say it was not queried, got \"%s\"",
          shortf.c_str());
    CHECK(longf.find("queried") != std::string::npos,
          "long label for NotQueried should say it was not queried, got \"%s\"",
          longf.c_str());

    CHECK(shortf != arStatusLabel(ARStatus::NotFound),
          "NotQueried must not read as NotFound");
    CHECK(shortf != arStatusLabel(ARStatus::NetworkError),
          "NotQueried must not read as NetworkError - the exact defect CD-S4 fixes");
    CHECK(longf != arStatusPassLabel(ARStatus::NotFound),
          "NotQueried must not read as \"no match\" on the Pass line");
    CHECK(longf != arStatusPassLabel(ARStatus::NetworkError),
          "NotQueried must not read as a network error on the Pass line");
}

// NetworkError and NotFound are different facts and had been sharing "no match"
// on the Pass line. Asked-and-missed is not could-not-ask.
static void test_network_error_is_not_not_found() {
    CHECK(std::strcmp(arStatusPassLabel(ARStatus::NetworkError),
                      arStatusPassLabel(ARStatus::NotFound)) != 0,
          "NetworkError and NotFound must not share a Pass-line label");
    CHECK(std::strcmp(arStatusLabel(ARStatus::NetworkError),
                      arStatusLabel(ARStatus::NotFound)) != 0,
          "NetworkError and NotFound must not share a summary label");
}

int main() {
    test_every_status_has_its_own_words();
    test_not_queried_is_not_a_failure();
    test_network_error_is_not_not_found();
    std::printf("ar_status_label_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
