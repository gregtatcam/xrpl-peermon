
#define VERSION "1.31"
#include <ripple/basics/strHex.h>
#include <ripple/basics/UnorderedContainers.h>
#include <ripple/json/json_value.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STValidation.h>
#include <sodium.h>

#include <boost/regex.hpp>
#include <boost/tokenizer.hpp>

#include <iostream>
#include <string_view>
#include <string>
#include <string.h>
#include <time.h>
#include <chrono>
#include <optional>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h> 

#include <stdio.h>
#include <secp256k1.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "libbase58.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <stdint.h>

#include <set>
#include <map>
#include <utility>

#include "ripple.pb.h"

#include "stlookup.h"
#include <netdb.h>
#include <pwd.h>

#include "sha-256.h"
#include "xd.h"
#define DEBUG 1
#define PACKET_STACK_BUFFER_SIZE 2048
//#define PACKET_STACK_BUFFER_SIZE 10

#define TLS_LISTEN_CERT "listen.cert" 
#define TLS_LISTEN_KEY "listen.key"

std::string peer;
time_t time_start;

char* host;
std::unordered_map<int, bool> messagesToLog;

ripple::hash_map<ripple::PublicKey, ripple::PublicKey> signingToMasterKeys;

void print_cert_help_then_exit()
{
    fprintf(stderr, "openssl req  -nodes -new -x509  -keyout %s -out %s\n", TLS_LISTEN_KEY, TLS_LISTEN_CERT);
    exit(1);
}

ripple::uint256
proposalUniqueId(
        ripple::uint256 const& proposeHash,
        ripple::uint256 const& previousLedger,
        std::uint32_t proposeSeq,
        ripple::NetClock::time_point closeTime,
        ripple::Slice const& publicKey,
        ripple::Slice const& signature)
{
    ripple::Serializer s(512);
    s.addBitString(proposeHash);
    s.addBitString(previousLedger);
    s.add32(proposeSeq);
    s.add32(closeTime.time_since_epoch().count());
    s.addVL(publicKey);
    s.addVL(signature);

    return s.getSHA512Half();
}

void
parseManifests(
    ripple::Slice s)
{
    if (s.empty())
        return;

    static ripple::SOTemplate const manifestFormat{
            // A manifest must include:
            // - the master public key
            {ripple::sfPublicKey, ripple::soeREQUIRED},

            // - a signature with that public key
            {ripple::sfMasterSignature, ripple::soeREQUIRED},

            // - a sequence number
            {ripple::sfSequence, ripple::soeREQUIRED},

            // It may, optionally, contain:
            // - a version number which defaults to 0
            {ripple::sfVersion, ripple::soeDEFAULT},

            // - a domain name
            {ripple::sfDomain, ripple::soeOPTIONAL},

            // - an ephemeral signing key that can be changed as necessary
            {ripple::sfSigningPubKey, ripple::soeOPTIONAL},

            // - a signature using the ephemeral signing key, if it is present
            {ripple::sfSignature, ripple::soeOPTIONAL},
    };

    try
    {
        ripple::SerialIter sit{s};
        ripple::STObject st{sit, ripple::sfGeneric};

        st.applyTemplate(manifestFormat);

        if (st.isFieldPresent(ripple::sfVersion) && st.getFieldU16(ripple::sfVersion) != 0)
            return;

        auto const pk = st.getFieldVL(ripple::sfPublicKey);

        if (!ripple::publicKeyType(ripple::makeSlice(pk)))
            return;

        auto const masterKey = ripple::PublicKey(ripple::makeSlice(pk));
        if (!st.isFieldPresent(ripple::sfSigningPubKey))
            return;

        auto const spk = st.getFieldVL(ripple::sfSigningPubKey);

        if (!ripple::publicKeyType(ripple::makeSlice(spk)))
            return;

        auto const signingKey = ripple::PublicKey(ripple::makeSlice(spk));
        signingToMasterKeys.emplace(signingKey, masterKey);
    }
    catch (...)
    {
    }

    return;
}

// these are defaults set at startup according to cmdline flags
int use_cls = 1, no_dump = 0, slow = 0, manifests_only = 0, raw_hex = 0, no_stats = 0, no_http = 0, no_hex = 0;
int stricmp(const uint8_t* a, const uint8_t* b)
{
    int ca, cb;
    do {
        ca = (unsigned char) *a++;
        cb = (unsigned char) *b++;
        ca = tolower(toupper(ca));
        cb = tolower(toupper(cb));
    } while (ca == cb && ca != '\0');
    return ca - cb;
}

int strnicmp(const uint8_t* a, const uint8_t* b, int n)
{
    int ca, cb;
    do {
        ca = (unsigned char) *a++;
        cb = (unsigned char) *b++;
        ca = tolower(toupper(ca));
        cb = tolower(toupper(cb));
    } while (ca == cb && ca != '\0' && --n > 0);
    return ca - cb;
}

void print_sto(const std::string& st)
{
    // hacky way to add \0 to the end due to bug in deserializer
    uint8_t* input = malloc(st.size() + 1);
    int i = 0;
    for (unsigned char c : st)
        input[i++] = c;
    input[i] = '\0';

    uint8_t* output = 0;
    if (!deserialize(&output, input, st.size() + 1, 0, 0, 0))
        return fprintf(stderr, "Could not deserialize\n");
    printf("%s\n", output);
    free(output);
    free(input);
}

void print_hex(uint8_t* packet_buffer, int packet_len)
{
    if (no_hex)
        return;

    for (int j = 0; j < packet_len; j++)
    {
        if (j % 16 == 0 && !raw_hex)
            printf("0x%08X:\t", j);

        printf("%02X%s", packet_buffer[j],
            (raw_hex ? "" : 
            (j % 16 == 15 ? "\n" :
            (j % 4 == 3 ? "  " :
            (j % 2 == 1 ? " " : "")))));
    }
    printf("\n");
}

template<class... Args>
int printd(Args ... args)
{
    (std::cerr << ... << args) << "\n";
    return 1;
}

int connect_peer(std::string_view ip_port, int listen_mode)
{

    auto x = ip_port.find_last_of(":");
    if (x == std::string_view::npos)
        return printd("[DBG] Could not find port in ", ip_port), -1;

    std::string ip { ip_port.substr(0, x) };
    long port = strtol(ip_port.substr(x+1).data(), 0, 10);

    if (port <= 0)
        return printd("[DBG] Port of ", ip_port, " parsed to 0 or neg"), -1;    

    int sockfd = 0;
    struct sockaddr_in serv_addr; 

    if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return printd("[DBG] Could not create socket for ", ip_port, "\n"), -1;

    memset(&serv_addr, '0', sizeof(serv_addr)); 

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port); 

    if(inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr)<=0)
        return printd("[DBG] Could not create socket for ", ip_port," inet_pton error occured"), -1;

    if (listen_mode)
    {
        if (access(TLS_LISTEN_CERT, F_OK) != 0)
        {
            fprintf(stderr, "Could not open ./%s\nTry: ", TLS_LISTEN_CERT);
            print_cert_help_then_exit();
        }

        if (access(TLS_LISTEN_KEY, F_OK) != 0)
        {
            return fprintf(stderr, "Could not open ./%s\nTry: ", TLS_LISTEN_KEY);
            print_cert_help_then_exit();
        }

        if (bind(sockfd, (struct sockaddr *)&serv_addr , sizeof(serv_addr)) < 0)
            return fprintf(stderr, "Could not bind to ip and port\n");

        if (listen(sockfd, 1) < 0)
            return fprintf(stderr, "Could not listen on ip and port\n");

        printf("Waiting for an incoming connection on %s %d\n", ip.c_str(), port);
        struct sockaddr client_addr;
        int address_len = sizeof(client_addr);
        sockfd = accept(sockfd, &client_addr, &address_len);

        struct sockaddr_in* pV4Addr = (struct sockaddr_in*)&client_addr;
        struct in_addr ipAddr = pV4Addr->sin_addr;
        char str[INET_ADDRSTRLEN];
        for (int i = 0; i < INET_ADDRSTRLEN; ++i)
            str[0] = 0;

        inet_ntop( AF_INET, &ipAddr, str, INET_ADDRSTRLEN );

        printf("Received connection from: %s\n", str);

        // fall through
    }
    else
    {
        if(connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
            return fprintf(stderr, "Could not connect to ip and port\n");
    }

    return sockfd;
}


int generate_node_keys(
        secp256k1_context* ctx,
        unsigned char* outsec32,
        unsigned char* outpubraw64,
        unsigned char* outpubcompressed33,
        char* outnodekeyb58,
        size_t* outnodekeyb58size)
{
    
    // create secp256k1 context and randomize it
    int rndfd = open("/dev/urandom", O_RDONLY);

    if (rndfd < 0) {
        fprintf(stderr, "[FATAL] Could not open /dev/urandom for reading\n");
        exit(1);
    }

    unsigned char seed[32];
    auto result = read(rndfd, seed, 32);
    if (result != 32) {
        fprintf(stderr, "[FATAL] Could not read 32 bytes from /dev/urandom\n");
        exit(2);
    }



    if (!secp256k1_context_randomize(ctx, seed)) {
        fprintf(stderr, "[FATAL] Could not randomize secp256k1 context\n");
        exit(3);
    }

    // fixed key mode
    struct passwd *pw = getpwuid(getuid());
    const char *homedir = pw->pw_dir;

    char keyfn[256];
    strcpy(keyfn, homedir);
    strcat(keyfn, "/.peermon");

    int keyfd = open(keyfn, O_RDONLY);
    if (!(keyfd >= 0 && read(keyfd, outsec32, 32) == 32))
    {
        result = read(rndfd, outsec32, 32);
        if (result != 32) {
            fprintf(stderr, "[FATAL] Could not read 32 bytes from /dev/urandom\n");
            exit(4);
        }
    }

    secp256k1_pubkey* pubkey = (secp256k1_pubkey*)((void*)(outpubraw64));

    if (!secp256k1_ec_pubkey_create(ctx, pubkey, (const unsigned char*)outsec32)) {
        fprintf(stderr, "[FATAL] Could not generate secp256k1 keypair\n");
        exit(5);
    }

    size_t out_size = 33;
    secp256k1_ec_pubkey_serialize(ctx, outpubcompressed33, &out_size, pubkey, SECP256K1_EC_COMPRESSED);

    unsigned char outpubcompressed38[38];

    // copy into the 38 byte check version
    for(int i = 0; i < 33; ++i) outpubcompressed38[i+1] = outpubcompressed33[i];

    // clean up
    close(rndfd);

    // pub key must start with magic type 0x1C
    outpubcompressed38[0] = 0x1C;
    // generate the double sha256
    unsigned char hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, outpubcompressed38, 34);

    unsigned char hash2[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash2, hash, crypto_hash_sha256_BYTES);

    // copy checksum bytes to the end of the compressed key
    for (int i = 0; i < 4; ++i) 
        outpubcompressed38[34+i] = hash2[i];


    // generate base58 encoding
    b58enc(outnodekeyb58, outnodekeyb58size, outpubcompressed38, 38);

    outnodekeyb58[*outnodekeyb58size] = '\0';

}
//todo: clean up and optimise, check for overrun 
SSL* ssl_handshake_and_upgrade(secp256k1_context* secp256k1ctx, int fd, SSL_CTX** outctx, int listen_mode)
{
    const SSL_METHOD *method = 
        listen_mode ? SSLv23_server_method() : TLS_client_method(); 

    SSL_CTX *ctx = SSL_CTX_new(method);
    SSL_CTX_set_ecdh_auto(ctx, 1);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    if (listen_mode)
    {
        if (access(TLS_LISTEN_CERT, F_OK) != 0 ||
                SSL_CTX_use_certificate_file(ctx, TLS_LISTEN_CERT, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stderr);
            fprintf(stderr, "Could not open ./%s\n Try: ", TLS_LISTEN_CERT);
            print_cert_help_then_exit();
        }
        if (access(TLS_LISTEN_KEY, F_OK) != 0 ||
                SSL_CTX_use_PrivateKey_file(ctx, TLS_LISTEN_KEY, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stderr);
            fprintf(stderr, "Could not open ./%s\nTry: ", TLS_LISTEN_KEY);
            print_cert_help_then_exit();
        }        
    }

    *outctx = ctx;

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd); 


    int status = -100;
    if (listen_mode)
        status = SSL_accept(ssl);
    else
        status = SSL_connect(ssl);

    if (status != 1)
    {
        status = SSL_get_error(ssl, status);
        fprintf(stderr, "[FATAL] SSL_connect failed with SSL_get_error code %d\n", status);
        return NULL;
    }    

    unsigned char buffer[1024];
    size_t len = SSL_get_finished(ssl, buffer, 1024);
    if (len < 12)
    {
        fprintf(stderr, "[FATAL] Could not SSL_get_finished\n");
        return NULL;
    }

    // SHA512 SSL_get_finished to create cookie 1
    unsigned char cookie1[64];
    crypto_hash_sha512(cookie1, buffer, len);
    
    len = SSL_get_peer_finished(ssl, buffer, 1024);
    if (len < 12) {
        fprintf(stderr, "[FATAL] Could not SSL_get_peer_finished\n");
        return NULL;
    }   
   
    // SHA512 SSL_get_peer_finished to create cookie 2
    unsigned char cookie2[64];
    crypto_hash_sha512(cookie2, buffer, len);

    // xor cookie2 onto cookie1
    for (int i = 0; i < 64; ++i) cookie1[i] ^= cookie2[i];

    // the first half of cookie2 is the true cookie
    crypto_hash_sha512(cookie2, cookie1, 64);

    // generate keys

    unsigned char sec[32], pub[64], pubc[33];
    char b58[100];
    size_t b58size = 100;
    generate_node_keys(secp256k1ctx, sec, pub, pubc, b58, &b58size);

    secp256k1_ecdsa_signature sig;
    secp256k1_ecdsa_sign(secp256k1ctx, &sig, cookie2, sec, NULL, NULL);
   
    unsigned char buf[200];
    size_t buflen = 200;
    secp256k1_ecdsa_signature_serialize_der(secp256k1ctx, buf, &buflen, &sig);

    char buf2[200];
    size_t buflen2 = 200;
    sodium_bin2base64(buf2, buflen2, buf, buflen, sodium_base64_VARIANT_ORIGINAL);
    buf2[buflen2] = '\0';


    char buf3[2048];
    size_t buf3len = 0;
    if (listen_mode)
    {
        //  we could read their incoming request first, but it probably doesn't matter full duplex ftw


        unsigned char buffer[PACKET_STACK_BUFFER_SIZE];
        size_t bufferlen = PACKET_STACK_BUFFER_SIZE;

        bufferlen = SSL_read(ssl, buffer, PACKET_STACK_BUFFER_SIZE); 
        if (bufferlen == 0)
        {
            fprintf(stderr, "Server stopped responding while we waited for upgrade request\n");
            exit(1);
        }

        buffer[bufferlen] = '\0';
        if (!no_http)
            printf("Received:\n%s", buffer);
        
        char* default_protocol = "RTXP/1.2";
        char* protocol = default_protocol;


        // hacky way to grab which protocol to upgrade to, RH TODO: make this sensible
        int find_comma = 0;
        for (int i = 0; i < bufferlen - 10; ++i) //`Upgrade: `
        {
            if (find_comma && (buffer[i] == ',' || buffer[i] == '\r'))
            {
                buffer[i] = '\0';
                break;
            }
            if (!find_comma && memcmp(buffer + i, "Upgrade: ", 9) == 0)
            {
                protocol = buffer + i + 9;
                find_comma = 1;
            }
        }
        

        buf3len = snprintf(buf3, 2047,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: %s\r\n"
            "Connect-As: Peer\r\n"
            "Server: rippled-1.8.0\r\n"
            "Crawl: private\r\n"
            "Public-Key: %s\r\n"
            "Session-Signature: %s\r\n\r\n", protocol, b58, buf2);

    }
    else
    {
        buf3len = snprintf(buf3, 2047, 
                "GET / HTTP/1.1\r\n"
                "User-Agent: rippled-1.8.0\r\n"
                "Upgrade: XRPL/2.0\r\n"
                "Connection: Upgrade\r\n"
                "Connect-As: Peer\r\n"
                "Crawl: private\r\n"
                "Session-Signature: %s\r\n"
                "Public-Key: %s\r\n\r\n", buf2, b58);

    }

    if (SSL_write(ssl, buf3, buf3len) <= 0) {
        fprintf(stderr, "[FATAL] Failed to write bytes to openssl fd\n");
        return NULL;
    }

    if (listen_mode)
    {
        // send a ping immediately after handshake

        /*
message TMPing
{
    enum pingType {
        ptPING = 0; // we want a reply
        ptPONG = 1; // this is a reply
    }
    required pingType type      = 1;
    optional uint32 seq         = 2; // detect stale replies, ensure other side is reading
    optional uint64 pingTime    = 3; // know when we think we sent the ping
    optional uint64 netTime     = 4;
}

*/
        uint8_t packet_buffer[512];
        int packet_len = sizeof(packet_buffer);

        protocol::TMPing ping;
        printf("%lu mtPING - sending out\n");
        ping.set_type(protocol::TMPing_pingType_ptPING);

        //unsigned char* buf = (unsigned char*) malloc(ping.ByteSizeLong());
        ping.SerializeToArray(packet_buffer, packet_len);

        uint32_t reply_len = packet_len;
        uint16_t reply_type = 3;

        // write reply header
        unsigned char header[6];
        header[0] = (reply_len >> 24) & 0xff;
        header[1] = (reply_len >> 16) & 0xff;
        header[2] = (reply_len >> 8) & 0xff;
        header[3] = reply_len & 0xff;
        header[4] = (reply_type >> 8) & 0xff;
        header[5] = reply_type & 0xff;
        SSL_write(ssl, header, 6);
        SSL_write(ssl, packet_buffer, packet_len);

    }

    return ssl;
}

const char* mtUNKNOWN = "mtUNKNOWN_PACKET";

const char* packet_name(
        int packet_type, bool padded = false)
{
    switch(packet_type)
    {
        case 2: return (padded ? "mtMANIFESTS               " : "mtMANIFESTS");
        case 3: return (padded ? "mtPING                    " : "mtPING");
        case 5: return (padded ? "mtCLUSTER                 " : "mtCLUSTER");
        case 15: return (padded ? "mtENDPOINTS               " : "mtENDPOINTS");
        case 30: return (padded ? "mtTRANSACTION             " : "mtTRANSACTION");
        case 31: return (padded ? "mtGET_LEDGER              " : "mtGET_LEDGER");
        case 32: return (padded ? "mtLEDGER_DATA             " : "mtLEDGER_DATA");
        case 33: return (padded ? "mtPROPOSE_LEDGER          " : "mtPROPOSE_LEDGER");
        case 34: return (padded ? "mtSTATUS_CHANGE           " : "mtSTATUS_CHANGE");
        case 35: return (padded ? "mtHAVE_SET                " : "mtHAVE_SET");
        case 41: return (padded ? "mtVALIDATION              " : "mtVALIDATION");
        case 42: return (padded ? "mtGET_OBJECTS             " : "mtGET_OBJECTS");
        case 50: return (padded ? "mtGET_SHARD_INFO          " : "mtGET_SHARD_INFO");
        case 51: return (padded ? "mtSHARD_INFO              " : "mtSHARD_INFO");
        case 52: return (padded ? "mtGET_PEER_SHARD_INFO     " : "mtGET_PEER_SHARD_INFO");
        case 53: return (padded ? "mtPEER_SHARD_INFO         " : "mtPEER_SHARD_INFO");
        case 54: return (padded ? "mtVALIDATORLIST           " : "mtVALIDATORLIST");
        case 55: return (padded ? "mtSQUELCH                 " : "mtSQUELCH");
        case 56: return (padded ? "mtVALIDATORLISTCOLLECTION " : "mtVALIDATORLISTCOLLECTION");
        case 57: return (padded ? "mtPROOF_PATH_REQ          " : "mtPROOF_PATH_REQ");
        case 58: return (padded ? "mtPROOF_PATH_RESPONSE     " : "mtPROOF_PATH_RESPONSE");
        case 59: return (padded ? "mtREPLAY_DELTA_REQ        " : "mtREPLAY_DELTA_REQ");
        case 60: return (padded ? "mtREPLAY_DELTA_RESPONSE   " : "mtREPLAY_DELTA_RESPONSE");
        case 61: return (padded ? "mtGET_PEER_SHARD_INFO_V2  " : "mtGET_PEER_SHARD_INFO_V2");
        case 62: return (padded ? "mtPEER_SHARD_INFO_V2      " : "mtPEER_SHARD_INFO_V2");
        case 63: return (padded ? "mtHAVE_TRANSACTIONS       " : "mtHAVE_TRANSACTIONS");
        case 64: return (padded ? "mtTRANSACTIONS            " : "mtTRANSACTIONS");        
        default: return (padded ? "mtUNKNOWN_PACKET          " : mtUNKNOWN);
    }
}


int32_t packet_id(char* packet_name)
{
    if (stricmp("mtMANIFESTS", packet_name) == 0) return 2;
    if (stricmp("mtPING", packet_name) == 0) return 3;
    if (stricmp("mtCLUSTER", packet_name) == 0) return 5;
    if (stricmp("mtENDPOINTS", packet_name) == 0) return 15;
    if (stricmp("mtTRANSACTION", packet_name) == 0) return 30;
    if (stricmp("mtGET_LEDGER", packet_name) == 0) return 31;
    if (stricmp("mtLEDGER_DATA", packet_name) == 0) return 32;
    if (stricmp("mtPROPOSE_LEDGER", packet_name) == 0) return 33;
    if (stricmp("mtSTATUS_CHANGE", packet_name) == 0) return 34;
    if (stricmp("mtHAVE_SET", packet_name) == 0) return 35;
    if (stricmp("mtVALIDATION", packet_name) == 0) return 41;
    if (stricmp("mtGET_OBJECTS", packet_name) == 0) return 42;
    if (stricmp("mtGET_SHARD_INFO", packet_name) == 0) return 50;
    if (stricmp("mtSHARD_INFO", packet_name) == 0) return 51;
    if (stricmp("mtGET_PEER_SHARD_INFO", packet_name) == 0) return 52;
    if (stricmp("mtPEER_SHARD_INFO", packet_name) == 0) return 53;
    if (stricmp("mtVALIDATORLIST", packet_name) == 0) return 54;
    if (stricmp("mtSQUELCH", packet_name) == 0) return 55;
    if (stricmp("mtVALIDATORLISTCOLLECTION", packet_name) == 0) return 56;
    if (stricmp("mtPROOF_PATH_REQ", packet_name) == 0) return 57;
    if (stricmp("mtPROOF_PATH_RESPONSE", packet_name) == 0) return 58;
    if (stricmp("mtREPLAY_DELTA_REQ", packet_name) == 0) return 59;
    if (stricmp("mtREPLAY_DELTA_RESPONSE", packet_name) == 0) return 60;
    if (stricmp("mtGET_PEER_SHARD_INFO_V2", packet_name) == 0) return 61;
    if (stricmp("mtPEER_SHARD_INFO_V2", packet_name) == 0) return 62;
    if (stricmp("mtHAVE_TRANSACTIONS", packet_name) == 0) return 63;
    if (stricmp("mtTRANSACTIONS", packet_name) == 0) return 64;
    return -1;
}

std::map<int, std::pair<uint64_t, uint64_t>> counters; // packet type => [ packet_count, total_bytes ];


void rpad(char* output, int padding_chars)
{
    int i = strlen(output);
    while (padding_chars -i > 0)
        output[i++] = ' ';
    output[i] = '\0';
}

void human_readable_double(double bytes, char* output, char* end)
{
    char* suffix[] = {"B", "K", "M", "G", "T"};
  	char length = sizeof(suffix) / sizeof(suffix[0]);
    int i = 0;
    while (bytes > 1024 && i < length - 1)
    {
        bytes /= 1024;
        i++;
	}
    sprintf(output, "%.02lf %s%s", bytes, suffix[i], (end ? end : ""), i);
}

void human_readable(uint64_t bytes, char* output, char* end)
{
    human_readable_double(bytes, output, end);
}

template <typename V>
std::string
qstr(V&& v)
{
    std::stringstream str;
    str << "\"" << v << "\"";
    return str.str();
}

void
dumpJson1()
{
    std::cout << "}" << std::endl;
}

template <typename Arg2, typename ... Args>
void
dumpJson1(std::string const& arg1, Arg2&& arg2, Args&& ... args)
{
    std::cout << ", " << arg1 << ":" << arg2;
    dumpJson1(args ...);
}

void
dumpJson1(int msgType, std::size_t packetLen)
{
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::cout << "{" << qstr("type") << ":" << msgType << ", "
        << qstr("msgsize") << ":" << packetLen << ", "
        << qstr("tstamp") << ":" << now << ", "
        << qstr("host") << ":" << qstr(host);
}

template <typename Arg2, typename ... Args>
void
dumpJson1(int msgType, std::size_t packetLen, std::string const& arg1, Arg2&& arg2, Args&& ... args)
{
    dumpJson1(msgType, packetLen);
    std::cout << ", " << arg1 << ":" << arg2;
    dumpJson1(args ...);
}

#define PAD 20

time_t last_print = 0;

std::set<int> show; // if this is set then only show these packets
std::set<int> hide; // if this is set then only show packets other than these packets, this is mut excl with above

template <typename Msg>
std::optional<Msg>
parse(int packet_type, unsigned char* packet_buffer, size_t packet_len)
{
    if (messagesToLog.find(packet_type) == messagesToLog.end())
        return std::nullopt;
    Msg m;
    bool success = m.ParseFromArray( packet_buffer, packet_len ) ;
    if (!success)
    {
        dumpJson1(packet_type, packet_len,
                 qstr("error"), qstr(std::string("failed to parse ") + packet_name(packet_type)));
        return std::nullopt;
    }
    return m;
}

void process_packet(
        SSL* ssl,
        int packet_type,
        unsigned char* packet_buffer,
        size_t packet_len,
        int compressed,
        uint32_t uncompressed_size)
{

    time_t time_now = time(NULL);
    int display = (slow ? 0 : 1);

    if (slow && time_now - last_print >= 5)
    {
        last_print = time_now;
        display = 1;
    }

    

    if (counters.find(packet_type) == counters.end())
        counters.emplace(std::pair<int, std::pair<uint64_t, uint64_t>>{packet_type, std::pair<uint64_t, uint64_t>{1,packet_len}});
    else
    {
        auto& p = counters[packet_type];
        p.first++;
        p.second += packet_len;
    }

    auto dumpJson = [&] (std::string const& arg1, auto&& arg2, auto ... args) {
        if (messagesToLog.find(packet_type) != messagesToLog.end())
            return dumpJson1(packet_type, packet_len, arg1, arg2, args ...);
    };

    auto dumpError = [&] (std::string const& error) {
        dumpJson(qstr("error"), qstr(error));
    };

    if (packet_type == 3) //mtPing
    {
        auto const ping = parse<protocol::TMPing>(packet_type, packet_buffer, packet_len);
        if (!ping)
            return;
        //if (!no_dump && display)
        //    printf("%lu mtPING - replying PONG\n");
        ping->set_type(protocol::TMPing_pingType_ptPONG);

        //unsigned char* buf = (unsigned char*) malloc(ping.ByteSizeLong());
        ping->SerializeToArray(packet_buffer, packet_len);

        dumpJson(qstr("seq"), ping->seq(),
                 qstr("pingtime"), ping->pingtime(), qstr("nettime"), ping->nettime());

        uint32_t reply_len = packet_len;
        uint16_t reply_type = 3;

        // write reply header
        unsigned char header[6];
        header[0] = (reply_len >> 24) & 0xff;
        header[1] = (reply_len >> 16) & 0xff;
        header[2] = (reply_len >> 8) & 0xff;
        header[3] = reply_len & 0xff;
        header[4] = (reply_type >> 8) & 0xff;
        header[5] = reply_type & 0xff;
        SSL_write(ssl, header, 6);
        SSL_write(ssl, packet_buffer, packet_len);

        return;
    }

    //using namespace std::chrono;
    //auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    if (!no_dump && display &&
            ((show.size() > 0 && show.find(packet_type) != show.end()) ||
            (hide.size() > 0 && hide.find(packet_type) == hide.end()) ||
            (show.size() == 0 && hide.size() == 0)))
    {
        switch (packet_type)
        {
            case 2: // mtMANIFESTS
            {
                auto const m = parse<protocol::TMManifests>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    auto const n = m->list_size();
                    for (int i = 0; i < n; ++i) {
                        auto &s = m->list().Get(i).stobject();
                        parseManifests(ripple::makeSlice(s));
                    }
                    dumpJson("size", n);
                }
                break;
            }
            case 5: // mtCLUSTER
            {
                auto const m = parse<protocol::TMCluster>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("sizenodes"), m->clusternodes_size(),
                             qstr("sizesources"), m->loadsources_size());
                }
                break;
            }
            case 15: // mtENDPOINTS
            {
                auto const m = parse<protocol::TMEndpoints>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("size"), m->endpoints_v2_size());
                }
                break;
            }
            case 30: // mtTRANSACTION
            {   //rawTransaction
                auto const m = parse<protocol::TMTransaction>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    const std::string &st = m->rawtransaction();
                    ripple::SerialIter sit(ripple::makeSlice(st));
                    ripple::STTx stx(sit);
                    ripple::uint256 txID = stx.getTransactionID();
                    dumpJson(qstr("txhash"), qstr(txID));
                }
                break;
            }
            case 31: // mtGET_LEDGER
            {
                auto const m = parse<protocol::TMGetLedger>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("itype"), m->itype());
                }
                break;
            }
            case 32: // mtLEDGER_DATA
            {
                auto const m = parse<protocol::TMLedgerData>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("hash"), ripple::uint256{m->ledgerhash()},
                             qstr("seq"), m->ledgerseq(), qstr("itype"), m->type());
                }
                break;
            }
            case 33: // mtPROPOSE_LEDGER
            {
                auto const m = parse<protocol::TMProposeSet>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    ripple::uint256 const proposeHash{m->currenttxhash()};
                    ripple::uint256 const prevLedger{m->previousledger()};
                    ripple::PublicKey const publicKey{ripple::makeSlice(m->nodepubkey())};
                    ripple::NetClock::time_point const closeTime{ripple::NetClock::duration{m->closetime()}};
                    auto const sig = ripple::makeSlice(m->signature());

                    auto const propID = proposalUniqueId(
                            proposeHash,
                            prevLedger,
                            m->proposeseq(),
                            closeTime,
                            publicKey.slice(),
                            sig);
                    dumpJson(qstr("validator"), qstr(ripple::makeSlice(m->nodepubkey())),
                             qstr("prophash"), qstr(propID));
                }
                break;
            }
            case 34: // mtSTATUS_CHANGE
            {
                auto const m = parse<protocol::TMStatusChange>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    if (m->has_newstatus())
                        dumpJson(qstr("newstatus"), m->newstatus());
                    else
                        dumpJson(qstr("newstatus"), m->newstatus());
                }
                break;
            }
            case 35: // mtHAVE_SET
            {
                auto const m = parse<protocol::TMHaveTransactionSet>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("status"), m->status(),
                             qstr("hash"), ripple::uint256(m->hash()));
                }
                break;
            }
            case 41: // mtVALIDATION
            {
                auto const m = parse<protocol::TMValidation>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    auto valID = ripple::sha512Half(ripple::makeSlice(m->validation()));
                    ripple::SerialIter sit(ripple::makeSlice(m->validation()));
                    ripple::STValidation stval(
                            std::ref(sit),
                            [&](ripple::PublicKey const &pk) {
                                if (signingToMasterKeys.find(pk) != signingToMasterKeys.end())
                                    return ripple::calcNodeID(signingToMasterKeys[pk]);
                                dumpJson(qstr("error"), qstr("manifest sigpk to master not found"),
                                         qstr("key"), qstr(ripple::Slice(pk)));
                                return ripple::NodeID{1};
                            },
                            false);
                    dumpJson(qstr("validator"), qstr(ripple::Slice(stval.getSignerPublic())),
                             qstr("valhash"), qstr(valID));
                }
                break;
            }
            case 42: // mtGET_OBJECTS
            {
                auto const m = parse<protocol::TMGetObjectByHash>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("type"), m->type(), qstr("query"), m->query(),
                             qstr("size"), m->objects_size());
                }
                break;
            }
            case 50: // mtGET_SHARD_INFO
            {
                dumpError("deprecated");
                break;
            }
            case 51: // mtSHARD_INFO
            {
                dumpError("deprecated");
                break;
            }
            case 52: // mtGET_PEER_SHARD_INFO
            {
                dumpError("deprecated");
                break;
            }
            case 53: // mtPEER_SHARD_INFO
            {
                dumpError("deprecated");
                break;
            }
            case 54: // mtVALIDATORLIST
            {
                auto const m = parse<protocol::TMValidatorList>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("version"), m->version());
                }
                break;
            }
            case 55: // mtSQUELCH
            {
                auto const m = parse<protocol::TMSquelch>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("squelch"), m->squelch(),
                             qstr("valhash"), ripple::uint256{m->validatorpubkey()});
                }
                break;
            }
            case 56: // mtVALIDATORLISTCOLLECTION
            {
                auto const m = parse<protocol::TMValidatorListCollection>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("version"), m->version());
                }
                break;
            }
            case 57: // mtPROOF_PATH_REQ
            {
                auto const m = parse<protocol::TMProofPathRequest>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("key"), ripple::uint256{m->key()},
                             qstr("ledger_hash"), ripple::uint256{m->ledgerhash()});
                }
                break;
            }
            case 58: // mtPROOF_PATH_RESPONSE
            {
                auto const m = parse<protocol::TMProofPathResponse>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("key"), ripple::uint256{m->key()},
                             qstr("ledger_hash"), ripple::uint256{m->ledgerhash()},
                             qstr("type"), m->type());
                }
                break;
            }
            case 59: // mtREPLAY_DELTA_REQ
            {
                auto const m = parse<protocol::TMReplayDeltaRequest>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("ledger_hash"), ripple::uint256{m->ledgerhash()});
                }
                break;
            }
            case 60: // mtREPLAY_DELTA_RESPONSE
            {
                auto const m = parse<protocol::TMReplayDeltaResponse>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("ledger_hash"), ripple::uint256{m->ledgerhash()});
                }
                break;
            }
            case 61: // mtGET_PEER_SHARD_INFO_V2
            {
                auto const m = parse<protocol::TMGetPeerShardInfoV2>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("peerchain_size"), m->peerchain_size(), qstr("relays"), m->relays());
                }
                break;
            }
            case 62: // mtPEER_SHARD_INFO_V2
            {
                auto const m = parse<protocol::TMPeerShardInfoV2>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("timestamp"), m->timestamp(), qstr("publickey"), m->publickey());
                }
                break;
            }
            case 63: // mtHAVE_TRANSACTIONS
            {
                auto const m = parse<protocol::TMHaveTransactions>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("size_hashes"), m->hashes_size());
                }
                break;
            }
            case 64: // mtTRANSACTIONS
            {
                auto const m = parse<protocol::TMTransactions>(packet_type, packet_buffer, packet_len);
                if (m)
                {
                    dumpJson(qstr("size_transactions"), m->transactions_size());
                }
                break;
            }
            default:
            {
                dumpError("unknown message");
            }
        }
    }
    
    // display logic
    if (display && !no_stats)
    {
        time_t time_elapsed = time_now - time_start;
        if (time_elapsed <= 0) time_elapsed = 1;

        printf(
            "XRPL-Peermon -- Connected to Peer: %s for %llu sec\n\n"
            "Packet                    Total               Per second          Total Bytes         Data rate       \n"
            "------------------------------------------------------------------------------------------------------\n"
            ,peer.c_str(), time_elapsed);

        double total_rate = 0;
        uint64_t total_packets = 0;
        uint64_t total_bytes = 0;

        for (int i = 0; i < 128; ++i)
        if (counters.find(i) != counters.end())
        {
            auto& p = counters[i];
            double bps = ((double)(p.second))/(double)(time_elapsed);
            double cps = ((double)(p.first))/(double)(time_elapsed);
            total_bytes += p.second;
            total_rate += p.second;
            total_packets += p.first;

            char bps_str[64];
            bps_str[0] = '\0';

            char bto_str[64];
            bto_str[0] = '\0';

            human_readable_double(bps, bps_str, "/s");
            human_readable(p.second, bto_str, 0);
            rpad(bto_str, PAD);

            char cou_str[64];
            cou_str[0] = '\0';
            sprintf(cou_str, "%llu", p.first);
            rpad(cou_str, PAD);

            char cps_str[64];
            cps_str[0] = '\0';
            sprintf(cps_str, "%g", cps);
            rpad(cps_str, PAD);

            printf("%s%s%s%s%s\n", packet_name(i, 1), cou_str, cps_str, bto_str, bps_str);
        }
        total_rate /= ((double)(time_elapsed));

        char bps_str[64];
        bps_str[0] = '\0';
        char bto_str[64];
        bto_str[0] = '\0';
        human_readable_double(total_rate, bps_str, "/s");
        human_readable(total_bytes, bto_str, 0);
        rpad(bto_str, PAD);
            
        char cou_str[64];
        cou_str[0] = '\0';
        sprintf(cou_str, "%llu", total_packets);
        rpad(cou_str, PAD);

        char cps_str[64];
        cps_str[0] = '\0';
        sprintf(cps_str, "%g", ((double)(total_packets))/((double)(time_elapsed)));
        rpad(cps_str, PAD);

        
        printf(        
            "------------------------------------------------------------------------------------------------------\n"
            "Totals                    %s%s%s%s\n\n\n",
            cou_str, cps_str, bto_str, bps_str);
        printf("Latest packet: %s [%d] -- %lu bytes\n", packet_name(packet_type, 0), packet_type, packet_len);
    }

    if (manifests_only && packet_type != 2)
    {
        printf("Quiting due to manifests-only flag\n");
        exit(0);
    }
   
    fflush(stdout); 

}

int print_usage(int argc, char** argv, char* message)
{
    fprintf(stderr, "XRPL Peer Monitor\nVersion: %s\nRichard Holland / XRPL-Labs\n", VERSION);
    if (message)
        fprintf(stderr, "Error: %s\n", message);
    else
        fprintf(stderr, "A tool to connect to a rippled node as a peer and monitor the traffic it produces\n");
    fprintf(stderr, "Usage: %s IP PORT [OPTIONS] [show:mtPACKET,... | hide:mtPACKET,...]\n", argv[0]);
    fprintf(stderr, "Options:\n"
            "\tslow\t\t- Only print at most once every 5 seconds. Will skip displaying most packets. Use for stats.\n"
            "\tno-cls\t\t- Don't clear the screen between printing. If you're after packet contents use this.\n"
            "\tno-dump\t\t- Don't dump any packet contents.\n"
            "\tno-stats\t- Don't produce stats.\n"
            "\tno-http\t\t- Don't output HTTP upgrade.\n"
            "\tmanifests-only\t- Only collect and print manifests then exit.\n"
            "\traw-hex\t\t- Print raw hex where appropriate instead of giving it line numbers and spacing.\n"
            "\tno-hex\t\t- Never print hex, only parsed / able-to-be-parsed STObjects or omit.\n"
            "\tmessages [m1,m2,m3-m4]\t\t- Monitor for the specified messages.\n"
            "\tlisten\t\t- experimental do not use.\n"
            );
    fprintf(stderr, "Show / Hide:\n"
            "\tshow:mtPACKET[,mtPACKET...]\t\t- Show only the packets in the comma seperated list (no spaces!)\n"
            "\thide:mtPACKET[,mtPACKET...]\t\t- Show all packets except those in the comma seperated list.\n"
           );
    fprintf(stderr, "Packet Types:\n"
    "\tmtMANIFESTS mtPING mtCLUSTER mtENDPOINTS mtTRANSACTION mtGET_LEDGER mtLEDGER_DATA mtPROPOSE_LEDGER\n"
    "\tmtSTATUS_CHANGE mtHAVE_SET mtVALIDATION mtGET_OBJECTS mtGET_SHARD_INFO mtSHARD_INFO mtGET_PEER_SHARD_INFO\n"
    "\tmtPEER_SHARD_INFO mtVALIDATORLIST mtSQUELCH mtVALIDATORLISTCOLLECTION mtPROOF_PATH_REQ mtPROOF_PATH_RESPONSE\n"
    "\tmtREPLAY_DELTA_REQ mtREPLAY_DELTA_RESPONSE mtGET_PEER_SHARD_INFO_V2 mtPEER_SHARD_INFO_V2 mtHAVE_TRANSACTIONS\n"
    "\tmtTRANSACTIONS\n");
    fprintf(stderr, "Keys:\n"
            "\tWhen connecting, peermon choses a random secp256k1 node key for itself.\n"
            "\tIf this is not the behaviour you want please place a binary 32 byte key file at ~/.peermon.\n");
    fprintf(stderr, "Example:\n"
        "\t%s r.ripple.com 51235 no-dump\t\t\t\t\t# display realtime stats for this node\n"
        "\t%s r.ripple.com 51235 no-cls no-stats show:mtGET_LEDGER\t\t# show only the GET_LEDGER packets"
        "\n", argv[0], argv[0]);
    return 1;
}

int fd_valid(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

int main(int argc, char** argv)
{

    if (argc < 3)
        return print_usage(argc, argv, NULL);

    int port = 0;

    int listen_mode = 0;

    if (sscanf(argv[2], "%lu", &port) != 1)
        return print_usage(argc, argv, "Invalid port");

    int ip[4];
    host = argv[1];
    if (sscanf(host, "%lu.%lu.%lu.%lu", ip, ip+1, ip+2, ip+3) != 4)
    {
        // try do a hostname lookup
        struct hostent* hn = gethostbyname(argv[1]);
        if (!hn)
            return print_usage(argc, argv, "Invalid IP/hostname (IPv4 ONLY)");
        
        struct in_addr** addr_list = (struct in_addr **)hn->h_addr_list;
        host = inet_ntoa(*addr_list[0]);
        if (sscanf(host, "%lu.%lu.%lu.%lu", ip, ip+1, ip+2, ip+3) != 4)
            return print_usage(argc, argv, "Invalid IP after resolving hostname (IPv4 ONLY)");
    }


    peer = std::string{host} + ":" + std::string{argv[2]};

    for (int i = 3; i < argc; ++i)
    {

        size_t len = strlen(argv[i]);
        char* opt = argv[i];
        if (strcmp(opt, "no-cls") == 0)
            use_cls = 0;
        else if (strcmp(opt, "listen") == 0)
            listen_mode = 1;
        else if (strcmp(opt, "no-dump") == 0)
            no_dump = 1;
        else if (strcmp(opt, "no-stats") == 0)
            no_stats = 1;
        else if (strcmp(opt, "no-http") == 0)
            no_http = 1;
        else if (strcmp(opt, "slow") == 0)
            slow = 1;
        else if (strcmp(opt, "manifests-only") == 0)
            manifests_only = 1;
        else if (strcmp(opt, "raw-hex") == 0)
        {
            if (no_hex)
                return fprintf(stderr, "Incompatible options: no-hex and raw-hex\n");
            raw_hex = 1;
        }
        else if (strcmp(opt, "no-hex") == 0)
        {
            if (raw_hex)
                return fprintf(stderr, "Incompatible options: no-hex and raw-hex\n");
            no_hex = 1;
        }
        else if (len > 5 && (memcmp(opt, "show:", 5) == 0 || memcmp(opt, "hide:", 5) == 0))
        {
            std::set<int>* showhide = NULL;
            if (len > 5 && memcmp(opt, "show:", 5) == 0)
            {
                if (hide.size() > 0)
                    return print_usage(argc, argv, "Choose either show or hide, not both.");
                opt += 5;
                showhide = &show;
            }
            else if (len > 5 && memcmp(opt, "hide:", 5) == 0)
            {
                if (show.size() > 0)
                    return print_usage(argc, argv, "Choose either show or hide, not both.");
                opt += 5;
                showhide = &hide;
            }

            char tmp[1024]; tmp[1023] = '\0';
            strncpy(tmp, opt, 1023);

            char* ptr = strtok(tmp, ",");
            while (ptr != NULL)
            {
                int packet = packet_id(ptr);
                if (packet == -1)
                    return fprintf(stderr, "Invalid packet type: `%s`\n", ptr);

                showhide->emplace(packet);
                ptr = strtok(NULL, ",");
            }
        }
        else if (strcmp(opt, "messages") == 0)
        {
            ++i;
            if (i == argc)
                return print_usage(argc, argv, "Invalid messages option");
            else
            {
                typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
                boost::char_separator<char> sep{","};
                tokenizer tok{std::string(argv[i]), sep};
                boost::regex rx("^(\\d+)\\-(\\d+)$");
                for (const auto& t: tok)
                {
                    boost::smatch match;
                    if (boost::regex_search(t, match, rx))
                    {
                        for (int i = std::stoi(match[1]); i <= std::stoi(match[2]); ++i)
                            if (strcmp(packet_name(i), mtUNKNOWN))
                                messagesToLog[i] = true;
                    }
                    else
                        messagesToLog[std::stoi(t)] = true;
                }
            }
        }
        else
            return print_usage(argc, argv, "Invalid option");
    }
    if (messagesToLog.size() == 0)
        return print_usage(argc, argv, "messages option must be specified");

    b58_sha256_impl = calc_sha_256; 
    if (sodium_init() < 0) {
        fprintf(stderr, "[FATAL] Could not init libsodium\n");
        exit(6);
    }

    SSL_library_init();
   
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    secp256k1_context* secp256k1ctx = secp256k1_context_create(
                SECP256K1_CONTEXT_VERIFY |
                SECP256K1_CONTEXT_SIGN) ;


    int fd = -1;
    fd = connect_peer(peer.c_str(), listen_mode);

    time_start = time(NULL);

    if (fd <= 0) {
        fprintf(stderr, "[FATAL] Could not connect\n"); // todo just return
        exit(11);
    }

    SSL_CTX* sslctx = NULL;

    SSL* ssl = ssl_handshake_and_upgrade(secp256k1ctx, fd, &sslctx, listen_mode);

    if (ssl == NULL) {
        fprintf(stderr, "[FATAL] Could not handshake\n");
        exit(12);
    }
 
    unsigned char buffer[PACKET_STACK_BUFFER_SIZE];
    size_t bufferlen = PACKET_STACK_BUFFER_SIZE;

    int pc = (listen_mode ? 1 : 0);
    while (fd_valid(fd)) {
        bufferlen = SSL_read(ssl, buffer, (pc == 0 ? PACKET_STACK_BUFFER_SIZE : 10)); 
        if (bufferlen == 0)
        {
            int status = SSL_get_error(ssl, bufferlen);
            fprintf(stderr, "SSL_get_error code %d\n", status);
            fprintf(stderr, "Server stopped responding\n");
            break;
        }

        buffer[bufferlen] = '\0';
        if (!pc) {
            //if (!no_http)
            //    printf("Received:\n%s", buffer);

            if (bufferlen >= sizeof("HTTP/1.1 503 Service Unavailable")-1 &&
                memcmp(buffer, "HTTP/1.1 503 Service Unavailable", sizeof("HTTP/1.1 503 Service Unavailable")-1) == 0)
            {
                fprintf(stderr, "Node reported Service Unavailable\n");
                return 2;
            }

            pc++;
            continue;
        }


        // check header version
//        if (buffer[0] >> 2 != 0) {
//            fprintf(stderr, "[FATAL] Peer sent packets we don't understand\n");
//            exit(13);
//        }

        // first 4 bytes are bigendian payload size
        uint32_t payload_size = 
            (buffer[0] << 24) + (buffer[1] << 16) + (buffer[2] << 8) + buffer[3];
        int compressed = payload_size >> 28U;
        
        if (compressed)
            payload_size &= 0x0FFFFFFFU;

        uint16_t packet_type = (buffer[4] << 8) + buffer[5];

        uint32_t uncompressed_size = payload_size;
        if (compressed)
            uncompressed_size = 
                (buffer[6] << 24) + (buffer[7] << 16) + (buffer[8] << 8) + buffer[9];

        int header_size = (compressed ? 10 : 6);

//        printf("HEADER: %02X%02X%02X%02X %02X%02X %02X%02X%02X%02X\n", buffer[0], buffer[1], buffer[2], buffer[3],
//                buffer[4], buffer[5],
//                buffer[6], buffer[7], buffer[8], buffer[9]);

        // the vast majority of packets will fit in the stack buffer, but for those which do not, we will read the rest into heap
        if (payload_size + header_size > bufferlen)
        {

//            printf("payload_size[%d] + header_size[%d] = %d, bufferlen = %d\n",
//                    payload_size, header_size, payload_size + header_size, bufferlen); 
            // incomplete packet, receive the rest into a heap buffer
            
            size_t total_read = bufferlen - header_size;

            unsigned char* heapbuf = (unsigned char*) malloc( payload_size );
            
            // inefficient copy
            for (size_t i = header_size; i < bufferlen; ++i)
                heapbuf[i - header_size] = buffer[i];


            while (total_read < payload_size)
            {
                size_t bytes_read = SSL_read(ssl, heapbuf + total_read, payload_size - total_read);
                if (bytes_read == 0)
                {
                    fprintf(stderr, "Error reading / disconnect\n");
                    exit(1);
                }
//                printf("Large message... read %d bytes of %d...\n", bytes_read, payload_size);
                total_read += bytes_read;
            }

//            printf("payload_size: %d  toal_read: %d\n", payload_size, total_read);

            process_packet(
                    ssl, packet_type, heapbuf, payload_size, compressed, uncompressed_size);
            
            free(heapbuf);

            continue;
        }

        process_packet(
                ssl, packet_type, buffer + header_size, bufferlen - header_size, compressed, uncompressed_size);
        
    }

    secp256k1_context_destroy(secp256k1ctx);
    SSL_free(ssl);
    SSL_CTX_free(sslctx);
    close(fd);

    return 0;
}
