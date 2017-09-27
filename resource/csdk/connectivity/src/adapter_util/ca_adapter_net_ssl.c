/******************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************/

#define _GNU_SOURCE

#include <stddef.h>
#include <stdbool.h>
#include "ca_adapter_net_ssl.h"
#include "cacommon.h"
#include "caipinterface.h"
#include "oic_malloc.h"
#include "ocrandom.h"
#include "byte_array.h"
#include "camutex.h"
#include "timer.h"


// headers required for mbed TLS
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pkcs12.h"
#include "mbedtls/ssl_internal.h"
#include "mbedtls/net.h"
#ifdef __WITH_DTLS__
#include "mbedtls/timing.h"
#include "mbedtls/ssl_cookie.h"
#endif

#if !defined(NDEBUG) || defined(TB_LOG)
#include "mbedtls/debug.h"
#include "mbedtls/version.h"
#endif

#ifdef __unix__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif


/**
 * @def MBED_TLS_VERSION_LEN
 * @brief mbedTLS version string length
 */
#define MBED_TLS_VERSION_LEN (16)
/**
 * @def SEED
 * @brief Seed for initialization RNG
 */
#define SEED "IOTIVITY_RND"
/**
 * @def UUID_PREFIX
 * @brief uuid prefix in certificate subject field
 */
#define UUID_PREFIX "uuid:"
/**
 * @def USERID_PREFIX
 * @brief userid prefix in certificate alternative subject name field
 */
#define USERID_PREFIX "userid:"

/**
 * @def NET_SSL_TAG
 * @brief Logging tag for module name
 */
#define NET_SSL_TAG "OIC_CA_NET_SSL"
/**
 * @def MBED_TLS_TAG
 * @brief Logging tag for mbedTLS library
 */
#define MBED_TLS_TAG "MBED_TLS"
/**
 * @def MMBED_TLS_DEBUG_LEVEL
 * @brief Logging level for mbedTLS library
 */
#define MBED_TLS_DEBUG_LEVEL (4)

/**
 * @def TLS_MSG_BUF_LEN
 * @brief Buffer size for TLS record. A single TLS record may be up to 16384 octets in length
 */

#define TLS_MSG_BUF_LEN (16384)
/**
 * @def PSK_LENGTH
 * @brief PSK keys max length
 */
#define PSK_LENGTH (256/8)
/**
 * @def UUID_LENGTHPSK_LENGTH
 * @brief Identity max length
 */
#define UUID_LENGTH (128/8)
/**
 * @def MASTER_SECRET_LEN
 * @brief TLS master secret length
 */
#define MASTER_SECRET_LEN (48)
/**
 * @def RANDOM_LEN
 * @brief TLS client and server random bytes length
 */
#define RANDOM_LEN (32)
/**
 * @def RANDOM_LEN
 * @brief PSK generated keyblock length
 */
#define KEY_BLOCK_LEN (96)

/**@def SSL_CLOSE_NOTIFY(peer, ret)
 *
 * Notifies of existing \a peer about closing TLS connection.
 *
 * @param[in] peer remote peer
 * @param[in] ret used internaly
 */

/**
 * @var RETRANSMISSION_TIME
 * @brief Maximum timeout value (in seconds) to start DTLS retransmission.
 */
#define RETRANSMISSION_TIME 1

#define SSL_CLOSE_NOTIFY(peer, ret)                                                                \
do                                                                                                 \
{                                                                                                  \
    (ret) = mbedtls_ssl_close_notify(&(peer)->ssl);                                                \
} while (MBEDTLS_ERR_SSL_WANT_WRITE == (ret))

/**@def SSL_RES(peer, status)
 *
 * Sets SSL result for callback.
 *
 * @param[in] peer remote peer
 */
#define SSL_RES(peer, status)                                                                      \
if (g_sslCallback)                                                                                 \
{                                                                                                  \
    CAErrorInfo_t errorInfo;                                                                       \
    errorInfo.result = (status);                                                                   \
    g_sslCallback(&(peer)->sep.endpoint, &errorInfo);                                              \
}
/**@def SSL_CHECK_FAIL(peer, ret, str, mutex, error, msg)
 *
 * Checks handshake result and send alert if needed.
 *
 * @param[in] peer remote peer
 * @param[in] ret error code
 * @param[in] str debug string
 * @param[in] mutex ca mutex
 * @param[in] if code does not equal to -1 returns error code
 * @param[in] msg allert message
 */
#define SSL_CHECK_FAIL(peer, ret, str, mutex, error, msg)                                          \
if (0 != (ret) && MBEDTLS_ERR_SSL_WANT_READ != (int) (ret) &&                                      \
    MBEDTLS_ERR_SSL_WANT_WRITE != (int) (ret) &&                                                   \
    MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED != (int) (ret) &&                                        \
    MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY != (int) (ret))                                              \
{                                                                                                  \
    OIC_LOG_V(ERROR, NET_SSL_TAG, "%s: -0x%x", (str), -(ret));                                     \
    if ((int) MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE != (int) (ret) &&                                \
       (int) MBEDTLS_ERR_SSL_BAD_HS_CLIENT_HELLO != (int) (ret))                                   \
    {                                                                                              \
        mbedtls_ssl_send_alert_message(&(peer)->ssl, MBEDTLS_SSL_ALERT_LEVEL_FATAL, (msg));        \
    }                                                                                              \
    if ((int) MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE == (int) (ret) &&                                \
        ((int) MBEDTLS_SSL_ALERT_MSG_DECRYPTION_FAILED == (peer)->ssl.in_msg[1] ||                 \
         (int) MBEDTLS_SSL_ALERT_MSG_DECRYPT_ERROR == (peer)->ssl.in_msg[1] ||                     \
         (int) MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE == (peer)->ssl.in_msg[1] ||                 \
         (int) MBEDTLS_SSL_ALERT_MSG_BAD_RECORD_MAC == (peer)->ssl.in_msg[1]))                     \
    {                                                                                              \
        SSL_RES((peer), CA_DTLS_AUTHENTICATION_FAILURE);                                           \
    }                                                                                              \
    RemovePeerFromList(&(peer)->sep.endpoint);                                                     \
    if (mutex)                                                                                     \
    {                                                                                              \
        ca_mutex_unlock(g_sslContextMutex);                                                        \
    }                                                                                              \
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);                                             \
    if (-1 != error)                                                                               \
    {                                                                                              \
        return (error);                                                                            \
    }                                                                                              \
}
/** @def CHECK_MBEDTLS_RET(f, ...)
 * A macro that checks \a f function return code
 *
 * If function returns error code it goes to error processing.
 *
 * @param[in] f  Function to call
 */
#define CHECK_MBEDTLS_RET(f, ...) do {                                                             \
int ret = (f)(__VA_ARGS__);                                                                        \
if (0 != ret) {                                                                                    \
    OIC_LOG_V(ERROR, NET_SSL_TAG, "%s returned -0x%04x\n", __func__, -(ret));                      \
    goto exit;                                                                                     \
} } while(0)

typedef enum
{
    ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA,
    ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8,
    ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256,
    ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256,
    ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
    ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    ADAPTER_CIPHER_MAX
} AdapterCipher_t;

typedef enum
{
    ADAPTER_CURVE_SECP256R1,
    ADAPTER_CURVE_MAX
} AdapterCurve_t;

int tlsCipher[ADAPTER_CIPHER_MAX][2] =
{
    {MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA, 0},
    {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8, 0},
    {MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256, 0},
    {MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0},
    {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM, 0},
    {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, 0}
};

static int g_cipherSuitesList[ADAPTER_CIPHER_MAX];

mbedtls_ecp_group_id curve[ADAPTER_CURVE_MAX][2] =
{
    {MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_NONE}
};

static PkiInfo_t g_pkiInfo = {{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}};

typedef struct  {
    int code;
    int alert;
} CrtVerifyAlert_t;

static const CrtVerifyAlert_t crtVerifyAlerts[] = {
    {MBEDTLS_X509_BADCERT_EXPIRED,       MBEDTLS_SSL_ALERT_MSG_CERT_EXPIRED},
    {MBEDTLS_X509_BADCERT_REVOKED,       MBEDTLS_SSL_ALERT_MSG_CERT_REVOKED},
    {MBEDTLS_X509_BADCERT_CN_MISMATCH,   MBEDTLS_SSL_ALERT_MSG_CERT_UNKNOWN},
    {MBEDTLS_X509_BADCERT_NOT_TRUSTED,   MBEDTLS_SSL_ALERT_MSG_UNKNOWN_CA},
    {MBEDTLS_X509_BADCRL_NOT_TRUSTED,    MBEDTLS_SSL_ALERT_MSG_UNKNOWN_CA},
    {MBEDTLS_X509_BADCRL_EXPIRED,        MBEDTLS_SSL_ALERT_MSG_INSUFFICIENT_SECURITY},
    {MBEDTLS_X509_BADCERT_MISSING,       MBEDTLS_SSL_ALERT_MSG_NO_CERT},
    {MBEDTLS_X509_BADCERT_SKIP_VERIFY,   MBEDTLS_SSL_ALERT_MSG_INSUFFICIENT_SECURITY},
    {MBEDTLS_X509_BADCERT_OTHER,         MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR},
    {MBEDTLS_X509_BADCERT_FUTURE,        MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCRL_FUTURE,         MBEDTLS_SSL_ALERT_MSG_INSUFFICIENT_SECURITY},
    {MBEDTLS_X509_BADCERT_KEY_USAGE,     MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCERT_EXT_KEY_USAGE, MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCERT_NS_CERT_TYPE,  MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCERT_BAD_MD,        MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCERT_BAD_PK,        MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCERT_BAD_KEY,       MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCRL_BAD_MD,         MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCRL_BAD_PK,         MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {MBEDTLS_X509_BADCRL_BAD_KEY,        MBEDTLS_SSL_ALERT_MSG_BAD_CERT},
    {0, 0}
};

static int GetAlertCode(uint32_t flags)
{
    const CrtVerifyAlert_t *cur;

    for (cur = crtVerifyAlerts; cur->alert != 0 ; cur++)
    {
        if (flags & cur->code)
        {
            return cur->alert;
        }
    }
    return 0;
}

#if !defined(NDEBUG) || defined(TB_LOG)
/**
 * Pass a message to the OIC logger.
 *
 * @param[in] ctx  opaque context for the callback
 * @param[in] level  debug level
 * @param[in] file  file name
 * @param[in] line  line number
 * @param[in] str  message
 */
static void DebugSsl(void *ctx, int level, const char *file, int line, const char *str)
{
    ((void) level);
    ((void) file);
    ((void) line);
    ((void) ctx);

    OIC_LOG_V(DEBUG, MBED_TLS_TAG, "%s", str);
}
#endif

#if defined(_WIN32)
/*
 * Finds the first occurrence of the byte string s in byte string l.
 */

static void * memmem(const void *l, size_t lLen, const void *s, size_t sLen)
{
    char *cur;
    char *last;
    const char *cl = (const char *)l;
    const char *cs = (const char *)s;

    if (lLen == 0 || sLen == 0)
    {
        return NULL;
    }
    if (lLen < sLen)
    {
        return NULL;
    }
    if (sLen == 1)
    {
        return (void *)memchr(l, (int)*cs, lLen);
    }

    last = (char *)cl + lLen - sLen;

    for (cur = (char *)cl; cur <= last; cur++)
    {
        if (cur[0] == cs[0] && memcmp(cur, cs, sLen) == 0)
        {
            return cur;
        }
    }
    return NULL;
}
#endif
/**
 * structure to holds the information of cache message and address info.
 */
typedef ByteArray_t SslCacheMessage_t;


/**
 * Data structure for holding the send and recv callbacks.
 */
typedef struct TlsCallBacks
{
    CAPacketReceivedCallback recvCallback;  /**< Callback used to send data to upper layer. */
    CAPacketSendCallback sendCallback;      /**< Callback used to send data to socket layer. */
} SslCallbacks_t;

/**
 * Data structure for holding the mbedTLS interface related info.
 */
typedef struct SslContext
{
    u_arraylist_t *peerList;         /**< peer list which holds the mapping between
                                              peer id, it's n/w address and mbedTLS context. */
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rnd;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pkey;

    mbedtls_ssl_config clientTlsConf;
    mbedtls_ssl_config serverTlsConf;
    mbedtls_ssl_config clientDtlsConf;
    mbedtls_ssl_config serverDtlsConf;

    AdapterCipher_t cipher;
    SslCallbacks_t adapterCallbacks[MAX_SUPPORTED_ADAPTERS];
    mbedtls_x509_crl crl;
    bool cipherFlag[2];
    int selectedCipher;

#ifdef __WITH_DTLS__
    int timerId;
#endif

} SslContext_t;

/**
 * @var g_caSslContext
 * @brief global context which holds tls context and cache list information.
 */
static SslContext_t * g_caSslContext = NULL;

/**
 * @var g_getCredentialsCallback
 * @brief callback to get TLS credentials (same as for DTLS)
 */
static CAgetPskCredentialsHandler g_getCredentialsCallback = NULL;
/**
 * @var g_getCerdentilTypesCallback
 * @brief callback to get different credential types from SRM
 */
static CAgetCredentialTypesHandler g_getCredentialTypesCallback = NULL;
/**
 * @var g_getPkixInfoCallback
 *
 * @brief callback to get X.509-based Public Key Infrastructure
 */
static CAgetPkixInfoHandler g_getPkixInfoCallback = NULL;

/**
 * @var g_dtlsContextMutex
 * @brief Mutex to synchronize access to g_caSslContext.
 */
static ca_mutex g_sslContextMutex = NULL;

/**
 * @var g_sslCallback
 * @brief callback to deliver the TLS handshake result
 */
static CAErrorCallback g_sslCallback = NULL;

/**
 * Data structure for holding the data to be received.
 */
typedef struct SslRecBuf
{
    uint8_t * buff;
    size_t len;
    size_t loaded;
} SslRecBuf_t;
/**
 * Data structure for holding the data related to endpoint
 * and TLS session.
 */
typedef struct SslEndPoint
{
    mbedtls_ssl_context ssl;
    CASecureEndpoint_t sep;
    u_arraylist_t * cacheList;
    SslRecBuf_t recBuf;
    uint8_t master[MASTER_SECRET_LEN];
    uint8_t random[2*RANDOM_LEN];
#ifdef __WITH_DTLS__
    mbedtls_ssl_cookie_ctx cookieCtx;
    mbedtls_timing_delay_context timer;
#endif // __WITH_DTLS__
} SslEndPoint_t;

void CAsetPskCredentialsCallback(CAgetPskCredentialsHandler credCallback)
{
    // TODO Does this method needs protection of tlsContextMutex?
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    g_getCredentialsCallback = credCallback;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}

void CAsetPkixInfoCallback(CAgetPkixInfoHandler infoCallback)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    g_getPkixInfoCallback = infoCallback;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}
void CAsetCredentialTypesCallback(CAgetCredentialTypesHandler credTypesCallback)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    g_getCredentialTypesCallback = credTypesCallback;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}

static int GetAdapterIndex(CATransportAdapter_t adapter)
{
    switch (adapter)
    {
        case CA_ADAPTER_IP:
            return 0;
        case CA_ADAPTER_TCP:
            return 1;
        default:
            OIC_LOG(ERROR, NET_SSL_TAG, "Unsupported adapter");
            return -1;
    }
}
/**
 * Write callback.
 *
 * @param[in]  tep    TLS endpoint
 * @param[in]  data    message
 * @param[in]  dataLen    message length
 *
 * @return  message length or -1 on error.
 */
static int SendCallBack(void * tep, const unsigned char * data, size_t dataLen)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(tep, NET_SSL_TAG, "secure endpoint is NULL", -1);
    VERIFY_NON_NULL_RET(data, NET_SSL_TAG, "data is NULL", -1);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Data len: %zu", dataLen);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Adapter: %u", ((SslEndPoint_t * )tep)->sep.endpoint.adapter);
    ssize_t sentLen = 0;
    int adapterIndex = GetAdapterIndex(((SslEndPoint_t * )tep)->sep.endpoint.adapter);
    if (0 == adapterIndex || 1 == adapterIndex)
    {
        CAPacketSendCallback sendCallback = g_caSslContext->adapterCallbacks[adapterIndex].sendCallback;
        sentLen = sendCallback(&(((SslEndPoint_t * )tep)->sep.endpoint), (const void *) data, dataLen);
        if (sentLen != dataLen)
        {
            OIC_LOG_V(DEBUG, NET_SSL_TAG,
                      "Packet was partially sent - total/sent/remained bytes : %d/%d/%d",
                      sentLen, dataLen, (dataLen - sentLen));
        }
    }
    else
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Unsupported adapter");
    }

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return sentLen;
}
/**
 * Read callback.
 *
 * @param[in]  tep    TLS endpoint
 * @param[in]  data    message
 * @param[in]  dataLen    message length
 *
 * @return  read length
 */
static int RecvCallBack(void * tep, unsigned char * data, size_t dataLen)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(tep, NET_SSL_TAG, "endpoint is NULL", 0);
    VERIFY_NON_NULL_RET(data, NET_SSL_TAG, "data is NULL", 0);

    SslRecBuf_t *recBuf = &((SslEndPoint_t *)tep)->recBuf;
    size_t retLen = (recBuf->len > recBuf->loaded ? recBuf->len - recBuf->loaded : 0);
    retLen = (retLen < dataLen ? retLen : dataLen);

    memcpy(data, recBuf->buff + recBuf->loaded, retLen);
    recBuf->loaded += retLen;

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return (int)retLen;
}

/**
 * Parse chain of X.509 certificates.
 *
 * @param[out] crt     container for X.509 certificates
 * @param[in]  data    buffer with X.509 certificates. Certificates may be in either in PEM
                       or DER format in a jumble. Each PEM certificate must be NULL-terminated.
 * @param[in]  bufLen  buffer length
 *
 * @return  0 on success, -1 on error
 */
static int ParseChain(mbedtls_x509_crt * crt, const unsigned char * buf, int bufLen)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(crt, NET_SSL_TAG, "Param crt is NULL" , -1);
    VERIFY_NON_NULL_RET(buf, NET_SSL_TAG, "Param buf is NULL" , -1);

    int pos = 0;
    int ret = 0;
    size_t len = 0;
    unsigned char * tmp = NULL;

    char pemCertHeader[] = {
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x42, 0x45, 0x47, 0x49, 0x4e, 0x20, 0x43, 0x45, 0x52,
        0x54, 0x49, 0x46, 0x49, 0x43, 0x41, 0x54, 0x45, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d
    };
    char pemCertFooter[] = {
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x45, 0x4e, 0x44, 0x20, 0x43, 0x45, 0x52, 0x54, 0x49,
        0x46, 0x49, 0x43, 0x41, 0x54, 0x45, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d
    };
    size_t pemCertHeaderLen = sizeof(pemCertHeader);
    size_t pemCertFooterLen = sizeof(pemCertFooter);

    while (pos < bufLen)
    {
        if (buf[pos] == 0x30 && buf[pos + 1] == 0x82)
        {
            tmp = (unsigned char *)buf + pos + 1;
            CHECK_MBEDTLS_RET(mbedtls_asn1_get_len, &tmp, buf + bufLen, &len);
            if (pos + len < bufLen)
            {
                CHECK_MBEDTLS_RET(mbedtls_x509_crt_parse_der, crt, buf + pos, len + 4);
            }
            pos += len + 4;
        }
        else if (0 == memcmp(buf + pos, pemCertHeader, pemCertHeaderLen))
        {
            void * endPos = NULL;
            endPos = memmem(&(buf[pos]), bufLen - pos, pemCertFooter, pemCertFooterLen);
            if (NULL == endPos)
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "Error: end of PEM certificate not found.");
                OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
                return -1;
            }
            if ((*((char*)endPos + pemCertFooterLen + 0) == 0x0d) &&
                (*((char*)endPos + pemCertFooterLen + 1) == 0x0a) &&
                (*((char*)endPos + pemCertFooterLen + 2) == 0x00))
            {
                len = (char*)endPos - ((char*)buf + pos) + pemCertFooterLen + 3;
            }
            else if ((*((char*)endPos + pemCertFooterLen + 0) == 0x0a) &&
                     (*((char*)endPos + pemCertFooterLen + 1) == 0x00))
            {
                len = (char*)endPos - ((char*)buf + pos) + pemCertFooterLen + 2;
            }
            else
            {
                OIC_LOG_V(ERROR, NET_SSL_TAG, "Incorrect PEM certificate ending");
                OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
                return -1;
            }
            CHECK_MBEDTLS_RET(mbedtls_x509_crt_parse, crt, buf + pos, len);
            pos += len;
        }
        else
        {
             OIC_LOG_BUFFER(DEBUG, NET_SSL_TAG, buf, bufLen);
             OIC_LOG_V(ERROR, NET_SSL_TAG, "parseChain returned -0x%x", -ret);
             OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
             return -1;
        }
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return 0;

exit:
    return -1;
}

//Loads PKIX related information from SRM
static int InitPKIX(CATransportAdapter_t adapter)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(g_getPkixInfoCallback, NET_SSL_TAG, "PKIX info callback is NULL", -1);
    g_getPkixInfoCallback(&g_pkiInfo);

    mbedtls_x509_crt_free(&g_caSslContext->ca);
    mbedtls_x509_crt_free(&g_caSslContext->crt);
    mbedtls_pk_free(&g_caSslContext->pkey);
    mbedtls_x509_crl_free(&g_caSslContext->crl);

    mbedtls_x509_crt_init(&g_caSslContext->ca);
    mbedtls_x509_crt_init(&g_caSslContext->crt);
    mbedtls_pk_init(&g_caSslContext->pkey);
    mbedtls_x509_crl_init(&g_caSslContext->crl);

    mbedtls_ssl_config * serverConf = (adapter == CA_ADAPTER_IP ?
                                   &g_caSslContext->serverDtlsConf : &g_caSslContext->serverTlsConf);
    mbedtls_ssl_config * clientConf = (adapter == CA_ADAPTER_IP ?
                                   &g_caSslContext->clientDtlsConf : &g_caSslContext->clientTlsConf);
    // optional
    int ret = ParseChain(&g_caSslContext->crt, g_pkiInfo.crt.data, g_pkiInfo.crt.len);
    if (0 != ret)
    {
        OIC_LOG(WARNING, NET_SSL_TAG, "Own certificate chain parsing error");
        goto required;
    }
    ret =  mbedtls_pk_parse_key(&g_caSslContext->pkey, g_pkiInfo.key.data, g_pkiInfo.key.len,
                                                                               NULL, 0);
    if (0 != ret)
    {
        OIC_LOG(WARNING, NET_SSL_TAG, "Key parsing error");
        goto required;
    }

    ret = mbedtls_ssl_conf_own_cert(serverConf, &g_caSslContext->crt, &g_caSslContext->pkey);
    if (0 != ret)
    {
        OIC_LOG(WARNING, NET_SSL_TAG, "Own certificate parsing error");
        goto required;
    }
    ret = mbedtls_ssl_conf_own_cert(clientConf, &g_caSslContext->crt, &g_caSslContext->pkey);
    if(0 != ret)
    {
        OIC_LOG(WARNING, NET_SSL_TAG, "Own certificate configuration error");
        goto required;
    }

    required:
    ret = ParseChain(&g_caSslContext->ca, g_pkiInfo.ca.data, g_pkiInfo.ca.len);
    if(0 != ret)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "CA chain parsing error");
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return -1;
    }

    ret = mbedtls_x509_crl_parse_der(&g_caSslContext->crl, g_pkiInfo.crl.data, g_pkiInfo.crl.len);
    if(0 != ret)
    {
        OIC_LOG(WARNING, NET_SSL_TAG, "CRL parsing error");
        mbedtls_ssl_conf_ca_chain(clientConf, &g_caSslContext->ca, NULL);
        mbedtls_ssl_conf_ca_chain(serverConf, &g_caSslContext->ca, NULL);
    }
    else
    {
        mbedtls_ssl_conf_ca_chain(clientConf, &g_caSslContext->ca, &g_caSslContext->crl);
        mbedtls_ssl_conf_ca_chain(serverConf, &g_caSslContext->ca, &g_caSslContext->crl);
    }

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return 0;
}

/*
 * PSK callback.
 *
 * @param[in]  notUsed     opaque context
 * @param[in]  ssl    mbedTLS context
 * @param[in]  desc    identity
 * @param[in]  descLen    identity length
 *
 * @return  0 on success any other return value will result in a denied PSK identity
 */
static int GetPskCredentialsCallback(void * notUsed, mbedtls_ssl_context * ssl,
                                     const unsigned char * desc, size_t descLen)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(g_getCredentialsCallback, NET_SSL_TAG, "Credential callback s NULL", -1);
    VERIFY_NON_NULL_RET(ssl, NET_SSL_TAG, "ssl pointer is NULL", -1);
    VERIFY_NON_NULL_RET(desc, NET_SSL_TAG, "desc pointer is NULL", -1);
    if (descLen > CA_MAX_ENDPOINT_IDENTITY_LEN)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "desc too long!");
        return -1;
    }
    (void) notUsed;
    uint8_t keyBuf[PSK_LENGTH] = {0};

    // Retrieve the credentials blob from security module
    int ret = g_getCredentialsCallback(CA_DTLS_PSK_KEY, desc, descLen, keyBuf, PSK_LENGTH);
    if (ret > 0)
    {
        memcpy(((SslEndPoint_t *) ssl)->sep.identity.id, desc, descLen);
        ((SslEndPoint_t *) ssl)->sep.identity.id_length = descLen;
        OIC_LOG(DEBUG, NET_SSL_TAG, "PSK:");
        OIC_LOG_BUFFER(DEBUG, NET_SSL_TAG, keyBuf, ret);

        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return(mbedtls_ssl_set_hs_psk(ssl, keyBuf, ret));
    }
    OIC_LOG_V(WARNING, NET_SSL_TAG, "Out %s", __func__);
    return -1;
}
/**
 * Gets session corresponding for endpoint.
 *
 * @param[in]  peer    remote address
 *
 * @return  TLS session or NULL
 */
static SslEndPoint_t *GetSslPeer(const CAEndpoint_t *peer)
{
    uint32_t listIndex = 0;
    uint32_t listLength = 0;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(peer, NET_SSL_TAG, "TLS peer is NULL", NULL);

    SslEndPoint_t *tep = NULL;
    listLength = u_arraylist_length(g_caSslContext->peerList);
    for (listIndex = 0; listIndex < listLength; listIndex++)
    {
        tep = (SslEndPoint_t *) u_arraylist_get(g_caSslContext->peerList, listIndex);
        if (NULL == tep)
        {
            continue;
        }
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Compare [%s:%d] and [%s:%d]",
                  peer->addr, peer->port, tep->sep.endpoint.addr, tep->sep.endpoint.port);
        if((0 == strncmp(peer->addr, tep->sep.endpoint.addr, MAX_ADDR_STR_SIZE_CA))
                && (peer->port == tep->sep.endpoint.port))
        {
            OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
            return tep;
        }
    }
    OIC_LOG(DEBUG, NET_SSL_TAG, "Return NULL");
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return NULL;
}

#ifdef _ENABLE_MULTIPLE_OWNER_
/**
 * Gets CA secure endpoint info corresponding for endpoint.
 *
 * @param[in]  peer    remote address
 *
 * @return  CASecureEndpoint or NULL
 */
const CASecureEndpoint_t *GetCASecureEndpointData(const CAEndpoint_t* peer)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);

    // TODO: Added as workaround, need to debug
    ca_mutex_unlock(g_sslContextMutex);

    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return NULL;
    }

    SslEndPoint_t* sslPeer = GetSslPeer(peer);
    if(sslPeer)
    {
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        ca_mutex_unlock(g_sslContextMutex);
        return &sslPeer->sep;
    }

    OIC_LOG(DEBUG, NET_SSL_TAG, "Return NULL");
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    ca_mutex_unlock(g_sslContextMutex);
    return NULL;
}
#endif

/**
 * Deletes cached message.
 *
 * @param[in]  msg    message
 */
static void DeleteCacheMessage(SslCacheMessage_t * msg)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_VOID(msg, NET_SSL_TAG, "msg");

    OICFree(msg->data);
    OICFree(msg);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}
/**
 * Deletes cached message list.
 *
 * @param[in] cacheList  list of cached messages
 */
static void DeleteCacheList(u_arraylist_t * cacheList)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_VOID(cacheList, NET_SSL_TAG, "cacheList");
    uint32_t listIndex = 0;
    uint32_t listLength = 0;

    listLength = u_arraylist_length(cacheList);
    for (listIndex = 0; listIndex < listLength; listIndex++)
    {
        SslCacheMessage_t * msg = (SslCacheMessage_t *) u_arraylist_get(cacheList, listIndex);
        if (NULL != msg)
        {
            DeleteCacheMessage(msg);
        }
    }
    u_arraylist_free(&cacheList);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}
/**
 * Deletes endpoint with session.
 *
 * @param[in]  tep    endpoint with session info
 */
static void DeleteSslEndPoint(SslEndPoint_t * tep)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_VOID(tep, NET_SSL_TAG, "tep");

    mbedtls_ssl_free(&tep->ssl);
#ifdef __WITH_DTLS__
    mbedtls_ssl_cookie_free(&tep->cookieCtx);
#endif
    DeleteCacheList(tep->cacheList);
    OICFree(tep);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}
/**
 * Removes endpoint session from list.
 *
 * @param[in]  endpoint    remote address
 */
static void RemovePeerFromList(CAEndpoint_t * endpoint)
{
    uint32_t listLength = u_arraylist_length(g_caSslContext->peerList);
    VERIFY_NON_NULL_VOID(endpoint, NET_SSL_TAG, "endpoint");
    for (uint32_t listIndex = 0; listIndex < listLength; listIndex++)
    {
        SslEndPoint_t * tep = (SslEndPoint_t *)u_arraylist_get(g_caSslContext->peerList,listIndex);
        if (NULL == tep)
        {
            continue;
        }
        if(0 == strncmp(endpoint->addr, tep->sep.endpoint.addr, MAX_ADDR_STR_SIZE_CA)
                && (endpoint->port == tep->sep.endpoint.port))
        {
            u_arraylist_remove(g_caSslContext->peerList, listIndex);
            DeleteSslEndPoint(tep);
            return;
        }
    }
}
/**
 * Deletes session list.
 */
static void DeletePeerList()
{
    uint32_t listLength = u_arraylist_length(g_caSslContext->peerList);
    for (uint32_t listIndex = 0; listIndex < listLength; listIndex++)
    {
        SslEndPoint_t * tep = (SslEndPoint_t *)u_arraylist_get(g_caSslContext->peerList,listIndex);
        if (NULL == tep)
        {
            continue;
        }
        DeleteSslEndPoint(tep);
    }
    u_arraylist_free(&g_caSslContext->peerList);
}

CAResult_t CAcloseSslConnection(const CAEndpoint_t *endpoint)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG, "Param endpoint is NULL" , CA_STATUS_INVALID_PARAM);

    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }
    SslEndPoint_t * tep = GetSslPeer(endpoint);
    if (NULL == tep)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Session does not exist");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }
    /* No error checking, the connection might be closed already */
    int ret = 0;
    do
    {
        ret = mbedtls_ssl_close_notify(&tep->ssl);
    }
    while (MBEDTLS_ERR_SSL_WANT_WRITE == ret);

    RemovePeerFromList(&tep->sep.endpoint);
    ca_mutex_unlock(g_sslContextMutex);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}

void CAcloseSslConnectionAll()
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return;
    }

    uint32_t listLength = u_arraylist_length(g_caSslContext->peerList);
    for (uint32_t i = listLength; i > 0; i--)
    {
        SslEndPoint_t *tep = (SslEndPoint_t *)u_arraylist_remove(g_caSslContext->peerList, i - 1);
        if (NULL == tep)
        {
            continue;
        }
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "SSL Connection [%s:%d]",
                  tep->sep.endpoint.addr, tep->sep.endpoint.port);

        // TODO: need to check below code after socket close is ensured.
        /*int ret = 0;
        do
        {
            ret = mbedtls_ssl_close_notify(&tep->ssl);
        }
        while (MBEDTLS_ERR_SSL_WANT_WRITE == ret);*/

        DeleteSslEndPoint(tep);
    }
    ca_mutex_unlock(g_sslContextMutex);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return;
}
/**
 * Creates session for endpoint.
 *
 * @param[in]  endpoint    remote address
 * @param[in]  config    mbedTLS configuration info
 *
 * @return  TLS endpoint or NULL
 */
static SslEndPoint_t * NewSslEndPoint(const CAEndpoint_t * endpoint, mbedtls_ssl_config * config)
{
    SslEndPoint_t * tep = NULL;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG, "endpoint", NULL);
    VERIFY_NON_NULL_RET(config, NET_SSL_TAG, "config", NULL);

    tep = (SslEndPoint_t *) OICCalloc(1, sizeof (SslEndPoint_t));
    if (NULL == tep)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Malloc failed!");
        return NULL;
    }

    tep->sep.endpoint = *endpoint;
    tep->sep.endpoint.flags = (CATransportFlags_t)(tep->sep.endpoint.flags | CA_SECURE);

    if(0 != mbedtls_ssl_setup(&tep->ssl, config))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Setup failed");
        OICFree(tep);
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return NULL;
    }

    mbedtls_ssl_set_bio(&tep->ssl, tep, SendCallBack, RecvCallBack, NULL);
    if (MBEDTLS_SSL_TRANSPORT_DATAGRAM == config->transport)
    {
        mbedtls_ssl_set_timer_cb(&tep->ssl, &tep->timer,
                                  mbedtls_timing_set_delay, mbedtls_timing_get_delay);
        if (MBEDTLS_SSL_IS_SERVER == config->endpoint)
        {
            if (0 != mbedtls_ssl_cookie_setup(&tep->cookieCtx, mbedtls_ctr_drbg_random,
                                              &g_caSslContext->rnd))
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "Cookie setup failed!");
                OICFree(tep);
                OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
                return NULL;
            }
            mbedtls_ssl_conf_dtls_cookies(config, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check,
                                          &tep->cookieCtx);
            if (0 != mbedtls_ssl_set_client_transport_id(&tep->ssl,
                                    (const unsigned char *) endpoint->addr, sizeof(endpoint->addr)))
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "Transport id setup failed!");
                OICFree(tep);
                OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
                return NULL;
            }
        }
    }
    tep->cacheList = u_arraylist_create();
    if (NULL == tep->cacheList)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "cacheList initialization failed!");
        mbedtls_ssl_free(&tep->ssl);
        OICFree(tep);
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return NULL;
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return tep;
}
/**
 * Initializes PSK identity.
 *
 * @param[out]  config    client/server config to be updated
 *
 * @return  0 on success or -1 on error
 */
static int InitPskIdentity(mbedtls_ssl_config * config)
{
    uint8_t idBuf[UUID_LENGTH] = {0};
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(config, NET_SSL_TAG, "Param config is NULL" , -1);

    if (0 > g_getCredentialsCallback(CA_DTLS_PSK_IDENTITY, NULL, 0, idBuf, UUID_LENGTH))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Identity not found");
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return -1;
    }
    if (0 != mbedtls_ssl_conf_psk(config, idBuf, 0, idBuf, UUID_LENGTH))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Identity initialization failed!");
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return -1;
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return 0;
}
static void SetupCipher(mbedtls_ssl_config * config, CATransportAdapter_t adapter)
{
    int index = 0;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    if (NULL == g_getCredentialTypesCallback)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Param callback is null");
        return;
    }

    g_getCredentialTypesCallback(g_caSslContext->cipherFlag);
    // Retrieve the PSK credential from SRM
    // PIN OTM if (true == g_caSslContext->cipherFlag[0] && 0 != InitPskIdentity(config))
    if (0 != InitPskIdentity(config))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "PSK identity initialization failed!");
    }

    // Retrieve the ECC credential from SRM
    if (true == g_caSslContext->cipherFlag[1] || ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA == g_caSslContext->cipher)
    {
        int ret = InitPKIX(adapter);
        if (0 != ret)
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "Failed to init X.509");
        }
    }

    memset(g_cipherSuitesList, 0, sizeof(g_cipherSuitesList));
    if (ADAPTER_CIPHER_MAX != g_caSslContext->cipher)
    {
        g_cipherSuitesList[index] = tlsCipher[g_caSslContext->cipher][0];
        index ++;
    }
    if (true == g_caSslContext->cipherFlag[1])
    {
        g_cipherSuitesList[index] = MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8;
        index ++;
    }
    if (true == g_caSslContext->cipherFlag[0])
    {
       g_cipherSuitesList[index] = MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256;
    }

    mbedtls_ssl_conf_ciphersuites(config, g_cipherSuitesList);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}
/**
 * Initiate TLS handshake with endpoint.
 *
 * @param[in]  endpoint    remote address
 *
 * @return  TLS endpoint or NULL
 */
static SslEndPoint_t * InitiateTlsHandshake(const CAEndpoint_t *endpoint)
{
    int ret = 0;
    SslEndPoint_t * tep = NULL;

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG, "Param endpoint is NULL" , NULL);


    mbedtls_ssl_config * config = (endpoint->adapter == CA_ADAPTER_IP ?
                                   &g_caSslContext->clientDtlsConf : &g_caSslContext->clientTlsConf);
    tep = NewSslEndPoint(endpoint, config);
    if (NULL == tep)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Malloc failed!");
        return NULL;
    }

    //Load allowed SVR suites from SVR DB
    SetupCipher(config, endpoint->adapter);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Add %s:%d", tep->sep.endpoint.addr, tep->sep.endpoint.port);
    ret = u_arraylist_add(g_caSslContext->peerList, (void *) tep);
    if (!ret)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "u_arraylist_add failed!");
        DeleteSslEndPoint(tep);
        return NULL;
    }

    while (MBEDTLS_SSL_HANDSHAKE_OVER > tep->ssl.state)
    {
        ret = mbedtls_ssl_handshake_step(&tep->ssl);
        if (MBEDTLS_ERR_SSL_CONN_EOF == ret)
        {
            break;
        }
        SSL_CHECK_FAIL(tep, ret, "Handshake error", 0, NULL, MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return tep;
}
#ifdef __WITH_DTLS__
/**
 * Stops DTLS retransmission.
 */
static void StopRetransmit()
{
    if (g_caSslContext)
    {
        unregisterTimer(g_caSslContext->timerId);
    }
}
#endif
void CAdeinitSslAdapter()
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);

    VERIFY_NON_NULL_VOID(g_caSslContext, NET_SSL_TAG, "context is NULL");
    VERIFY_NON_NULL_VOID(g_sslContextMutex, NET_SSL_TAG, "context mutex is NULL");

    //Lock tlsContext mutex
    ca_mutex_lock(g_sslContextMutex);

    // Clear all lists
    DeletePeerList();

    // De-initialize mbedTLS
    mbedtls_x509_crt_free(&g_caSslContext->crt);
    mbedtls_pk_free(&g_caSslContext->pkey);
#ifdef __WITH_TLS__
    mbedtls_ssl_config_free(&g_caSslContext->clientTlsConf);
    mbedtls_ssl_config_free(&g_caSslContext->serverTlsConf);
#endif // __WITH_TLS__
#ifdef __WITH_DTLS__
    mbedtls_ssl_config_free(&g_caSslContext->clientDtlsConf);
    mbedtls_ssl_config_free(&g_caSslContext->serverDtlsConf);
#endif // __WITH_DTLS__
    mbedtls_ctr_drbg_free(&g_caSslContext->rnd);
    mbedtls_entropy_free(&g_caSslContext->entropy);
#ifdef __WITH_DTLS__
    StopRetransmit();
#endif
    // De-initialize tls Context
    OICFree(g_caSslContext);
    g_caSslContext = NULL;

    // Unlock tlsContext mutex and de-initialize it
    ca_mutex_unlock(g_sslContextMutex);
    ca_mutex_free(g_sslContextMutex);
    g_sslContextMutex = NULL;

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s ", __func__);
}

static int InitConfig(mbedtls_ssl_config * conf, int transport, int mode)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(conf, NET_SSL_TAG, "Param conf is NULL" , -1);
    mbedtls_ssl_config_init(conf);
    if (mbedtls_ssl_config_defaults(conf, mode, transport, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Config initialization failed!");
        return -1;
    }

    mbedtls_ssl_conf_psk_cb(conf, GetPskCredentialsCallback, NULL);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, &g_caSslContext->rnd);
    mbedtls_ssl_conf_curves(conf, curve[ADAPTER_CURVE_SECP256R1]);
    mbedtls_ssl_conf_min_version(conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_renegotiation(conf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);

#if !defined(NDEBUG) || defined(TB_LOG)
    mbedtls_ssl_conf_dbg(conf, DebugSsl, NULL);
    mbedtls_debug_set_threshold(MBED_TLS_DEBUG_LEVEL);
#endif
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return 0;
}
#ifdef __WITH_DTLS__
/**
 * Starts DTLS retransmission.
 */
static int StartRetransmit()
{
    uint32_t listIndex = 0;
    uint32_t listLength = 0;
    SslEndPoint_t *tep = NULL;
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL. Stop retransmission");
        return -1;
    }
    ca_mutex_lock(g_sslContextMutex);
    if (g_caSslContext->timerId != -1)
    {
        //clear previous timer
        unregisterTimer(g_caSslContext->timerId);

        listLength = u_arraylist_length(g_caSslContext->peerList);
        for (listIndex = 0; listIndex < listLength; listIndex++)
        {
            tep = (SslEndPoint_t *) u_arraylist_get(g_caSslContext->peerList, listIndex);
            if (NULL == tep
                || MBEDTLS_SSL_TRANSPORT_STREAM == tep->ssl.conf->transport
                || MBEDTLS_SSL_HANDSHAKE_OVER == tep->ssl.state)
            {
                continue;
            }
            int ret = mbedtls_ssl_handshake_step(&tep->ssl);

            if (MBEDTLS_ERR_SSL_CONN_EOF != ret)
            {
                SSL_CHECK_FAIL(tep, ret, "Retransmission", NULL, -1,
                MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);
            }
        }
    }
    //start new timer
    registerTimer(RETRANSMISSION_TIME, &g_caSslContext->timerId, (void *) StartRetransmit);
    ca_mutex_unlock(g_sslContextMutex);
    return 0;
}
#endif

CAResult_t CAinitSslAdapter()
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    // Initialize mutex for tlsContext
    if (NULL == g_sslContextMutex)
    {
        g_sslContextMutex = ca_mutex_new();
        VERIFY_NON_NULL_RET(g_sslContextMutex, NET_SSL_TAG, "malloc failed", CA_MEMORY_ALLOC_FAILED);
    }
    else
    {
        OIC_LOG(INFO, NET_SSL_TAG, "Done already!");
        return CA_STATUS_OK;
    }

    // Lock tlsContext mutex and create tlsContext
    ca_mutex_lock(g_sslContextMutex);
    g_caSslContext = (SslContext_t *)OICCalloc(1, sizeof(SslContext_t));

    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context malloc failed");
        ca_mutex_unlock(g_sslContextMutex);
        ca_mutex_free(g_sslContextMutex);
        g_sslContextMutex = NULL;
        return CA_MEMORY_ALLOC_FAILED;
    }

    // Create peer list
    g_caSslContext->peerList = u_arraylist_create();

    if(NULL == g_caSslContext->peerList)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "peerList initialization failed!");
        OICFree(g_caSslContext);
        g_caSslContext = NULL;
        ca_mutex_unlock(g_sslContextMutex);
        ca_mutex_free(g_sslContextMutex);
        g_sslContextMutex = NULL;
        return CA_STATUS_FAILED;
    }

    /* Initialize TLS library
     */
#if !defined(NDEBUG) || defined(TB_LOG)
    char version[MBED_TLS_VERSION_LEN];
    mbedtls_version_get_string(version);
    OIC_LOG_V(INFO, NET_SSL_TAG, "mbed TLS version: %s", version);
#endif

    /* Entropy settings
     */
    mbedtls_entropy_init(&g_caSslContext->entropy);
    mbedtls_ctr_drbg_init(&g_caSslContext->rnd);

#ifdef __unix__
    unsigned char seed[sizeof(SEED)] = {0};
    int urandomFd = -2;
    urandomFd = open("/dev/urandom", O_RDONLY);
    if(urandomFd == -1)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Fails open /dev/urandom!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        return CA_STATUS_FAILED;
    }
    if(0 > read(urandomFd, seed, sizeof(seed)))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Fails read from /dev/urandom!");
        close(urandomFd);
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        return CA_STATUS_FAILED;
    }
    close(urandomFd);

#else
    unsigned char * seed = (unsigned char*) SEED;
#endif
    if(0 != mbedtls_ctr_drbg_seed(&g_caSslContext->rnd, mbedtls_entropy_func,
                                  &g_caSslContext->entropy, seed, sizeof(SEED)))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Seed initialization failed!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        return CA_STATUS_FAILED;
    }
    mbedtls_ctr_drbg_set_prediction_resistance(&g_caSslContext->rnd, MBEDTLS_CTR_DRBG_PR_OFF);

#ifdef __WITH_TLS__
    if (0 != InitConfig(&g_caSslContext->clientTlsConf,
                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_IS_CLIENT))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Client config initialization failed!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return CA_STATUS_FAILED;
    }

    if (0 != InitConfig(&g_caSslContext->serverTlsConf,
                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_IS_SERVER))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Server config initialization failed!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return CA_STATUS_FAILED;
    }
#endif // __WITH_TLS__
#ifdef __WITH_DTLS__
    if (0 != InitConfig(&g_caSslContext->clientDtlsConf,
                        MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_IS_CLIENT))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Client config initialization failed!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return CA_STATUS_FAILED;
    }

    if (0 != InitConfig(&g_caSslContext->serverDtlsConf,
                        MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_IS_SERVER))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Server config initialization failed!");
        ca_mutex_unlock(g_sslContextMutex);
        CAdeinitSslAdapter();
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return CA_STATUS_FAILED;
    }
#endif // __WITH_DTLS__

    // set default cipher
    g_caSslContext->cipher = ADAPTER_CIPHER_MAX;

    // init X.509
    mbedtls_x509_crt_init(&g_caSslContext->ca);
    mbedtls_x509_crt_init(&g_caSslContext->crt);
    mbedtls_pk_init(&g_caSslContext->pkey);
    mbedtls_x509_crl_init(&g_caSslContext->crl);

#ifdef __WITH_DTLS__
    g_caSslContext->timerId = -1;
#endif

   ca_mutex_unlock(g_sslContextMutex);
#ifdef __WITH_DTLS__
    StartRetransmit();
#endif

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}

SslCacheMessage_t *  NewCacheMessage(uint8_t * data, size_t dataLen)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(data, NET_SSL_TAG, "Param data is NULL" , NULL);
    if (0 == dataLen)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "dataLen is equal to zero");
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return NULL;
    }
    SslCacheMessage_t * message = (SslCacheMessage_t *) OICCalloc(1, sizeof(SslCacheMessage_t));
    if (NULL == message)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "calloc failed!");
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return NULL;
    }

    message->data = (uint8_t *)OICCalloc(dataLen, sizeof(uint8_t));
    if (NULL == message->data)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "calloc failed!");
        OICFree(message);
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return NULL;
    }
    memcpy(message->data, data, dataLen);
    message->len = dataLen;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return message;
}

/* Send data via TLS connection.
 */
CAResult_t CAencryptSsl(const CAEndpoint_t *endpoint,
                        void *data, uint32_t dataLen)
{
    int ret = 0;

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s ", __func__);

    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG,"Remote address is NULL", CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(data, NET_SSL_TAG, "Data is NULL", CA_STATUS_INVALID_PARAM);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Port %d", endpoint->port);

    if (0 == dataLen)
    {
        OIC_LOG_V(ERROR, NET_SSL_TAG, "dataLen is zero [%d]", dataLen);
        return CA_STATUS_FAILED;
    }

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Data to be encrypted dataLen [%d]", dataLen);

    ca_mutex_lock(g_sslContextMutex);
    if(NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }

    SslEndPoint_t * tep = GetSslPeer(endpoint);
    if (NULL == tep)
    {
        tep = InitiateTlsHandshake(endpoint);
    }
    if (NULL == tep)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "TLS handshake failed");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }

    if (MBEDTLS_SSL_HANDSHAKE_OVER == tep->ssl.state)
    {
        unsigned char *dataBuf = (unsigned char *)data;
        size_t written = 0;

        do
        {
            ret = mbedtls_ssl_write(&tep->ssl, dataBuf, dataLen - written);
            if (ret < 0)
            {
                if (MBEDTLS_ERR_SSL_WANT_WRITE != ret)
                {
                    OIC_LOG_V(ERROR, NET_SSL_TAG, "mbedTLS write failed! returned 0x%x", -ret);
                    RemovePeerFromList(&tep->sep.endpoint);
                    ca_mutex_unlock(g_sslContextMutex);
                    return CA_STATUS_FAILED;
                }
                continue;
            }
            OIC_LOG_V(DEBUG, NET_SSL_TAG, "mbedTLS write returned with sent bytes[%d]", ret);

            dataBuf += ret;
            written += ret;
        } while (dataLen > written);

    }
    else
    {
        SslCacheMessage_t * msg = NewCacheMessage((uint8_t*) data, dataLen);
        if (NULL == msg || !u_arraylist_add(tep->cacheList, (void *) msg))
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "u_arraylist_add failed!");
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_FAILED;
        }
    }

    ca_mutex_unlock(g_sslContextMutex);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}
/**
 * Sends cached messages via TLS connection.
 *
 * @param[in]  tep    remote address with session info
 */
static void SendCacheMessages(SslEndPoint_t * tep)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_VOID(tep, NET_SSL_TAG, "Param tep is NULL");

    uint32_t listIndex = 0;
    uint32_t listLength = 0;
    listLength = u_arraylist_length(tep->cacheList);
    for (listIndex = 0; listIndex < listLength;)
    {
        int ret = 0;
        SslCacheMessage_t * msg = (SslCacheMessage_t *) u_arraylist_get(tep->cacheList, listIndex);
        if (NULL != msg && NULL != msg->data && 0 != msg->len)
        {
            unsigned char *dataBuf = (unsigned char *)msg->data;
            size_t written = 0;

            do
            {
                ret = mbedtls_ssl_write(&tep->ssl, dataBuf, msg->len - written);
                if (ret < 0)
                {
                    if (MBEDTLS_ERR_SSL_WANT_WRITE != ret)
                    {
                        OIC_LOG_V(ERROR, NET_SSL_TAG, "mbedTLS write failed! returned -0x%x", -ret);
                        break;
                    }
                    continue;
                }
                OIC_LOG_V(DEBUG, NET_SSL_TAG, "mbedTLS write returned with sent bytes[%d]", ret);

                dataBuf += ret;
                written += ret;
            } while (msg->len > written);

            if (u_arraylist_remove(tep->cacheList, listIndex))
            {
                DeleteCacheMessage(msg);
                // Reduce list length by 1 as we removed one element.
                listLength--;
            }
            else
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "u_arraylist_remove failed.");
                break;
            }
        }
        else
        {
            // Move to the next element
            ++listIndex;
        }
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}

void CAsetSslHandshakeCallback(CAErrorCallback tlsHandshakeCallback)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    g_sslCallback = tlsHandshakeCallback;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}

/* Read data from TLS connection
 */
CAResult_t CAdecryptSsl(const CASecureEndpoint_t *sep, uint8_t *data, uint32_t dataLen)
{
    int ret = 0;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(sep, NET_SSL_TAG, "endpoint is NULL" , CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(data, NET_SSL_TAG, "Param data is NULL" , CA_STATUS_INVALID_PARAM);

    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }


    SslEndPoint_t * peer = GetSslPeer(&sep->endpoint);
    if (NULL == peer)
    {
        mbedtls_ssl_config * config = (sep->endpoint.adapter == CA_ADAPTER_IP ?
                                   &g_caSslContext->serverDtlsConf : &g_caSslContext->serverTlsConf);
        peer = NewSslEndPoint(&sep->endpoint, config);
        if (NULL == peer)
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "Malloc failed!");
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_FAILED;
        }
        //Load allowed TLS suites from SVR DB
        SetupCipher(config, sep->endpoint.adapter);

        ret = u_arraylist_add(g_caSslContext->peerList, (void *) peer);
        if (!ret)
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "u_arraylist_add failed!");
            OICFree(peer);
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_FAILED;
        }
    }

    peer->recBuf.buff = data;
    peer->recBuf.len = dataLen;
    peer->recBuf.loaded = 0;

    while (MBEDTLS_SSL_HANDSHAKE_OVER != peer->ssl.state)
    {
        ret = mbedtls_ssl_handshake_step(&peer->ssl);
        if (MBEDTLS_ERR_SSL_CONN_EOF == ret)
        {
            break;
        }

        if (MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED == ret)
        {
            OIC_LOG(DEBUG, NET_SSL_TAG, "Hello verification requested");
            mbedtls_ssl_session_reset(&peer->ssl);
            mbedtls_ssl_set_client_transport_id(&peer->ssl,
                                                (const unsigned char *) sep->endpoint.addr,
                                                 sizeof(sep->endpoint.addr));
            ret = mbedtls_ssl_handshake_step(&peer->ssl);
        }
        uint32_t flags = mbedtls_ssl_get_verify_result(&peer->ssl);
        if (0 != flags)
        {
            OIC_LOG_BUFFER(ERROR, NET_SSL_TAG, (const uint8_t *) &flags, sizeof(flags));
            SSL_CHECK_FAIL(peer, flags, "Cert verification failed", 1,
                                                     CA_STATUS_FAILED, GetAlertCode(flags));
        }
        SSL_CHECK_FAIL(peer, ret, "Handshake error", 1, CA_STATUS_FAILED, MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);
        if (MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC == peer->ssl.state)
        {
            memcpy(peer->master, peer->ssl.session_negotiate->master, sizeof(peer->master));
            g_caSslContext->selectedCipher = peer->ssl.session_negotiate->ciphersuite;
        }
        if (MBEDTLS_SSL_CLIENT_KEY_EXCHANGE == peer->ssl.state)
        {
            memcpy(peer->random, peer->ssl.handshake->randbytes, sizeof(peer->random));
        }

        if (MBEDTLS_SSL_HANDSHAKE_OVER == peer->ssl.state)
        {
            SSL_RES(peer, CA_STATUS_OK);
            if (MBEDTLS_SSL_IS_CLIENT == peer->ssl.conf->endpoint)
            {
                SendCacheMessages(peer);
            }

            if (MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8 == g_caSslContext->selectedCipher ||
                MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA == g_caSslContext->selectedCipher)
            {
                char uuid[UUID_LENGTH * 2 + 5] = {0};
                void * uuidPos = NULL;
                void * userIdPos = NULL;
                const mbedtls_x509_crt * peerCert = mbedtls_ssl_get_peer_cert(&peer->ssl);
                ret = (NULL == peerCert ? -1 : 0);
                SSL_CHECK_FAIL(peer, ret, "Failed to retrieve cert", 1,
                                            CA_STATUS_FAILED, MBEDTLS_SSL_ALERT_MSG_NO_CERT);
                uuidPos = memmem(peerCert->subject_raw.p, peerCert->subject_raw.len,
                                                 UUID_PREFIX, sizeof(UUID_PREFIX) - 1);

                if (NULL != uuidPos)
                {
                    memcpy(uuid, (char*) uuidPos + sizeof(UUID_PREFIX) - 1, UUID_LENGTH * 2 + 4);
                    OIC_LOG_V(DEBUG, NET_SSL_TAG, "certificate uuid string: %s" , uuid);
                    ret = OCConvertStringToUuid(uuid, peer->sep.identity.id);
                    SSL_CHECK_FAIL(peer, ret, "Failed to convert subject", 1,
                                          CA_STATUS_FAILED, MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT);
                }
                else
                {
                    OIC_LOG(WARNING, NET_SSL_TAG, "uuid not found");
                }

                userIdPos = memmem(peerCert->subject_raw.p, peerCert->subject_raw.len,
                                             USERID_PREFIX, sizeof(USERID_PREFIX) - 1);
                if (NULL != userIdPos)
                {
                    memcpy(uuid, (char*) userIdPos + sizeof(USERID_PREFIX) - 1, UUID_LENGTH * 2 + 4);
                    ret = OCConvertStringToUuid(uuid, peer->sep.userId.id);
                    SSL_CHECK_FAIL(peer, ret, "Failed to convert subject alt name", 1,
                                      CA_STATUS_FAILED, MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT);
                }
                else
                {
                    OIC_LOG(WARNING, NET_SSL_TAG, "Subject alternative name not found");
                }
            }

            ca_mutex_unlock(g_sslContextMutex);
            OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
            return CA_STATUS_OK;
        }
    }

    if (MBEDTLS_SSL_HANDSHAKE_OVER == peer->ssl.state)
    {
        uint8_t decryptBuffer[TLS_MSG_BUF_LEN] = {0};
        do
        {
            ret = mbedtls_ssl_read(&peer->ssl, decryptBuffer, TLS_MSG_BUF_LEN);
        } while (MBEDTLS_ERR_SSL_WANT_READ == ret);

        if (MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY == ret ||
            // TinyDTLS sends fatal close_notify alert
            (MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE == ret &&
             MBEDTLS_SSL_ALERT_LEVEL_FATAL == peer->ssl.in_msg[0] &&
             MBEDTLS_SSL_ALERT_MSG_CLOSE_NOTIFY == peer->ssl.in_msg[1]))
        {
            OIC_LOG(INFO, NET_SSL_TAG, "Connection was closed gracefully");
            SSL_CLOSE_NOTIFY(peer, ret);
            RemovePeerFromList(&peer->sep.endpoint);
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_OK;
        }

        if (0 > ret)
        {
            OIC_LOG_V(ERROR, NET_SSL_TAG, "mbedtls_ssl_read returned -0x%x", -ret);
            //SSL_RES(peer, CA_STATUS_FAILED);
            RemovePeerFromList(&peer->sep.endpoint);
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_FAILED;
        }
        int adapterIndex = GetAdapterIndex(peer->sep.endpoint.adapter);
        if (0 == adapterIndex || adapterIndex == 1)
        {
            g_caSslContext->adapterCallbacks[adapterIndex].recvCallback(&peer->sep, decryptBuffer, ret);
        }
        else
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "Unsuported adapter");
            RemovePeerFromList(&peer->sep.endpoint);
            ca_mutex_unlock(g_sslContextMutex);
            return CA_STATUS_FAILED;
        }
    }

    ca_mutex_unlock(g_sslContextMutex);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}

void CAsetSslAdapterCallbacks(CAPacketReceivedCallback recvCallback,
                              CAPacketSendCallback sendCallback,
                              CATransportAdapter_t type)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_VOID(sendCallback, NET_SSL_TAG, "sendCallback is NULL");
    VERIFY_NON_NULL_VOID(recvCallback, NET_SSL_TAG, "recvCallback is NULL");
    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        return;
    }

//    if (MAX_SUPPORTED_ADAPTERS > type)
    {
        switch (type)
        {
            case CA_ADAPTER_IP:
                g_caSslContext->adapterCallbacks[0].recvCallback = recvCallback;
                g_caSslContext->adapterCallbacks[0].sendCallback = sendCallback;
                break;
            case CA_ADAPTER_TCP:
                g_caSslContext->adapterCallbacks[1].recvCallback = recvCallback;
                g_caSslContext->adapterCallbacks[1].sendCallback = sendCallback;
                break;
            default:
                OIC_LOG_V(ERROR, NET_SSL_TAG, "Unsupported adapter: %d", type);
        }
    }

    ca_mutex_unlock(g_sslContextMutex);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
}

CAResult_t CAsetTlsCipherSuite(const uint32_t cipher)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(g_getCredentialTypesCallback, NET_SSL_TAG, "Param callback is null", CA_STATUS_FAILED);
    g_getCredentialTypesCallback(g_caSslContext->cipherFlag);
    switch(cipher)
    {
        case MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA:
        {
#ifdef __WITH_TLS__
            //todo check that Cred with RSA cert exists
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                         tlsCipher[ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                         tlsCipher[ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA]);
#endif
#ifdef __WITH_DTLS__
            //todo check that Cred with RSA cert exists
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                         tlsCipher[ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                         tlsCipher[ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_RSA_WITH_AES_256_CBC_SHA;
            break;
        }
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8:
        {
            if (false == g_caSslContext->cipherFlag[1])
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "No Credential for ECC");
                return CA_STATUS_FAILED;
            }
#ifdef __WITH_TLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8]);
#endif
#ifdef __WITH_DTLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8;
            break;
        }
        case MBEDTLS_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA256:
        {
#ifdef __WITH_TLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256]);
#endif
#ifdef __WITH_DTLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_ECDH_ANON_WITH_AES_128_CBC_SHA_256;
            break;
        }
        case MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256:
        {
#if 0 // PIN OTM
            if (false == g_caSslContext->cipherFlag[0])
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "No Credential for PSK");
                return CA_STATUS_FAILED;
            }
#endif
#ifdef __WITH_TLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                          tlsCipher[ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                          tlsCipher[ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256]);
#endif
#ifdef __WITH_DTLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                          tlsCipher[ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                          tlsCipher[ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256;
            break;
        }
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM:
        {
            if (false == g_caSslContext->cipherFlag[1])
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "No Credential for ECC");
                return CA_STATUS_FAILED;
            }
#ifdef __WITH_TLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM]);
#endif
#ifdef __WITH_DTLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CCM;
            break;
        }
        case MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
        {
            if (false == g_caSslContext->cipherFlag[1])
            {
                OIC_LOG(ERROR, NET_SSL_TAG, "No Credential for ECC");
                return CA_STATUS_FAILED;
            }
#ifdef __WITH_TLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverTlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256]);
#endif
#ifdef __WITH_DTLS__
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->clientDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256]);
            mbedtls_ssl_conf_ciphersuites(&g_caSslContext->serverDtlsConf,
                                         tlsCipher[ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256]);
#endif
            g_caSslContext->cipher = ADAPTER_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256;
            break;
        }
        default:
        {
            OIC_LOG(ERROR, NET_SSL_TAG, "Unknown cipher");
            return CA_STATUS_FAILED;
        }
    }
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Selected cipher: 0x%x", cipher);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}

CAResult_t CAinitiateSslHandshake(const CAEndpoint_t *endpoint)
{
    CAResult_t res = CA_STATUS_OK;
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG, "Param endpoint is NULL" , CA_STATUS_INVALID_PARAM);
    ca_mutex_lock(g_sslContextMutex);
    if (NULL == InitiateTlsHandshake(endpoint))
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "TLS handshake failed");
        res = CA_STATUS_FAILED;
    }
    ca_mutex_unlock(g_sslContextMutex);
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return res;
}
/**
 * Expands the secret into blocks of data according
 * to the algorithm specified in section 5 of RFC 4346
 *
 * This function writes upto @p bufLen bytes into the given output buffer @p buf
 *
 * @param  key    secret key.
 * @param  keyLen    secret key length.
 * @param  label    A PRF label.
 * @param  labelLen     Actual length of @p label.
 * @param  random1    Random seed.
 * @param  random1Len     Actual length of @p random1 (may be zero).
 * @param  random2     Random seed.
 * @param  random2Len    Actual length of @p random2 (may be zero).
 * @param  buf    Output buffer for generated random data.
 * @param  bufLen    Maximum size of @p buf.
 *
 * @return The actual number of bytes written to @p buf or @c -1 on error.
 */

static int pHash (const unsigned char *key, size_t keyLen,
     const unsigned char *label, size_t labelLen,
     const unsigned char *random1, size_t random1Len,
     const unsigned char *random2, size_t random2Len,
     unsigned char *buf, size_t bufLen)
{
    unsigned char A[RANDOM_LEN] = {0};
    unsigned char tmp[RANDOM_LEN] = {0};
    size_t dLen;   /* digest length */
    size_t len = 0;   /* result length */

    VERIFY_NON_NULL_RET(key, NET_SSL_TAG, "key is NULL", -1);
    VERIFY_NON_NULL_RET(label, NET_SSL_TAG, "label is NULL", -1);
    VERIFY_NON_NULL_RET(random1, NET_SSL_TAG, "random1 is NULL", -1);
    VERIFY_NON_NULL_RET(random2, NET_SSL_TAG, "random2 is NULL", -1);
    VERIFY_NON_NULL_RET(buf, NET_SSL_TAG, "buf is NULL", -1);

    mbedtls_md_context_t hmacA;
    mbedtls_md_context_t hmacP;

    mbedtls_md_init(&hmacA);
    mbedtls_md_init(&hmacP);

    CHECK_MBEDTLS_RET(mbedtls_md_setup, &hmacA, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    CHECK_MBEDTLS_RET(mbedtls_md_setup, &hmacP, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

    CHECK_MBEDTLS_RET(mbedtls_md_hmac_starts, &hmacA, key, keyLen );
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacA, label, labelLen);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacA, random1, random1Len);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacA, random2, random2Len);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_finish, &hmacA, A);

    dLen = RANDOM_LEN;

    CHECK_MBEDTLS_RET(mbedtls_md_hmac_starts, &hmacP, key, keyLen);

    while (len + dLen < bufLen)
    {
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_reset, &hmacP);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_starts, &hmacP, key, keyLen);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, A, dLen);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, label, labelLen);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, random1, random1Len);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, random2, random2Len);

        CHECK_MBEDTLS_RET(mbedtls_md_hmac_finish, &hmacP, tmp);

        len += RANDOM_LEN;

        memcpy(buf, tmp, dLen);
        buf += dLen;

        CHECK_MBEDTLS_RET(mbedtls_md_hmac_reset, &hmacA);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_starts, &hmacA, key, keyLen);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacA, A, dLen);
        CHECK_MBEDTLS_RET(mbedtls_md_hmac_finish, &hmacA, A);
    }

    CHECK_MBEDTLS_RET(mbedtls_md_hmac_reset, &hmacP);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_starts, &hmacP, key, keyLen);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, A, dLen);

    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, label, labelLen);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, random1, random1Len);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_update, &hmacP, random2, random2Len);
    CHECK_MBEDTLS_RET(mbedtls_md_hmac_finish, &hmacP, tmp);

    memcpy(buf, tmp, bufLen - len);

    mbedtls_md_free(&hmacA);
    mbedtls_md_free(&hmacP);
    return bufLen;

exit:
    mbedtls_md_free(&hmacA);
    mbedtls_md_free(&hmacP);
    return -1;
}

CAResult_t CAsslGenerateOwnerPsk(const CAEndpoint_t *endpoint,
                            const uint8_t* label, const size_t labelLen,
                            const uint8_t* rsrcServerDeviceId, const size_t rsrcServerDeviceIdLen,
                            const uint8_t* provServerDeviceId, const size_t provServerDeviceIdLen,
                            uint8_t* ownerPsk, const size_t ownerPskSize)
{
    OIC_LOG_V(DEBUG, NET_SSL_TAG, "In %s", __func__);
    VERIFY_NON_NULL_RET(endpoint, NET_SSL_TAG, "endpoint is NULL", CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(label, NET_SSL_TAG, "label is NULL", CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(rsrcServerDeviceId, NET_SSL_TAG, "rsrcId is NULL", CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(provServerDeviceId, NET_SSL_TAG, "provId is NULL", CA_STATUS_INVALID_PARAM);
    VERIFY_NON_NULL_RET(ownerPsk, NET_SSL_TAG, "ownerPSK is NULL", CA_STATUS_INVALID_PARAM);

    // TODO: Added as workaround, need to debug
    ca_mutex_unlock(g_sslContextMutex);

    ca_mutex_lock(g_sslContextMutex);
    if (NULL == g_caSslContext)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Context is NULL");
        ca_mutex_unlock(g_sslContextMutex);
        OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
        return CA_STATUS_FAILED;
    }
    SslEndPoint_t * tep = GetSslPeer(endpoint);
    if (NULL == tep)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "Session does not exist");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }

    uint8_t keyblock[KEY_BLOCK_LEN] = {0};
    // "key expansion"
    uint8_t lab[] = {0x6b, 0x65, 0x79, 0x20, 0x65, 0x78, 0x70, 0x61, 0x6e, 0x73, 0x69, 0x6f, 0x6e};
    int ret = pHash(tep->master, sizeof(tep->master), lab, sizeof(lab),
                    (tep->random) + RANDOM_LEN, RANDOM_LEN, tep->random, RANDOM_LEN,
                    keyblock, KEY_BLOCK_LEN);
    if (-1 == ret)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "PSK not generated");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }
    ret = pHash(keyblock, sizeof(keyblock), label, labelLen,
                rsrcServerDeviceId, rsrcServerDeviceIdLen,
                provServerDeviceId, provServerDeviceIdLen,
                ownerPsk, ownerPskSize);
    if (-1 == ret)
    {
        OIC_LOG(ERROR, NET_SSL_TAG, "PSK not generated");
        ca_mutex_unlock(g_sslContextMutex);
        return CA_STATUS_FAILED;
    }

    ca_mutex_unlock(g_sslContextMutex);

    OIC_LOG_V(DEBUG, NET_SSL_TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}
