#pragma once

// Bench-only, read-only TLS probes. Never sends credentials or vehicle commands.
#include <WiFiClientSecure.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/ecp.h>

class RelayTlsProbe : public WiFiClientSecure {
public:
    void run(const char* host, uint16_t port, const char* ca, unsigned profile) {
        setCACert(ca);
        setHandshakeTimeout(12);
        setTimeout(5000);
        static const char* alpn[] = {"http/1.1", nullptr};
        setAlpnProtocols(alpn);
        setPlainStart(); // Prepare TLS, but allow configuration before ClientHello.
        Serial.printf("# TLS-PROBE profile=%u begin\n", profile);
        if (!WiFiClientSecure::connect(host, port, 5000)) {
            Serial.println("# TLS-PROBE TCP/setup failed");
            return;
        }
        static const int suites[] = {
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 0
        };
        static const int rsaSuites[] = {
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 0
        };
        // TLS supported_groups wire IDs: secp256r1, secp384r1.
        static const uint16_t curves[] = {23, 24, 0};
        if (profile >= 1) mbedtls_ssl_conf_ciphersuites(&sslclient->ssl_conf, suites);
        if (profile == 3) mbedtls_ssl_conf_ciphersuites(&sslclient->ssl_conf, rsaSuites);
        if (profile >= 2) mbedtls_ssl_conf_groups(&sslclient->ssl_conf, curves);
        const UBaseType_t originalPriority = uxTaskPriorityGet(nullptr);
        if (profile == 4) vTaskPrioritySet(nullptr, 1);
        const uint32_t started = millis();
        int previous = -1;
        int error = 0;
        while (sslclient->ssl_ctx.MBEDTLS_PRIVATE(state) != MBEDTLS_SSL_HANDSHAKE_OVER) {
            const int state = sslclient->ssl_ctx.MBEDTLS_PRIVATE(state);
            if (state != previous) {
                Serial.printf("# TLS-PROBE profile=%u state=%d elapsed=%lu\n", profile, state,
                              static_cast<unsigned long>(millis() - started));
                previous = state;
            }
            error = mbedtls_ssl_handshake_step(&sslclient->ssl_ctx);
            if (error != 0 && error != MBEDTLS_ERR_SSL_WANT_READ && error != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            if (millis() - started > 12000) { error = MBEDTLS_ERR_SSL_TIMEOUT; break; }
            vTaskDelay(2);
        }
        const uint32_t verify = mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx);
        vTaskPrioritySet(nullptr, originalPriority);
        char description[100] = {};
        mbedtls_strerror(error, description, sizeof(description));
        Serial.printf("# TLS-PROBE profile=%u result=%d verify=%lu detail=%s\n", profile, error,
                      static_cast<unsigned long>(verify), description);
        if (error == 0 && verify == 0) {
            _stillinPlainStart = false;
            Serial.printf("# TLS-PROBE protocol=%s cipher=%s\n",
                          mbedtls_ssl_get_version(&sslclient->ssl_ctx),
                          mbedtls_ssl_get_ciphersuite(&sslclient->ssl_ctx));
            printf("GET /health HTTP/1.1\r\nHost: %s\r\nngrok-skip-browser-warning: 1\r\nConnection: close\r\n\r\n", host);
            Serial.printf("# TLS-PROBE HTTP=%s\n", readStringUntil('\n').c_str());
        }
        stop();
    }
};
