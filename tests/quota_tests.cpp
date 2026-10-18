// 解析逻辑的单元测试。无测试框架依赖：失败时打印位置并返回非零退出码。

#include <cmath>
#include <cstdio>
#include <string>

#include "json.h"
#include "quota.h"

using namespace cpa;

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s(%d): CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-6)

static void TestJson()
{
    auto v = json::Parse(R"({"a":[1,2.5,-3e2],"s":"x\u00e9\ud83d\ude00\n","t":true,"n":null})");
    CHECK(v.has_value());
    CHECK(v->Find("a")->Items().size() == 3);
    CHECK_NEAR(*v->Find("a")->Items()[2].AsNumber(), -300.0);
    CHECK(v->Find("s")->AsString() == std::string("x\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK(v->Find("t")->AsBool() == true);
    CHECK(v->Find("n")->IsNull());
    CHECK(v->FindAny({"n", "t"}) == v->Find("t"));

    CHECK(!json::Parse("{\"a\":1,}").has_value());
    CHECK(!json::Parse("[1 2]").has_value());
    CHECK(!json::Parse("\"\\ud800\"").has_value());
    CHECK(json::Quote("a\"b\\\n") == "\"a\\\"b\\\\\\n\"");
}

static void TestIso8601()
{
    CHECK(ParseIso8601("1970-01-01T00:00:00Z") == 0);
    CHECK(ParseIso8601("2026-10-18T12:00:00Z") == 1792324800);
    CHECK(ParseIso8601("2026-10-18T20:00:00.123456789+08:00") == 1792324800);
    CHECK(ParseIso8601("2026-10-18T07:00:00-05:00") == 1792324800);
    CHECK(!ParseIso8601("not a date").has_value());
    CHECK(NormalizeUnix(1792324800.0) == 1792324800);
    CHECK(NormalizeUnix(1792324800000.0) == 1792324800);
}

static void TestDuration()
{
    CHECK(FormatDuration(0) == L"0m");
    CHECK(FormatDuration(59) == L"1m");
    CHECK(FormatDuration(2 * 3600 + 13 * 60) == L"2h13m");
    CHECK(FormatDuration(3 * 86400 + 4 * 3600 + 30) == L"3d4h");
}

static void TestCredentials()
{
    const char* body = R"({"observed_at":"2026-10-18T00:00:00Z","files":[
        {"auth_index":"b2","name":"codex-b.json","provider":"codex","email":"b@x.com","disabled":false,
         "id_token":{"chatgpt_account_id":"acc-123","plan_type":"plus"}},
        {"auth_index":"a1","name":"claude-a.json","type":"claude","email":"a@x.com"},
        {"auth_index":"g1","name":"gem.json","provider":"gemini"},
        {"auth_index":"c3","name":"codex-c.json","provider":"Codex","label":"team","disabled":true},
        {"name":"no-index.json","provider":"codex"}
    ]})";
    std::string err;
    auto accounts = ParseCredentials(body, &err);
    CHECK(accounts.has_value());
    CHECK(accounts->size() == 3);
    // 排序：Codex 在前，按显示名
    CHECK((*accounts)[0].provider == Provider::Codex);
    CHECK((*accounts)[0].auth_index == "b2");
    CHECK((*accounts)[0].chatgpt_account_id == "acc-123");
    CHECK((*accounts)[0].plan == "plus");
    CHECK((*accounts)[1].DisplayName() == "team");
    CHECK((*accounts)[1].disabled);
    CHECK((*accounts)[2].provider == Provider::Claude);

    CHECK(!ParseCredentials(R"({"error":"x"})", &err).has_value());
}

static void TestApiCall()
{
    std::string req = BuildApiCallBody("a1", "GET", "https://x/y", {{"Authorization", "Bearer $TOKEN$"}});
    CHECK(req == R"({"auth_index":"a1","method":"GET","url":"https://x/y","header":{"Authorization":"Bearer $TOKEN$"}})");

    std::string err;
    auto r = ParseApiCallResponse(R"({"status_code":200,"header":{},"body":"{\"a\":1}"})", &err);
    CHECK(r.has_value());
    CHECK(r->status_code == 200);
    CHECK(r->body == R"({"a":1})");

    CHECK(ExtractErrorMessage(R"({"error":{"message":"bad token"}})") == "bad token");
    CHECK(ExtractErrorMessage(R"({"error":"invalid management key"})") == "invalid management key");
    CHECK(ExtractErrorMessage("plain text") == "plain text");
}

static void TestCodexUsage()
{
    const int64_t now = 1792324800;
    std::string err;

    // 新格式：按窗口时长归类（顺序颠倒也能识别）
    AccountQuota q;
    CHECK(ParseCodexUsage(R"({"plan_type":"pro","rate_limit":{"allowed":true,"limit_reached":false,
        "primary_window":{"used_percent":40,"limit_window_seconds":604800,"reset_at":1792500000},
        "secondary_window":{"used_percent":15.5,"limit_window_seconds":18000,"reset_after_seconds":3600}}})",
                          now, q, &err));
    CHECK(q.plan == "pro");
    CHECK_NEAR(q.five_hour.RemainingPercent(), 84.5);
    CHECK(q.five_hour.reset_at == now + 3600);
    CHECK_NEAR(q.weekly.RemainingPercent(), 60.0);
    CHECK(q.weekly.reset_at == 1792500000);
    CHECK(!q.weekly.monthly);

    // 旧格式：无时长时按 primary/secondary 顺序；月度窗口
    AccountQuota legacy;
    CHECK(ParseCodexUsage(R"({"rate_limit":{"primary_window":{"used_percent":10},
        "secondary_window":{"used_percent":20,"limit_window_seconds":2592000}}})", now, legacy, &err));
    CHECK_NEAR(legacy.five_hour.RemainingPercent(), 90.0);
    CHECK(legacy.weekly.monthly);

    // 达到上限且未给出百分比 → 视为用尽
    AccountQuota limited;
    CHECK(ParseCodexUsage(R"({"rate_limit":{"limit_reached":true,
        "primary_window":{"limit_window_seconds":18000,"reset_after_seconds":60}}})", now, limited, &err));
    CHECK_NEAR(limited.five_hour.RemainingPercent(), 0.0);
    CHECK(!limited.weekly.present);
    CHECK(limited.weekly.RemainingPercent() < 0);

    AccountQuota bad;
    CHECK(!ParseCodexUsage(R"({"detail":"Unauthorized"})", now, bad, &err));
}

static void TestClaudeUsage()
{
    std::string err;
    AccountQuota q;
    CHECK(ParseClaudeUsage(R"({"five_hour":{"utilization":12.0,"resets_at":"2026-10-18T13:00:00.5+00:00"},
        "seven_day":{"utilization":130,"resets_at":null},"seven_day_opus":null})", q, &err));
    CHECK_NEAR(q.five_hour.RemainingPercent(), 88.0);
    CHECK(q.five_hour.reset_at == 1792324800 + 3600);
    CHECK_NEAR(q.weekly.RemainingPercent(), 0.0);  // 超额时钳制为 0
    CHECK(q.weekly.reset_at == 0);

    AccountQuota partial;
    CHECK(ParseClaudeUsage(R"({"five_hour":{"utilization":null},"seven_day":{"utilization":"50"}})", partial, &err));
    CHECK(!partial.five_hour.present);
    CHECK_NEAR(partial.weekly.RemainingPercent(), 50.0);

    AccountQuota none;
    CHECK(!ParseClaudeUsage(R"({"five_hour":null})", none, &err));
}

int main()
{
    TestJson();
    TestIso8601();
    TestDuration();
    TestCredentials();
    TestApiCall();
    TestCodexUsage();
    TestClaudeUsage();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
