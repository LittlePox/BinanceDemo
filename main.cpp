#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <atomic>

#include "curl_utils.h"
#include "thread_pool.h"
#include "ws_client.h"

using namespace std;

mutex http_m, curlm_m;
condition_variable http_cv, tick_updated;
int still_running = 0;
CURLM *curlm;
bool shutdown_ready = false;
atomic<uint32_t> oid {1};
map<string, string> header;
client ws_c;
client::connection_ptr ws_con;
atomic<double> mid_price {0};

constexpr auto *key = "c87633a373a0d27c0c56b2e09371fd7c77eb6671a198c646c41ef2539ea26a5a";
constexpr auto *secret = "24aefb8ff858758f820b50473199025ef2d1ea63e908dc7d5145a0754c6d5a78";
constexpr auto *base_url = "https://testnet.binancefuture.com/fapi/";
constexpr auto *ws_uri = "wss://stream.binancefuture.com/ws/btcusdt@bookTicker";
constexpr auto *fmt = R"({"e":"bookTicker","u":%*d,"s":"BTCUSDT","b":"%lf","B":"%*f","a":"%lf"%*s)";

void http_worker() {
    while (!shutdown_ready) {
        unique_lock<mutex> lk(http_m);
        http_cv.wait(lk, []{
            return still_running > 0 || shutdown_ready;
        });
        if (shutdown_ready) {
            return;
        }
        while (still_running) {
            curl_multi_perform(curlm, &still_running);
            int numfds;
            curl_multi_poll(curlm, NULL, 0, 100, &numfds);
        }
        lk.unlock();
        http_cv.notify_one();
    }
}

RequestResources request(HTTP_METHOD method, const string&url, const map<string, string> &header, const string &body) {
    CURL *curl = curl_easy_init();
    switch (method) {
        case GET:
            break;
        case POST:
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            break;
        case PUT:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        case DELETE:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        default:
            throw std::runtime_error("Unknown request method: " + std::to_string(method));
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, no_write_callback);
    curl_slist *list = NULL;
    for (auto &&i : header) {
        list = curl_slist_append(list, (i.first + ": " + i.second).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    {
        // not thread-safe so a lock is required.
        unique_lock<mutex> l(curlm_m);
        curl_multi_add_handle(curlm, curl);
        still_running += 1;
    }
    return {curl, curlm, list};
}

RequestResources SendOrder(const char *side, double price) {
    char buf[1024], param[1024];
    uint32_t id = oid.fetch_add(1);
    size_t len = snprintf(param, 1024, "recvWindow=10000&symbol=BTCUSDT&newClientOrderId=%d&side=%s&positionSide=BOTH&type=LIMIT&timeInForce=GTC&quantity=0.01&price=%.3lf&timestamp=%zu",
        id, side, price, GetTimestamp());
    param[len] = 0;
    string sign = GetSign(secret, param);
    len = snprintf(buf, 1024, "%sv1/order?%s&signature=%s", base_url, param, sign.c_str());
    buf[len] = 0;
    auto t = request(POST, buf, header, "");
    t.oid = id;
    return t;
}

RequestResources CancelOrder(uint32_t oid) {
    char buf[1024], param[1024];
    size_t len = snprintf(param, 1024, "recvWindow=10000&symbol=BTCUSDT&origClientOrderId=%u&timestamp=%zu",
        oid, GetTimestamp());
    param[len] = 0;
    string sign = GetSign(secret, param);
    len = snprintf(buf, 1024, "%sv1/order?%s&signature=%s", base_url, param, sign.c_str());
    buf[len] = 0;
    return request(DELETE, buf, header, "");
}

void on_tick(const string &c) {
    double bid, ask;
    sscanf(c.data(), fmt, &bid, &ask);
    mid_price = (bid + ask) / 2;
    tick_updated.notify_one();
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    header["X-MBX-APIKEY"] = key;
    thread http_thread(http_worker);
    thread ws_thread(websocket_worker, ws_uri, std::ref(ws_c), std::ref(ws_con), &on_tick);
    curlm = curl_multi_init();
    if (!curlm) return -1;

    ThreadPool tp{};
    array<RequestResources, 100> reqs;
    size_t idx;

    for (auto k = 0; k < 10; k++) {
        // waiting for a new_tick
        {
            unique_lock<mutex> lk(http_m);
            tick_updated.wait(lk);
        }
        double mp = mid_price.load();

        // sending 100 requests;
        printf("Ready to send 100 orders around price: %.4lf\n", mp);

        vector<future<RequestResources>> request_tasks;
        request_tasks.reserve(100);
        for (auto i = 0; i < 50; i++) {
            request_tasks.emplace_back(tp.enqueue([mp] {
                return SendOrder("BUY", mp * 0.85);
            }));
            request_tasks.emplace_back(tp.enqueue([mp] {
                return SendOrder("SELL", mp * 1.15);
            }));
        }
        idx = 0;
        for (auto &&t : request_tasks) {
            reqs[idx++] = t.get();
        }
        http_cv.notify_one();

        // wait for all sent;
        {
            unique_lock<mutex> lk(http_m);
            http_cv.wait(lk, []{return still_running == 0;});
            cout << "Orders sent." << endl;
        }

        // clear all requests, this is very fast so multi-threading is not needed.
        for (auto i = 0; i < idx; i++) {
            clear_request(reqs[i]);
        }
       
        // wait for 10 seconds;
        this_thread::sleep_for(chrono::seconds(10));

        //canceling 100 orders;
        cout << "Ready to cancel 100 orders." << endl;
        request_tasks.clear();
        for (auto i = 0; i < idx; i++) {
            auto oid = reqs[i].oid;
            request_tasks.emplace_back(tp.enqueue([oid] {
                return CancelOrder(oid);
            }));
        }
        idx = 0;
        for (auto &&t : request_tasks) {
            reqs[idx++] = t.get();
        }
        http_cv.notify_one();
        // wait for all sent;
        {
            unique_lock<mutex> lk(http_m);
            http_cv.wait(lk, []{return still_running == 0;});
        }
        cout << "Orders canceled." << endl;
        for (auto i = 0; i < idx; i++) {
            clear_request(reqs[i]);
        }
    }

    cout << endl << "Done." << endl;

    // close http connections
    curl_multi_cleanup(curlm); 
    shutdown_ready = true;
    http_cv.notify_one();
    http_thread.join();

    // close websocket connections.
    ws_c.pause_reading(ws_con);
    ws_c.stop_perpetual();
    websocketpp::lib::error_code ec;
    ws_c.close(ws_con, websocketpp::close::status::going_away, "bye", ec);
    ws_thread.join();

    return 0;
}