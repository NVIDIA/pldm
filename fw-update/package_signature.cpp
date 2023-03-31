#include "package_signature.hpp"

#include <endian.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace pldm
{

namespace fw_update
{

using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

PackageSignatureSha384::PackageSignatureSha384()
{
    minimumSignatureSize = minimumSignatureSizeSha384;
    maximumSignatureSize = maximumSignatureSizeSha384;

    digestLength = SHA384_DIGEST_LENGTH;
    digestName = packageSignatureSha384Name;
    useChunks = false;
}

std::vector<uint8_t>
    PackageSignature::getSignatureHeader(std::istream& package,
                                         uintmax_t sizeOfPkgWithoutSignHdr)
{
    package.seekg(0, std::ios_base::end);

    uintmax_t packageSize = package.tellg();

    if(sizeOfPkgWithoutSignHdr == packageSize)
    {
        lg2::info("Package Signature: package does not have Signature Header");
        return std::vector<uint8_t>();
    }

    if (sizeOfPkgWithoutSignHdr + pldmFwupSignaturePackageSize != packageSize)
    {
        lg2::error("Package Signature: invalid or corrupted package ");
        throw InternalFailure();
    }

    std::vector<uint8_t> pkgSignData;
    pkgSignData.resize(pldmFwupSignaturePackageSize);

    package.seekg(sizeOfPkgWithoutSignHdr);
    package.read(reinterpret_cast<char*>(&pkgSignData[0]), pkgSignData.size());

    return pkgSignData;
}

PackageSignatureVersion
    PackageSignature::getSignatureVersion(std::vector<uint8_t>& pkgSignData)
{
    return pkgSignData[pldmFwupSignatureMagicLength];
}

std::unique_ptr<PackageSignature>
    PackageSignature::createPackageSignatureParser(
        std::vector<uint8_t>& pkgSignData, const std::string& publicKey)
{
    PackageSignatureVersion version = getSignatureVersion(pkgSignData);

    switch (version)
    {
        case packageSignatureVersion1:
            lg2::error(
                "Parsing signature header failed, version {VERSION} is deprecated",
                "VERSION", packageSignatureVersion1);
            throw InternalFailure();
        case packageSignatureVersion2:
            return std::make_unique<PackageSignatureV2>(pkgSignData, publicKey);
        default:
        {
            lg2::error(
                "Parsing signature header failed, not supported version {VERSION}",
                "VERSION", version);
            throw InternalFailure();
        }
    }

    return nullptr;
}

bool PackageSignature::verify(std::istream& package,
                              uintmax_t lengthOfSignedData)
{
    int verificationErrorCode;
    bool result = true;

    auto digestVector =
        signatureSha->calculateDigest(package, lengthOfSignedData);

    // Context and key
    EVP_PKEY_CTX* verctx = NULL;
    EVP_PKEY* vkey = NULL;
    BIGNUM* input = NULL;
    BIO* bo = NULL;

    input = BN_new();
    int inputLength = BN_hex2bn(&input, publicKey.c_str());
    inputLength = (inputLength + 1) / 2;

    std::vector<uint8_t> publicKeyBuffer(inputLength);
    BN_bn2bin(input, publicKeyBuffer.data());

    bo = BIO_new(BIO_s_mem());
    BIO_write(bo, publicKeyBuffer.data(), inputLength);
    vkey = PEM_read_bio_PUBKEY(bo, &vkey, nullptr, nullptr);

    verctx = EVP_PKEY_CTX_new(vkey, NULL);
    if (!verctx)
    {
        lg2::error(
            "Verifying signature failed, cannot create a verify context");
        result = false;

        EVP_PKEY_free(vkey);
        BN_free(input);
        BIO_free(bo);
        return result;
    }

    if (EVP_PKEY_verify_init(verctx) <= 0)
    {
        lg2::error(
            "Verifying signature failed, cannot initialize a verify context");
        result = false;

        EVP_PKEY_CTX_free(verctx);
        EVP_PKEY_free(vkey);
        BN_free(input);
        BIO_free(bo);
        return result;
    }

    verificationErrorCode =
        EVP_PKEY_verify(verctx, signature.data(), signature.size(),
                        digestVector.data(), signatureSha->digestLength);

    if (verificationErrorCode != 1)
    {
        lg2::error(
            "Verifying signature failed, EVP_PKEY_verify error {VERIFICATION_ERR_CODE}",
            "VERIFICATION_ERR_CODE", verificationErrorCode);

        result = false;
    }

    EVP_PKEY_CTX_free(verctx);
    EVP_PKEY_free(vkey);
    BN_free(input);
    BIO_free(bo);
    
    return result;
}

uintmax_t PackageSignatureV2::calculateSizeOfSignedData(
    uintmax_t sizeOfPkgWithoutSignHdr)
{
    return sizeOfPkgWithoutSignHdr + sizeof(PldmSignatureHeaderInformationV2) -
           pldmFwupSignatureLength;
}

void PackageSignatureV2::parseHeader()
{
    auto signatureHeader =
        reinterpret_cast<struct PldmSignatureHeaderInformationV2*>(
            packageSignData.data());

    constexpr std::array<uint8_t, pldmFwupSignatureMagicLength>
        hdrSignatureMagic{0x5F, 0x32, 0xCB, 0x08};

    uint8_t* magic = reinterpret_cast<uint8_t*>(&(signatureHeader->magic));
    uint8_t* endOfMagic = magic + pldmFwupSignatureMagicLength;

    if (!std::equal(magic, endOfMagic, hdrSignatureMagic.begin(),
                    hdrSignatureMagic.end()))
    {
        lg2::error(
            "Parsing signature header failed, Signature Header does not contain PackageSignatureIdentifier");
        throw InternalFailure();
    }

    version = signatureHeader->majorVersion;

    if (version != packageSignatureVersion2)
    {
        lg2::error(
            "Parsing signature header failed, version={VERSION} is not supported",
            "VERSION", version);
        throw InternalFailure();
    }

    minorVersion = signatureHeader->minorVersion;
    securityVersion = signatureHeader->securityVersion;

    offsetToSignature = be16toh(signatureHeader->offsetToSignature);

    payloadSize = be32toh(signatureHeader->payloadSize);

    signatureType = signatureHeader->signatureType;

    if (signatureType != 0)
    {
        lg2::error(
            "Parsing signature header failed, signatureType={SIGNATURE_TYPE} is not supported",
            "SIGNATURE_TYPE", signatureType);
        throw InternalFailure();
    }

    signatureSize = be16toh(signatureHeader->signatureSize);

    if (signatureSize < signatureSha->minimumSignatureSize ||
        signatureSize > signatureSha->maximumSignatureSize)
    {
        lg2::error(
            "Parsing signature header failed, signatureSize={SIGNATURE_SIZE} is incorrect",
            "SIGNATURE_SIZE", signatureSize);
        throw InternalFailure();
    }

    size_t signatureBegin = sizeof(PldmSignatureHeaderInformationV2);
    size_t signatureEnd =
        sizeof(PldmSignatureHeaderInformationV2) + signatureSize;

    signature = std::vector<uint8_t>(packageSignData.begin() + signatureBegin,
                                     packageSignData.begin() + signatureEnd);
}

std::vector<unsigned char>
    PackageSignatureShaBase::calculateDigest(std::istream& package,
                                          uintmax_t lengthOfSignedData)
{
    package.seekg(0);
    unsigned char* digest;

    if (useChunks)
    {
        const EVP_MD* md = NULL;
        EVP_MD_CTX* mdctx = NULL;
        unsigned int mdLength;

        auto cleanupFunc = [&](EVP_MD_CTX* mdctx) {
            EVP_MD_CTX_free(mdctx);
            OPENSSL_free(digest);
            EVP_cleanup();
        };
        std::unique_ptr<EVP_MD_CTX, decltype(cleanupFunc)> ctxPtr(mdctx,
                                                                  cleanupFunc);

        md = EVP_get_digestbyname(digestName.c_str());
        if (md == NULL)
        {
            lg2::error(
                "Parsing signature header failed, unknown digest name {DIGESTNAME}",
                "DIGESTNAME", digestName);
            throw InternalFailure();
        }

        mdctx = EVP_MD_CTX_new();
        digest = reinterpret_cast<unsigned char*>(OPENSSL_malloc(digestLength));

        if (!EVP_DigestInit_ex(mdctx, md, NULL))
        {
            lg2::error(
                "Parsing signature header failed, message digest initialization failed");
            throw InternalFailure();
        }

        std::vector<uint8_t> buffer(chunkSize, 0);

        int chunkNumber = 0;
        size_t currentChunkSize = 0;

        while (!package.eof())
        {
            chunkNumber++;
            package.read(reinterpret_cast<char*>(buffer.data()), buffer.size());

            if ((chunkNumber * buffer.size()) <= lengthOfSignedData)
            {
                currentChunkSize = buffer.size();
            }
            else
            {
                currentChunkSize =
                    lengthOfSignedData - ((chunkNumber - 1) * buffer.size());
            }

            EVP_DigestUpdate(
                mdctx, reinterpret_cast<const unsigned char*>(buffer.data()),
                currentChunkSize);

            if (currentChunkSize < buffer.size())
                break;
        }

        EVP_DigestFinal(mdctx, reinterpret_cast<unsigned char*>(digest),
                        &mdLength);

        std::vector<unsigned char> digestVector(digest, digest + digestLength);

        return digestVector;
    }

    std::vector<uint8_t> testVector;
    testVector.resize(lengthOfSignedData);

    package.read(reinterpret_cast<char*>(&testVector[0]), lengthOfSignedData);

    unsigned char* bufferDigest = (unsigned char*)OPENSSL_malloc(digestLength);

    auto cleanupFunc = [](unsigned char* bufferDigest) {
        OPENSSL_free(bufferDigest);
        EVP_cleanup();
    };

    std::unique_ptr<unsigned char, decltype(cleanupFunc)> ctxPtr(bufferDigest,
                                                                 cleanupFunc);

    digest = SHA384((const unsigned char*)testVector.data(), lengthOfSignedData,
                    bufferDigest);

    std::vector<unsigned char> digestVector(digest, digest + (int)digestLength);

    return digestVector;
}

} // namespace fw_update
} // namespace pldm
