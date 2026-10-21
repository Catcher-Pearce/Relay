#include "Auth/Authenticator.h"
#include "TestFiles.h"
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

class AuthenticatorTest : public QObject {
    Q_OBJECT
    std::unique_ptr<test::Files> files;
    std::filesystem::path directory;
    Authenticator::Credentials create() { return Authenticator::ensureCredentials(directory, "test-peer"); }
private slots:
    void init() {
        files = std::make_unique<test::Files>();
        directory = files->root / "tls";
    }
    void cleanup() { files.reset(); }
    void trustedPeersPersistAndRemoveByUuid() {
        Authenticator auth(directory);
        QVERIFY(auth.loadTrustedPeers().empty());
        const std::string fingerprint(64, 'a');
        auth.addTrustedPeer("peer-one", "Laptop", fingerprint);
        auth.addTrustedPeer("peer-two", "Desktop", std::string(64, 'b'));
        Authenticator reloaded(directory);
        const auto peers = reloaded.loadTrustedPeers();
        QCOMPARE(peers.size(), std::size_t(2));
        QCOMPARE(peers.at("peer-one").deviceName, std::string("Laptop"));
        QCOMPARE(peers.at("peer-one").publicKeyFingerprint, fingerprint);
        QVERIFY(!peers.at("peer-one").approvedAt.empty());
        QVERIFY(reloaded.removeTrustedPeer("peer-one"));
        QVERIFY(!reloaded.removeTrustedPeer("peer-one"));
        QCOMPARE(auth.loadTrustedPeers().size(), std::size_t(1));
        QVERIFY(auth.loadTrustedPeers().contains("peer-two"));
    }
    void trustedPeersRejectChangedKeysAndInvalidFingerprints() {
        Authenticator auth(directory);
        auth.addTrustedPeer("peer", "Laptop", std::string(64, 'A'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            auth.addTrustedPeer("peer", "Imposter", std::string(64, 'b')));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, auth.addTrustedPeer("other", "Bad", "bad"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, auth.addTrustedPeer("", "Bad", std::string(64, 'a')));
        QCOMPARE(auth.loadTrustedPeers().at("peer").deviceName, std::string("Laptop"));
        QCOMPARE(auth.loadTrustedPeers().at("peer").publicKeyFingerprint, std::string(64, 'a'));
    }
    void malformedTrustStoreIsNotOverwritten() {
        Authenticator auth(directory);
        const auto path = directory / "trusted-peers.json";
        test::write(path, "{broken");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, auth.loadTrustedPeers());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, auth.addTrustedPeer("peer", "Laptop", std::string(64, 'a')));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)Authenticator{directory});
        QCOMPARE(test::read(path), std::string("{broken"));
    }
    void createsMatchingKeysAndSelfSignedTlsCertificate() {
        const auto paths = create();
        const auto privatePem = test::read(paths.privateKey);
        const auto publicPem = test::read(paths.publicKey);
        const auto certificatePem = test::read(paths.certificate);
        QVERIFY(privatePem.starts_with("-----BEGIN PRIVATE KEY-----"));
        QVERIFY(publicPem.starts_with("-----BEGIN PUBLIC KEY-----"));
        QVERIFY(certificatePem.starts_with("-----BEGIN CERTIFICATE-----"));
        std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
            BIO_new_mem_buf(privatePem.data(), static_cast<int>(privatePem.size())), BIO_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> certBio(
            BIO_new_mem_buf(certificatePem.data(), static_cast<int>(certificatePem.size())), BIO_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free);
        QVERIFY(key && certificate);
        QCOMPARE(EVP_PKEY_is_a(key.get(), "EC"), 1);
        QCOMPARE(EVP_PKEY_get_bits(key.get()), 256);
        QCOMPARE(X509_check_private_key(certificate.get(), key.get()), 1);
        QCOMPARE(X509_verify(certificate.get(), key.get()), 1);
        QCOMPARE(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_SERVER, 0), 1);
        QCOMPARE(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_CLIENT, 0), 1);
        QVERIFY(X509_cmp_current_time(X509_get0_notBefore(certificate.get())) < 0);
        QVERIFY(X509_cmp_current_time(X509_get0_notAfter(certificate.get())) > 0);
        QCOMPARE(X509_check_ca(certificate.get()), 0);
        // Validate that OpenSSL's actual TLS context accepts the saved files.
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        QVERIFY(context);
        const auto certPath = paths.certificate.string(), keyPath = paths.privateKey.string();
        QCOMPARE(SSL_CTX_use_certificate_chain_file(context.get(), certPath.c_str()), 1);
        QCOMPARE(SSL_CTX_use_PrivateKey_file(context.get(), keyPath.c_str(), SSL_FILETYPE_PEM), 1);
        QCOMPARE(SSL_CTX_check_private_key(context.get()), 1);
    }
    void repeatedStartupPreservesAllFiles() {
        const auto paths = create();
        const auto key = test::read(paths.privateKey), pub = test::read(paths.publicKey), cert = test::read(paths.certificate);
        const auto modified = std::filesystem::last_write_time(paths.privateKey);
        create();
        QCOMPARE(test::read(paths.privateKey), key);
        QCOMPARE(test::read(paths.publicKey), pub);
        QCOMPARE(test::read(paths.certificate), cert);
        QVERIFY(std::filesystem::last_write_time(paths.privateKey) == modified);
    }
    void missingPublicKeyIsRecreatedWithoutReplacingPrivateKey() {
        const auto paths = create();
        const auto key = test::read(paths.privateKey), pub = test::read(paths.publicKey);
        std::filesystem::remove(paths.publicKey);
        create();
        QCOMPARE(test::read(paths.privateKey), key);
        QCOMPARE(test::read(paths.publicKey), pub);
    }
    void missingCertificateIsRecreatedWithoutReplacingKeys() {
        const auto paths = create();
        const auto key = test::read(paths.privateKey), pub = test::read(paths.publicKey);
        std::filesystem::remove(paths.certificate);
        create();
        QCOMPARE(test::read(paths.privateKey), key);
        QCOMPARE(test::read(paths.publicKey), pub);
        QVERIFY(std::filesystem::exists(paths.certificate));
    }
    void missingPrivateKeyDoesNotReplaceIdentity() {
        const auto paths = create();
        const auto certificate = test::read(paths.certificate);
        std::filesystem::remove(paths.privateKey);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
        QVERIFY(!std::filesystem::exists(paths.privateKey));
        QCOMPARE(test::read(paths.certificate), certificate);
    }
    void corruptPrivateKeyIsNotOverwritten() {
        const auto paths = create();
        test::write(paths.privateKey, "broken");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
        QCOMPARE(test::read(paths.privateKey), std::string("broken"));
    }
    void mismatchedPublicKeyIsRejected() {
        const auto paths = create();
        const auto other = Authenticator::ensureCredentials(files->root / "other", "other-peer");
        const auto privateKey = test::read(paths.privateKey);
        test::write(paths.publicKey, test::read(other.publicKey));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
        QCOMPARE(test::read(paths.privateKey), privateKey);
    }
    void mismatchedCertificateIsRejected() {
        const auto paths = create();
        const auto other = Authenticator::ensureCredentials(files->root / "other", "other-peer");
        test::write(paths.certificate, test::read(other.certificate));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
    }
    void changedDeviceIdentityIsRejected() {
        create();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Authenticator::ensureCredentials(directory, "different-peer"));
    }
    void expiredCertificateIsRejectedWithoutReplacingIt() {
        const auto paths = create();
        const auto certPem = test::read(paths.certificate), keyPem = test::read(paths.privateKey);
        std::unique_ptr<BIO, decltype(&BIO_free)> certInput(
            BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size())), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> keyInput(
            BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size())), BIO_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            PEM_read_bio_X509(certInput.get(), nullptr, nullptr, nullptr), X509_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_bio_PrivateKey(keyInput.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        QVERIFY(certificate && key);
        QVERIFY(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), -60));
        QVERIFY(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
        QCOMPARE(PEM_write_bio_X509(output.get(), certificate.get()), 1);
        char* data = nullptr;
        const auto length = BIO_get_mem_data(output.get(), &data);
        const std::string expired(data, static_cast<std::size_t>(length));
        test::write(paths.certificate, expired);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
        QCOMPARE(test::read(paths.certificate), expired);
        QCOMPARE(test::read(paths.privateKey), keyPem);
    }
    void invalidArgumentsDoNotCreateFiles() {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, Authenticator::ensureCredentials({}, "test-peer"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, Authenticator::ensureCredentials(directory, ""));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, Authenticator::ensureCredentials(directory, std::string(65, 'x')));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, Authenticator::ensureCredentials(directory, std::string("a\0b", 3)));
        QVERIFY(!std::filesystem::exists(directory));
    }
    void credentialSymlinksAreRejected() {
        std::filesystem::create_directory(directory);
        std::error_code error;
        std::filesystem::create_symlink(files->root / "missing", directory / "private-key.pem", error);
        if (error) QSKIP("Creating symlinks is not permitted on this system");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, create());
        QVERIFY(!std::filesystem::exists(files->root / "missing"));
    }
    void privateKeyPermissionsAreRestricted() {
#ifdef Q_OS_UNIX
        const auto paths = create();
        const auto permissions = std::filesystem::status(paths.privateKey).permissions();
        QVERIFY((permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
        QVERIFY((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                std::filesystem::perms::none);
#else
        QSKIP("POSIX permission bits do not describe Windows file ACLs");
#endif
    }
};
QTEST_GUILESS_MAIN(AuthenticatorTest)
#include "AuthenticatorTest.moc"
