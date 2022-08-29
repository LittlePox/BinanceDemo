#pragma once

#include <string>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <chrono>
#include "curl/curl.h"
#include "algo_hmac.h"

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::system_clock;

enum HTTP_METHOD {
    GET,
    POST,
    PUT,
    DELETE
};

struct RequestResources {
    CURL *req;
    CURLM *curlm;
    curl_slist *list;
    uint32_t oid;
};

inline size_t no_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    // don't process anything; just assume return is OK.
    // std::cout << std::string(ptr, nmemb) << std::endl;
    return size * nmemb;
}

inline void clear_request(RequestResources &r) {
    curl_multi_remove_handle(r.curlm, r.req);
    curl_easy_cleanup(r.req);
    curl_slist_free_all(r.list);
}

inline std::string GetSign(const std::string &key, const std::string &data) {
    unsigned char *mac = nullptr;
    uint32_t mac_length = 0;
    int ret = HmacEncode("sha256", key.c_str(), key.length(), data.c_str(), data.length(), mac, mac_length);

    if (ret != 0) {
        printf("Get sign failed for data:%s", data.c_str());
        free(mac);
        return "";
    }
    std::string sign(mac_length * 2, 0);
    static const char hex_digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < mac_length; i++) {
        sign[i * 2] = hex_digits[mac[i] >> 4];
        sign[i * 2 + 1] = hex_digits[mac[i] & 15];
    }
    free(mac);
    return sign;
}

inline uint64_t GetTimestamp() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}