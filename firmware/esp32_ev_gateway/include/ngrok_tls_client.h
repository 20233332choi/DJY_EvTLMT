#pragma once

#include <WiFiClientSecure.h>
#include <esp_arduino_version.h>
#include <mbedtls/ssl.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "NgrokTlsClient requires Arduino ESP32 3.x; build platformio-modern.ini"
#endif

// ngrok closed our TLS 1.2 handshakes when the second client flight was delayed
// by about one second. Generation-Y ECDSA chain verification takes ~3 s at idle
// priority on this ESP32-S3. ECDHE-RSA/AES-GCM uses the server's RSA certificate
// chain, retains forward secrecy, and completes within the observed deadline.
// Use the ordinary Arduino verified TLS implementation, with no SDK patches.
class NgrokTlsClient : public WiFiClientSecure {
public:
    using WiFiClientSecure::connect;

    int connect(const char* host, uint16_t port) override {
        return connect(host, port, _timeout);
    }

    int connect(const char* host, uint16_t port, int32_t timeout) override {
        // This client intentionally has no insecure/plaintext fallback.
        if (_use_insecure || _CA_cert == nullptr) return 0;
        _timeout = timeout;
        _stillinPlainStart = true;
        // No application data is sent in this deferred-handshake interval.
        if (!WiFiClientSecure::connect(host, port, _CA_cert, _cert, _private_key)) return 0;
        static const int suites[] = {
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 0
        };
        static const uint16_t groups[] = {
            MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
            MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1, 0
        };
        mbedtls_ssl_conf_ciphersuites(&sslclient->ssl_conf, suites);
        mbedtls_ssl_conf_groups(&sslclient->ssl_conf, groups);
        // Let this short computation run without time-sharing with the idle
        // task. Network/driver tasks still preempt it; restore priority on exit.
        const UBaseType_t originalPriority = uxTaskPriorityGet(nullptr);
        if (originalPriority == 0) vTaskPrioritySet(nullptr, 1);
        const int result = ssl_starttls_handshake(sslclient.get());
        vTaskPrioritySet(nullptr, originalPriority);
        sslclient->last_error = result;
        if (result < 0) {
            stop();
            return 0;
        }
        _stillinPlainStart = false;
        return 1;
    }
};
