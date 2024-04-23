/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
    useChunks = true;
}

std::vector<uint8_t>
    PackageSignature::getSignatureHeader(std::istream& package,
                                         uintmax_t sizeOfPkgWithoutSignHdr)
{
    package.seekg(0, std::ios_base::end);

    uintmax_t packageSize = package.tellg();

    if (sizeOfPkgWithoutSignHdr == packageSize)
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
        std::vector<uint8_t>& pkgSignData)
{
    PackageSignatureVersion version = getSignatureVersion(pkgSignData);

    switch (version)
    {
        case packageSignatureVersion1:
        case packageSignatureVersion2:
            lg2::error(
                "Parsing signature header failed, version {VERSION} is deprecated",
                "VERSION", version);
            throw InternalFailure();
        case packageSignatureVersion3:
        {
            return std::make_unique<PackageSignatureV3>(pkgSignData);
        }
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
                              const std::string& publicKey,
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
    std::unique_ptr<BIGNUM, decltype(&::BN_free)> ctxInputPtr{input,
                                                              &::BN_free};

    int inputLength = BN_hex2bn(&input, publicKey.c_str());
    inputLength = (inputLength + 1) / 2;

    std::vector<uint8_t> publicKeyBuffer(inputLength);
    BN_bn2bin(input, publicKeyBuffer.data());

    bo = BIO_new(BIO_s_mem());
    std::unique_ptr<BIO, decltype(&::BIO_free)> ctxBoPtr{bo, &::BIO_free};

    BIO_write(bo, publicKeyBuffer.data(), inputLength);
    vkey = PEM_read_bio_PUBKEY(bo, &vkey, nullptr, nullptr);
    std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> ctxVkeyPtr{
        vkey, &::EVP_PKEY_free};

    verctx = EVP_PKEY_CTX_new(vkey, NULL);
    if (!verctx)
    {

        lg2::error(
            "Verifying signature failed, cannot create a verify context");
        result = false;

        return result;
    }

    std::unique_ptr<EVP_PKEY_CTX, decltype(&::EVP_PKEY_CTX_free)> ctxVerctxPtr{
        verctx, &::EVP_PKEY_CTX_free};

    if (EVP_PKEY_verify_init(verctx) <= 0)
    {
        lg2::error(
            "Verifying signature failed, cannot initialize a verify context");
        result = false;

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

    return result;
}

void PackageSignatureV3::parseHeader()
{
    auto signatureHeader =
        reinterpret_cast<struct PldmSignatureHeaderInformationV3*>(
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

    if (version != packageSignatureVersion3)
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

    offsetToPublicKey = be16toh(signatureHeader->offsetToPublicKey);

    auto iteratorToPublicKeySize = packageSignData.begin() + offsetToPublicKey;
    publicKeySize = static_cast<uint16_t>(*iteratorToPublicKeySize) << 8 |
                    *(iteratorToPublicKeySize + 1);

    size_t publicKeyBegin =
        sizeof(PldmSignatureHeaderInformationV3) + pldmFwupPublicKeySizeLength;
    size_t publicKeyEnd = sizeof(PldmSignatureHeaderInformationV3) +
                          pldmFwupPublicKeySizeLength + publicKeySize;

    publicKeyData =
        std::vector<uint8_t>(packageSignData.begin() + publicKeyBegin,
                             packageSignData.begin() + publicKeyEnd);

    auto iteratorToSignatureSize = packageSignData.begin() + publicKeyEnd;

    signatureSize = static_cast<uint16_t>(*iteratorToSignatureSize) << 8 |
                    *(iteratorToSignatureSize + 1);

    if (signatureSize < signatureSha->minimumSignatureSize ||
        signatureSize > signatureSha->maximumSignatureSize)
    {
        lg2::error(
            "Parsing signature header failed, signatureSize={SIGNATURE_SIZE} is incorrect",
            "SIGNATURE_SIZE", signatureSize);
        throw InternalFailure();
    }

    size_t signatureBegin = offsetToSignature + pldmFwupSignatureSizeLength;
    size_t signatureEnd =
        offsetToSignature + pldmFwupSignatureSizeLength + signatureSize;

    signature = std::vector<uint8_t>(packageSignData.begin() + signatureBegin,
                                     packageSignData.begin() + signatureEnd);
}

uintmax_t PackageSignatureV3::calculateSizeOfSignedData(
    uintmax_t sizeOfPkgWithoutSignHdr)
{
    return sizeOfPkgWithoutSignHdr + sizeof(PldmSignatureHeaderInformationV3) +
           pldmFwupPublicKeySizeLength + publicKeySize;
}

bool PackageSignature::integrityCheck(std::istream& package,
                                      uintmax_t lengthOfSignedData)
{
    std::string publicKeyString(publicKeyData.begin(), publicKeyData.end());

    std::stringstream publicKeyStream;

    publicKeyStream << std::hex << std::setfill('0');
    for (size_t i = 0; publicKeyString.length() > i; ++i)
    {
        publicKeyStream << std::setw(2)
                        << static_cast<unsigned int>(
                               static_cast<unsigned char>(publicKeyString[i]));
    }

    return verify(package, publicKeyStream.str(), lengthOfSignedData);
}

std::vector<unsigned char>
    PackageSignatureSha384::calculateDigest(std::istream& package,
                                            uintmax_t lengthOfSignedData)
{
    package.seekg(0);

    if (useChunks)
    {
        std::vector<unsigned char> hash(digestLength);

        const EVP_MD* md = nullptr;
        EVP_MD_CTX* mdctx = nullptr;
        unsigned int mdLength = 0;

        md = EVP_get_digestbyname(digestName.c_str());
        if (md == nullptr)
        {
            lg2::error(
                "Parsing signature header failed, unknown digest name {DIGESTNAME}",
                "DIGESTNAME", digestName);
            throw InternalFailure();
        }

        mdctx = EVP_MD_CTX_new();
        std::unique_ptr<EVP_MD_CTX, decltype(&::EVP_MD_CTX_free)> ctxMdctxPtr{
            mdctx, &::EVP_MD_CTX_free};

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

            if (!EVP_DigestUpdate(
                    mdctx,
                    reinterpret_cast<const unsigned char*>(buffer.data()),
                    currentChunkSize))
            {
                lg2::error("Error in EVP_DigestUpdate");
                throw InternalFailure();
            }

            if (currentChunkSize < buffer.size())
                break;
        }

        if (!EVP_DigestFinal(mdctx, hash.data(), &mdLength))
        {
            lg2::error("Error in EVP_DigestFinal");
            throw InternalFailure();
        }

        return hash;
    }
    else
    {

        std::vector<uint8_t> packageVector;
        packageVector.resize(lengthOfSignedData);

        package.read(reinterpret_cast<char*>(&packageVector[0]),
                     lengthOfSignedData);

        std::vector<unsigned char> buffer(digestLength);

        unsigned char* digest =
            SHA384((const unsigned char*)packageVector.data(),
                   lengthOfSignedData, buffer.data());

        std::vector<unsigned char> hash(digest, digest + (int)digestLength);

        return hash;
    }
}

} // namespace fw_update
} // namespace pldm
