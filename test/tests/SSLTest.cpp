#include <libstuff/libstuff.h>
#include <sqlitecluster/SQLiteNode.h>
#include <test/lib/BedrockTester.h>

struct SSLTest : tpunit::TestFixture {
    SSLTest()
        : tpunit::TestFixture("SSL",
                              TEST(SSLTest::testPayPal),
                              TEST(SSLTest::testGoogle),
                              TEST(SSLTest::testFailure)),

            state(SQLiteNode::LEADING),
            https(state)
    { }

    atomic<SQLiteNode::State> state;
    TestHTTPS https;

    // Simplified version of the loop that's used in bedrock to poll for data.
    void _wait(SHTTPSManager::Transaction* t, uint64_t timeout = STIME_US_PER_M) {
        // Wait for the transaction to have a return code.
        fd_map fdm;
        uint64_t stop = STimeNow() + timeout;
        uint64_t nextActivity = STimeNow();
        while ((t ? t->response == 0 : true) && STimeNow() < stop) {
            // Do another select.
            fdm.clear();
            https.prePoll(fdm);
            const uint64_t now = STimeNow();
            S_poll(fdm, max(nextActivity, now) - now);
            nextActivity = STimeNow() + STIME_US_PER_S;
            https.postPoll(fdm, nextActivity);
        }
    }

    /* A response is valid if it has a response code > 0 and less than 999, and has at least some content.
     * We're just looking that an SSL handshake succeeded here, not for any particular content, and we send junk
     * requests on purpose.
     */
    bool verifyFullResponse(const int responseCode, const SData& response) {
        if (responseCode <= 0) {
            return false;
        }
        if (responseCode > 999) {
            return false;
        }
        return !response.empty();
    }

    void testPayPal() {
        SData request;
        request.methodLine = "GET / HTTP/1.1";
        request["Host"] = "svcs.paypal.com";
        request["Connection"] = "Close";
        SHTTPSManager::Transaction* t = https.sendRequest("https://svcs.paypal.com/", request);
        _wait(t);
        ASSERT_TRUE(verifyFullResponse(t->response, t->fullResponse));
    }

    void testGoogle() {
        SData request;
        request.methodLine = "GET / HTTP/1.1";
        request["Host"] = "www.google.com";
        request["Connection"] = "Close";
        SHTTPSManager::Transaction* t = https.sendRequest("https://www.google.com/", request);
        _wait(t);
        ASSERT_TRUE(verifyFullResponse(t->response, t->fullResponse));
    }

    void testFailure() {
        SData request;
        request.methodLine = "GET / HTTP/1.1";
        request["Host"] = "www.notarealplaceforsure.com.fake";
        request["Connection"] = "Close";
        SHTTPSManager::Transaction* t = https.sendRequest("https://www.notarealplaceforsure.com.fake/", request);
        _wait(t);
        ASSERT_FALSE(verifyFullResponse(t->response, t->fullResponse));
    }
} __SSLTest;
