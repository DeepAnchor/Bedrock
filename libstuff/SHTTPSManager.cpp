#include "libstuff.h"

SHTTPSManager::SHTTPSManager() :
    // Generate an x509 certificate.
    _x509(SX509Open())
{
    SASSERT(_x509);
}

SHTTPSManager::SHTTPSManager(const string& pem, const string& srvCrt, const string& caCrt) {
    // Generate an x509 certificate.
    _x509 = SX509Open(pem, srvCrt, caCrt);
    SASSERT(_x509);
}

SHTTPSManager::~SHTTPSManager() {
    SAUTOLOCK(_listMutex);

    // Clean up outstanding transactions
    SASSERTWARN(_activeTransactionList.empty());
    while (!_activeTransactionList.empty()) {
        closeTransaction(_activeTransactionList.front());
    }
    SASSERTWARN(_completedTransactionList.empty());
    while (!_completedTransactionList.empty()) {
        closeTransaction(_completedTransactionList.front());
    }

    // Clean up certificate.
    SX509Close(_x509);
}

void SHTTPSManager::closeTransaction(Transaction* transaction) {
    if (transaction == nullptr) {
        return;
    }
    SAUTOLOCK(_listMutex);

    // Clean up the socket and done
    _activeTransactionList.remove(transaction);
    _completedTransactionList.remove(transaction);
    if (transaction->s) {
        closeSocket(transaction->s);
    }
    transaction->s = nullptr;
    delete transaction;
}

SHTTPSManager::Socket* SHTTPSManager::openSocket(const string& host, SX509* x509) {
    // Just call the base class function but in a thread-safe way.
    SAUTOLOCK(_listMutex);
    return STCPManager::openSocket(host, x509);
}

void SHTTPSManager::closeSocket(Socket* socket) {
    // Just call the base class function but in a thread-safe way.
    SAUTOLOCK(_listMutex);
    STCPManager::closeSocket(socket);
}

void SHTTPSManager::prePoll(fd_map& fdm) {
    // Just call the base class function but in a thread-safe way.
    SAUTOLOCK(_listMutex);
    return STCPManager::prePoll(fdm);
}

void SHTTPSManager::postPoll(fd_map& fdm, uint64_t& nextActivity) {
    SAUTOLOCK(_listMutex);

    // Let the base class do its thing
    STCPManager::postPoll(fdm);

    // Update each of the active requests
    uint64_t now = STimeNow();
    list<Transaction*>::iterator nextIt = _activeTransactionList.begin();
    while (nextIt != _activeTransactionList.end()) {
        // Did we get any responses?
        list<Transaction*>::iterator activeIt = nextIt++;
        Transaction* active = *activeIt;
        uint64_t elapsed = now - active->created;
        const uint64_t TIMEOUT = STIME_US_PER_S * 300;
        int size = active->fullResponse.deserialize(active->s->recvBuffer);
        if (size) {
            // Consume how much we read.
            SConsumeFront(active->s->recvBuffer, size);

            // 200OK or any content?
            active->finished = now;
            if (SContains(active->fullResponse.methodLine, " 200 ") || active->fullResponse.content.size()) {
                // Pass the transaction down to the subclass.
                if (_onRecv(active)) {
                    // If true, then the transaction was closed in onRecv.
                    continue;
                }
                SASSERT(active->response);
            } else {
                // Error, failed to authenticate or receive a valid server response.
                SWARN("Message failed: '" << active->fullResponse.methodLine << "'");
                active->response = 500;
            }
        } else if (active->s->state > Socket::CONNECTED || elapsed > TIMEOUT) {
            // Net problem. Did this transaction end in an inconsistent state?
            SWARN("Connection " << (elapsed > TIMEOUT ? "timed out" : "died prematurely") << " after "
                  << elapsed / STIME_US_PER_MS << "ms");
            active->response = active->s->sendBuffer.empty() ? 501 : 500;
            if (active->response == 501) {
                // This is pretty serious. Let us know.
                SHMMM("SHTTPSManager: '" << active->fullRequest.methodLine
                      << "' sent with no response. We don't know if they processed it!");
            }
        } else {
            // Haven't timed out yet, let the caller know how long until we do.
            nextActivity = min(nextActivity, active->created + TIMEOUT);
        }

        // If we're done, remove from the active and add to completed
        if (active->response) {
            // Switch lists
            SINFO("Completed request '" << active->fullRequest.methodLine << "' to '" << active->fullRequest["Host"]
                  << "' with response '" << active->response << "' in '" << elapsed / STIME_US_PER_MS << "'ms");
            _activeTransactionList.erase(activeIt);
            _completedTransactionList.push_back(active);
        }
    }
}

SHTTPSManager::Transaction::Transaction(SHTTPSManager& owner_) :
    s(nullptr),
    created(STimeNow()),
    finished(0),
    response(0),
    owner(owner_)
{ }

SHTTPSManager::Transaction::~Transaction() {
    SASSERT(!s);
}

SHTTPSManager::Transaction* SHTTPSManager::_createErrorTransaction() {
    // Sometimes we have to create transactions without an attempted connect. This could happen if we don't have the
    // host or service id yet.
    SWARN("We had to create an error transaction instead of attempting a real one.");
    Transaction* transaction = new Transaction(*this);
    transaction->response = 503;
    transaction->finished = STimeNow();
    SAUTOLOCK(_listMutex);
    _completedTransactionList.push_front(transaction);
    return transaction;
}

SHTTPSManager::Transaction* SHTTPSManager::_httpsSend(const string& url, const SData& request) {
    // Open a connection, optionally using SSL (if the URL is HTTPS). If that doesn't work, then just return a
    // completed transaction with an error response.
    string host, path;
    if (!SParseURI(url, host, path)) {
        return _createErrorTransaction();
    }
    if (!SContains(host, ":")) {
        host += ":443";
    }
    Socket* s = openSocket(host, SStartsWith(url, "https://") ? _x509 : 0);
    if (!s) {
        return _createErrorTransaction();
    }

    // Wrap in a transaction
    Transaction* transaction = new Transaction(*this);
    transaction->s = s;
    transaction->fullRequest = request;

    // Ship it.
    transaction->s->sendBuffer = request.serialize();

    // Keep track of the transaction.
    SAUTOLOCK(_listMutex);
    _activeTransactionList.push_front(transaction);
    return transaction;
}
